// Tiered compilation (Core::set_tiering): every block is compiled cheaply
// first and again at tier 1 once it started threshold times. The guest must
// do exactly what it does untiered, run for run: a block promoted in the
// middle of its native loop, so many blocks asking at once that the table
// overflows, a PC the chain filter keeps out of the chain table, a block
// compiled again careful after its loop-bottom guard missed, and code that
// changed in memory after tier 0 read it (an untiered block keeps running
// the code it was compiled from, and so does its promotion); the same with
// WebAssembly's cold tier first; and three cases found in review (below).
#include "core.h"
#include "bcore_profile.h"
#include "remote_tier.h"
#include "../src/bb_translator.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

using namespace bcore;

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr uint16_t kAdd1 = 0x6408;  // R0 += 1
static constexpr uint16_t kAdd2 = 0x6410;  // R0 += 2
static uint16_t jump_s(uint32_t from, uint32_t to) { return uint16_t(0x2000 | (((to - from) >> 1) & 0xfff)); }

struct Run {
    uint32_t pc, packets, chain_packets, chain_blocks;
    bool operator==(const Run& o) const {
        return pc == o.pc && packets == o.packets && chain_packets == o.chain_packets && chain_blocks == o.chain_blocks;
    }
};

struct Outcome {
    std::vector<Run> runs;
    std::vector<uint8_t> state;  // CpuState up to the execution state
    std::vector<uint8_t> data;
    BcoreStats stats;  // not compared
    unsigned support_sets = 0;  // Core::copy_patch_support_sets, not compared
    bool operator==(const Outcome& o) const { return runs == o.runs && state == o.state && data == o.data; }
};

struct Case {
    const char* name;
    std::function<void(FlatMemory&)> load;
    // Before each run: what the host changes (the loop registers, the code).
    std::function<void(CpuState&, FlatMemory&, int)> before = [](CpuState&, FlatMemory&, int) {};
    uint32_t start;
    int runs;
    uint32_t budget;
    std::function<bool(uint32_t)> filter;
    // `before` does nothing: what the case does depends on its packets
    // alone, not on where its runs end (Mode::cold runs it to as many
    // packets as the untiered runs instead).
    bool same_any_run = false;
};

// How the guest's memory and chaining are set up, and whether tier 0 is
// made of stencils (Core::set_copy_patch, which needs the page table).
struct Mode {
    const char* name;
    bool paged = false;
    bool copy_patch = false;
    bool dry = false;
    bool count = true;
    // Core::set_copy_patch_reach: a block's helper jumps and exits no
    // further than this (0: the host's reach).
    size_t reach = 0;
    // Core::set_cold_threshold and set_cold_batch (WebAssembly's): the
    // first starts of a block run cold, then blocks are compiled `batch`
    // at a time, or fewer after `age` packets.
    uint32_t cold = 0;
    uint32_t batch = 1;
    uint64_t age = 0;
    // Core::set_llvm_on_demand: LLVM made by the first block that needs it.
    bool on_demand = false;
};

// background: -1 promotions on this thread; else on the worker, published
// after that many packets (0: as soon as the worker is done), which a build
// without threads has not (Core::kHasThreads): its cases leave the worker
// out, as they would run on this thread, the same as the ones without it.
// packets: run until that many packets ran, the last run's budget cut to
// end there, instead of c.runs runs (each run's outcome then not kept): a
// cold block (Mode::cold) chains to none, so its runs end elsewhere.
// remote: promotions compiled by a RemoteCompiler (Core::set_remote_tier)
// and delivered that many runs after they were asked for (-1: none), the
// latest asked first where reversed.
struct Remote {
    int lag = -1;
    bool reversed = false;
    // WebAssembly: the code delivered as a browser's host delivers it, the
    // module made of the object in Module.bcoreModules (CodeLoader::install).
    bool modules = false;
};
static uint64_t g_remote_installed = 0, g_remote_refused = 0;

#if defined(__EMSCRIPTEN__)
// The module of object in Module.bcoreModules under ticket, as a host puts
// it there; and how many modules the map holds.
static void put_module(uint32_t ticket, const std::vector<uint8_t>& object) {
    EM_ASM({
        Module["bcoreModules"] ??= new Map();
        Module["bcoreModules"].set($0 >>> 0, new WebAssembly.Module(HEAPU8.slice($1 >>> 0, ($1 >>> 0) + ($2 >>> 0))));
    }, ticket, object.data(), object.size());
}
static int modules_held() {
    return EM_ASM_INT({ return Module["bcoreModules"] ? Module["bcoreModules"].size : 0; });
}
#endif

static Outcome run(const Case& c, int opt, uint32_t threshold, int background = -1, const Mode& mode = {"raw"},
                   uint32_t size_ref = 0, bool light = false, uint64_t packets = 0, Remote remote = {}) {
    CpuState cpu{};
    FlatMemory mem(0, 0x4000, &cpu);
    if (mode.paged) mem.enable_page_table();
    c.load(mem);
    Core core(&cpu, &mem);
    core.set_chaining(true, mode.dry);
    core.set_count_chained(mode.count);
    core.set_tiering(threshold, size_ref);
    core.set_light_tier1(light);
    if (threshold) core.set_copy_patch(mode.copy_patch);
    core.set_copy_patch_reach(mode.reach);
    if (threshold && mode.cold) require(core.set_cold_threshold(mode.cold), "no cold tier");
    core.set_cold_batch(mode.batch, mode.age);
    if (background >= 0) require(core.set_tier_background(true, uint64_t(background)), "no worker");
    core.set_llvm_on_demand(mode.on_demand);
    struct Job {
        uint32_t ticket;
        std::vector<uint8_t> bytes;
        int due;
    };
    std::vector<Job> jobs;
    int at = 0;  // the run asking
    RemoteCompiler compiler;
    if (remote.lag >= 0)
        core.set_remote_tier([&](uint32_t ticket, std::vector<uint8_t> bytes) {
            jobs.push_back({ticket, std::move(bytes), at + 1 + remote.lag});
            return true;
        });
    auto deliver = [&](int r) {
        std::vector<Job> due;
        for (auto it = jobs.begin(); it != jobs.end();)
            if (it->due <= r) {
                due.push_back(std::move(*it));
                it = jobs.erase(it);
            } else {
                ++it;
            }
        if (remote.reversed) std::reverse(due.begin(), due.end());
        for (const Job& job : due) {
            RemoteCode code;
            const bool ok = compiler.compile(job.bytes.data(), job.bytes.size(), code);
            if (!ok) fprintf(stderr, "FAIL: a remote promotion did not compile: %s\n", code.why.c_str()), std::exit(1);
#if defined(__EMSCRIPTEN__)
            if (remote.modules) {
                put_module(job.ticket, code.object);
                core.deliver(job.ticket, ok, {}, code.name, code.fallthrough_pc);
                continue;
            }
#endif
            core.deliver(job.ticket, ok, code.object, code.name, code.fallthrough_pc);
        }
    };
    require(core.init(opt), "init");
    if (c.filter) core.set_chain_filter(c.filter);
    cec_init(); evt_init();
    cpu.lb[0] = cpu.lb[1] = 0x3f00;  // no loop bottom anywhere near the code
    Outcome out;
    uint64_t ran = 0;
    for (int r = 0; packets ? ran < packets : r < c.runs; ++r) {
        at = r;
        deliver(r);
        c.before(cpu, mem, r);
        cpu.chain_budget = packets ? uint32_t(std::min<uint64_t>(c.budget, packets - ran)) : c.budget;
        require(core.run(r ? cpu.pc : c.start), "run");
        ran += uint64_t(cpu.chain_packets) + cpu.packets;
        if (!packets) out.runs.push_back({cpu.pc, cpu.packets, cpu.chain_packets, cpu.chain_blocks});
        cpu.did_jump = false;
    }
    require(!packets || ran == packets, "the runs ended past the packets");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&cpu);
    out.state.assign(bytes, bytes + offsetof(CpuState, insn_len));
    out.data.assign(mem.raw(), mem.raw() + 0x4000);
    out.stats = core.stats();
    out.support_sets = core.copy_patch_support_sets();
    g_remote_installed += core.remote_stats().installed;
    g_remote_refused += core.remote_stats().refused;
    return out;
}

// A host whose memory callback throws: the exception reaches the caller of
// Core::run through generated code, tiered or not (found in review: block
// functions were once nounwind, and the process aborted instead).
class ThrowingMemory : public FlatMemory {
public:
    explicit ThrowingMemory(CpuState* cpu) : FlatMemory(0, 0x4000, cpu) {}
    bool throwing = false;
    uint32_t rawmem_limit() const override { return 0; }
    uint32_t read32(uint32_t addr) const override {
        if (throwing) throw std::runtime_error("host read32 failure");
        return FlatMemory::read32(addr);
    }
};

// The same where tier 0 is made of stencils, which have no unwind
// information: their helpers catch, and Core::run throws again. The load
// reaches the callback outside the page table's RAM.
class ThrowingPagedMemory : public FlatMemory {
public:
    explicit ThrowingPagedMemory(CpuState* cpu) : FlatMemory(0, 0x4000, cpu) { enable_page_table(); }
    bool throwing = false;
    uint32_t read32(uint32_t addr) const override {
        if (addr < 0x4000) return FlatMemory::read32(addr);
        if (throwing) throw std::runtime_error("host read32 failure");
        return addr;
    }
};

static void test_throwing_stencils() {
    // Tier 0 throughout (1000 starts are never reached), on this thread or
    // with the worker; and the load in a parallel packet's slot, one of the
    // packet's pieces.
    for (int mode : {1, 2, 3}) {
        if (mode == 2 && !Core::kHasThreads) continue;
        CpuState cpu{};
        ThrowingPagedMemory mem(&cpu);
        if (mode == 3) {
            const uint16_t packet[] = {0xcc04, 0x000a, 0x9003, 0x0000};  // R0 = R1 + R2 || R3 = [P0++] || NOP
            for (int i = 0; i < 4; ++i) mem.write16(0x100 + 2 * i, packet[i]);
            mem.write16(0x108, 0x2ffc);  // JUMP.S 0x100
        } else {
            mem.write16(0x100, 0x9000);  // R0 = [P0++]
            mem.write16(0x102, 0x2fff);  // JUMP.S 0x100
        }
        Core core(&cpu, &mem);
        core.set_chaining(true, false);
        core.set_tiering(1000);
        core.set_copy_patch(true);
        if (mode == 2) require(core.set_tier_background(true, 1), "no worker");
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        bool caught = false;
        for (int r = 0; r < 6 && !caught; ++r) {
            mem.throwing = r >= 3;
            cpu.dpregs[8] = 0x8000;
            cpu.chain_budget = 7;
            try {
                require(core.run(0x100), "run");
            } catch (const std::runtime_error&) {
                caught = true;
            }
            cpu.did_jump = false;
        }
        require(caught, "a memory callback's exception did not reach the host through stencils");
#if BCORE_ENABLE_PROFILE && BCORE_COPY_PATCH_TEST
        require(core.stats().copy_patch_blocks > 0, "no block was made of stencils");
#endif
    }
}

// Stencils turned off and on again while blocks made of them are in the
// cache: those run on, and the guest does what it does without stencils.
static void test_toggle_stencils() {
    auto once = [](bool stencils) {
        CpuState cpu{};
        FlatMemory mem(0, 0x4000, &cpu);
        mem.enable_page_table();
        for (uint32_t at = 0x100; at < 0x140; at += 4) {
            mem.write16(at, kAdd1);
            mem.write16(at + 2, jump_s(at + 2, at + 4 < 0x140 ? at + 4 : 0x100));
        }
        Core core(&cpu, &mem);
        core.set_chaining(true, false);
        core.set_tiering(1000);
        core.set_copy_patch(stencils);
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        std::vector<uint32_t> seen;
        for (int r = 0; r < 40; ++r) {
            if (stencils && r % 10 == 5) core.set_copy_patch(r % 20 != 5);
            cpu.chain_budget = 3;
            require(core.run(r ? cpu.pc : 0x100), "run");
            seen.push_back(cpu.pc);
            seen.push_back(cpu.dpregs[0]);
            cpu.did_jump = false;
        }
        return seen;
    };
    require(once(true) == once(false), "turning stencils off and on changed what ran");
}

// A host that hides its page table (to see every access through its
// callbacks): blocks compiled while it is hidden call them, with stencils
// or without (found in review: stencils inlined through the table noted
// earlier).
class HidingMemory : public FlatMemory {
public:
    explicit HidingMemory(CpuState* cpu) : FlatMemory(0, 0x4000, cpu) { enable_page_table(); }
    bool shown = true;
    mutable unsigned reads = 0;
    uint32_t rawmem_limit() const override { return 0; }
    uint8_t* const* page_table() const override { return shown ? FlatMemory::page_table() : nullptr; }
    uint32_t read32(uint32_t addr) const override {
        if (addr == 0x2000) ++reads;
        return FlatMemory::read32(addr);
    }
};

static void test_hidden_page_table() {
    for (bool stencils : {false, true}) {
        CpuState cpu{};
        HidingMemory mem(&cpu);
        mem.write16(0x100, 0x0010);  // RTS, compiled while the table is shown
        mem.write16(0x200, 0x9000);  // R0 = [P0++]
        mem.write16(0x202, 0x0010);  // RTS
        mem.write32(0x2000, 42);
        Core core(&cpu, &mem);
        core.set_tiering(1000);
        core.set_copy_patch(stencils);
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.rets = 0x300;
        require(core.run(0x100), "run");
        mem.shown = false;
        cpu.dpregs[8] = 0x2000;
        require(core.run(0x200), "run");
        require(mem.reads == 1 && cpu.dpregs[0] == 42, "a block compiled with the table hidden called the host");
    }
}

static void test_throwing_callback() {
    for (int opt : {0, 1, 2})
        for (int mode : {0, 1, 2}) {  // untiered; tiered; tiered on the worker
            if (mode == 2 && !Core::kHasThreads) continue;
            CpuState cpu{};
            ThrowingMemory mem(&cpu);
            mem.write16(0x100, 0x9000);  // R0 = [P0++]
            mem.write16(0x102, 0x2fff);  // JUMP.S 0x100
            Core core(&cpu, &mem);
            core.set_chaining(true, false);
            if (mode) core.set_tiering(1);
            if (mode == 2) require(core.set_tier_background(true, 1), "no worker");
            require(core.init(opt), "init");
            cec_init(); evt_init();
            cpu.lb[0] = cpu.lb[1] = 0x3f00;
            bool caught = false;
            for (int r = 0; r < 6 && !caught; ++r) {
                mem.throwing = r >= 3;  // after the promotions
                cpu.dpregs[8] = 0x200;
                cpu.chain_budget = 7;
                try {
                    require(core.run(0x100), "run");
                } catch (const std::runtime_error&) {
                    caught = true;
                }
                cpu.did_jump = false;
            }
            require(caught, "a memory callback's exception did not reach the host");
#if BCORE_ENABLE_PROFILE
            require(core.stats().promotions_dropped == 0, "a promotion was dropped");
#endif
        }
}

// A page table the Memory shows only after the first compilation (a host
// that counts every access first, say): a promotion compiled on the worker
// inlines through it too, so CpuState::page_table must hold it (found in
// review: it stayed null, and the promoted code read through null).
class LatePagesMemory : public FlatMemory {
public:
    explicit LatePagesMemory(CpuState* cpu) : FlatMemory(0, 0x4000, cpu) { enable_page_table(); }
    bool shown = false;
    uint32_t rawmem_limit() const override { return 0; }
    uint8_t* const* page_table() const override { return shown ? FlatMemory::page_table() : nullptr; }
};

static void test_late_page_table() {
    for (int background : {-1, 0, 1}) {
        if (background >= 0 && !Core::kHasThreads) continue;
        CpuState cpu{};
        LatePagesMemory mem(&cpu);
        mem.write16(0x100, 0x9000);  // R0 = [P0++]
        mem.write16(0x102, 0x2fff);  // JUMP.S 0x100
        mem.write32(0x200, 42);
        Core core(&cpu, &mem);
        core.set_tiering(2);
        if (background >= 0) require(core.set_tier_background(true, uint64_t(background)), "no worker");
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        for (int r = 0; r < 6; ++r) {
            if (r == 1) mem.shown = true;
            cpu.dpregs[8] = 0x200;
            require(core.run(0x100), "run");
            require(cpu.dpregs[0] == 42, "a promotion read the wrong word");
            cpu.did_jump = false;
        }
#if BCORE_ENABLE_PROFILE
        // Published at once when the worker is done: maybe not yet.
        require((background == 0 || core.stats().blocks_promoted > 0) && core.stats().promotions_dropped == 0,
                "the block was not promoted");
#endif
    }
}

// A promotion queued on the worker, then tiering turned off and the block
// compiled again (careful, after a loop-bottom guard miss): the promotion
// was of the code replaced, and must not replace the new code (found in
// review: it did, the guard missed again, and the second careful
// compilation's symbol was defined twice).
static void test_stale_promotion() {
    if (!Core::kHasThreads) return;
    auto once = [](bool tiered) {
        CpuState cpu{};
        FlatMemory mem(0, 0x4000, &cpu);
        mem.write16(0x100, kAdd1); mem.write16(0x102, kAdd1); mem.write16(0x104, jump_s(0x104, 0x100));
        Core core(&cpu, &mem);
        if (tiered) {
            core.set_tiering(1);
            require(core.set_tier_background(true, 20), "no worker");
        }
        require(core.init(2), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        require(core.run(0x100), "run");
        if (tiered) core.set_tiering(0);
        cpu.lt[0] = 0x100; cpu.lb[0] = 0x102;
        std::vector<Run> runs;
        for (int r = 0; r < 30; ++r) {
            cpu.lc[0] = 3;
            require(core.run(0x100), "a run after the block was compiled again failed");
            runs.push_back({cpu.pc, cpu.packets, cpu.chain_packets, cpu.chain_blocks});
            cpu.did_jump = false;
        }
        runs.push_back({cpu.dpregs[0], 0, 0, 0});
        return runs;
    };
    require(once(true) == once(false), "a stale promotion changed what ran");
}

// A block of more packets than size_ref asks to be compiled again only
// after as many times the threshold's starts as it has packets more
// (tier_threshold), whether tier 0 is made of stencils or by LLVM: 40
// packets with a threshold of 4 and size_ref 8 after 20 runs, without
// size_ref after 4, and a block of 8 after 4 either way.
static void test_size_ref() {
    struct Want {
        uint32_t packets, size_ref;
        int runs;
    };
    for (bool stencils : {false, true})
        for (const Want& w : {Want{40, 8, 20}, Want{40, 0, 4}, Want{8, 8, 4}, Want{8, 3, 10}}) {
            CpuState cpu{};
            FlatMemory mem(0, 0x4000, &cpu);
            mem.enable_page_table();
            for (uint32_t i = 0; i + 1 < w.packets; ++i) mem.write16(0x100 + 2 * i, kAdd1);
            mem.write16(0x100 + 2 * (w.packets - 1), 0x0010);  // RTS
            Core core(&cpu, &mem);
            core.set_tiering(4, w.size_ref);
            core.set_copy_patch(stencils);
            require(core.init(1), "init");
            cec_init(); evt_init();
            cpu.lb[0] = cpu.lb[1] = 0x3f00;
            for (int r = 1; r <= w.runs; ++r) {
                cpu.rets = 0x2000;
                require(core.run(0x100), "run");
                require(cpu.pc == 0x2000 && cpu.dpregs[0] == uint32_t(r) * (w.packets - 1), "the block's work");
#if BCORE_ENABLE_PROFILE
#if BCORE_COPY_PATCH_TEST
                if (stencils) require(core.stats().copy_patch_blocks == 1, "tier 0 not made of stencils");
#endif
                require(core.stats().blocks_promoted == (r == w.runs ? 1u : 0u),
                        r < w.runs ? "promoted before its threshold" : "not promoted at its threshold");
#endif
            }
        }
#if BCORE_ENABLE_PROFILE
    // More requests in one run than the table holds: the host looks at
    // every count, each against its own block's threshold. A ring of 300
    // one-packet blocks (threshold 2) and one of 3 packets (6): the second
    // time round, in one chained run, the 300 ask, and the larger block,
    // at 2, is not promoted with them.
    for (bool stencils : {false, true}) {
        CpuState cpu{};
        FlatMemory mem(0, 0x4000, &cpu);
        mem.enable_page_table();
        constexpr uint32_t kSmall = 300, kLarge = 0x100 + 2 * kSmall;
        for (uint32_t i = 0; i < kSmall; ++i) mem.write16(0x100 + 2 * i, jump_s(0x100 + 2 * i, 0x102 + 2 * i));
        mem.write16(kLarge, kAdd1); mem.write16(kLarge + 2, kAdd1); mem.write16(kLarge + 4, jump_s(kLarge + 4, 0x100));
        Core core(&cpu, &mem);
        core.set_chaining(true, false);
        core.set_tiering(2, 1);
        core.set_copy_patch(stencils);
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        uint32_t pc = 0x100;
        do {  // the first time round: every block compiled, a run each
            cpu.chain_budget = 1;
            require(core.run(pc), "run");
            pc = cpu.pc;
            cpu.did_jump = false;
        } while (pc != 0x100);
        require(core.stats().blocks_promoted == 0, "a block promoted at its first start");
        cpu.chain_budget = kSmall + 3;  // once round, chained
        require(core.run(0x100), "run");
        require(cpu.pc == 0x100 && core.stats().tier_request_overflows == 1, "no overflow");
        require(core.stats().blocks_promoted == kSmall, "the overflow promoted a block under its threshold");
    }
#endif
}

// A translation's result left untaken, as Core leaves a promotion it drops
// (its code read code the tier-0 block had not): the module goes before its
// context (found: it went after it, a use after free; ASan says so).
static void test_result_order() {
    BBTranslateResult result;
    result.context = std::make_unique<llvm::LLVMContext>();
    result.module = std::make_unique<llvm::Module>("dropped", *result.context);
}

// A build without threads (Core::kHasThreads): set_tier_background refuses
// a worker and sets nothing, no worker starts, and the promotions run on
// this thread as without it (found in review: the cases on the worker ran
// there, the build's tests none the wiser).
static int g_worker_starts = 0;

static void test_no_worker() {
    if (Core::kHasThreads) return;
    for (int publish_after : {0, 1, 50}) {
        CpuState cpu{};
        FlatMemory mem(0, 0x4000, &cpu);
        mem.write16(0x100, kAdd1); mem.write16(0x102, kAdd1); mem.write16(0x104, jump_s(0x104, 0x100));
        Core core(&cpu, &mem);
        core.set_tiering(1);
        require(!core.set_tier_background(true, uint64_t(publish_after), [] { ++g_worker_starts; }),
                "a worker in a build without threads");
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        for (int r = 0; r < 6; ++r) {
            require(core.run(0x100), "run");
            require(cpu.pc == 0x100 && cpu.dpregs[0] == uint32_t(2 * (r + 1)), "a promotion on this thread ran wrong");
            cpu.did_jump = false;
        }
        require(core.set_tier_background(false), "set_tier_background(false) refused");
    }
    require(g_worker_starts == 0, "a worker started in a build without threads");
}

#if defined(__EMSCRIPTEN__)
// The cold tier keeps what tier 0 promises (found in review): a block runs
// the code it was first compiled from until invalidate(), cold or not, and
// no halfword of it is read from the host twice. A block of R0 += 1; RTS,
// the add rewritten after the first run, then copy-and-patch turned off,
// the page table hidden or a chain filter set (which invalidates); and a
// parallel packet copy-and-patch does not take where the Memory's
// callbacks may change registers.
class CountingMemory : public FlatMemory {
public:
    explicit CountingMemory(CpuState* cpu) : FlatMemory(0, 0x4000, cpu) { enable_page_table(); }
    mutable unsigned reads = 0;
    bool shown = true, keeps_registers = true;
    uint16_t read16(uint32_t addr) const override {
        ++reads;
        return FlatMemory::read16(addr);
    }
    uint8_t* const* page_table() const override { return shown ? FlatMemory::page_table() : nullptr; }
    bool callbacks_preserve_registers() const override { return keeps_registers; }
};

static void test_cold_code() {
    enum Change { kCopyPatchOff, kPageTableHidden, kChainFilter };
    for (Change change : {kCopyPatchOff, kPageTableHidden, kChainFilter}) {
        uint32_t r0[2][4] = {};
        for (uint32_t cold : {0u, 2u}) {
            CpuState cpu{};
            CountingMemory mem(&cpu);
            mem.write16(0x100, kAdd1);
            mem.write16(0x102, 0x0010);  // RTS
            Core core(&cpu, &mem);
            core.set_tiering(UINT32_MAX);
            core.set_copy_patch(true);
            if (cold) require(core.set_cold_threshold(cold), "no cold tier");
            require(core.init(1), "init");
            cec_init(); evt_init();
            cpu.lb[0] = cpu.lb[1] = 0x3f00;
            for (int r = 0; r < 4; ++r) {
                if (r == 1) {
                    mem.write16(0x100, kAdd2);
                    if (change == kCopyPatchOff) core.set_copy_patch(false);
                    if (change == kPageTableHidden) mem.shown = false;
                    if (change == kChainFilter) core.set_chain_filter([](uint32_t) { return true; });
                }
                cpu.rets = 0x800;
                cpu.dpregs[0] = 0;
                require(core.run(0x100), "run");
                r0[cold != 0][r] = cpu.dpregs[0];
            }
        }
        for (int r = 0; r < 4; ++r)
            if (r0[0][r] != r0[1][r]) {
                fprintf(stderr, "FAIL: change %d run %d: R0 %u without the cold tier, %u with it\n", int(change), r,
                        r0[0][r], r0[1][r]);
                std::exit(1);
            }
    }
    unsigned reads[2] = {};
    for (uint32_t cold : {0u, 2u}) {
        CpuState cpu{};
        CountingMemory mem(&cpu);
        mem.keeps_registers = false;
        const uint16_t code[] = {0xcc04, 0x000a, 0x9003, 0x0000, 0x0010};  // R0 = R1 + R2 || R3 = [P0++] || NOP; RTS
        for (int i = 0; i < 5; ++i) mem.write16(0x100 + 2 * i, code[i]);
        Core core(&cpu, &mem);
        core.set_tiering(UINT32_MAX);
        core.set_copy_patch(true);
        if (cold) require(core.set_cold_threshold(cold), "no cold tier");
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        cpu.rets = 0x800;
        cpu.dpregs[8] = 0x2000;
        mem.reads = 0;
        require(core.run(0x100), "run");
        reads[cold != 0] = mem.reads;
    }
    if (reads[0] != reads[1]) {
        fprintf(stderr, "FAIL: a block no stencils take read %u halfwords, %u with the cold tier\n", reads[0], reads[1]);
        std::exit(1);
    }
}
#endif

// A remote promotion delivered after its engine went (invalidate, here by
// a chain filter): it must not replace the new engine's block at that PC,
// which runs other code now (found in review of the plan: invalidate bumped
// the generation only with a worker, and counters start again at 0 in the
// new engine, so the stale code would have looked current); nor one that
// failed. Either way the block keeps running what it is now.
static void test_remote_stale() {
    for (bool failed : {false, true}) {
        CpuState cpu{};
        FlatMemory mem(0, 0x4000, &cpu);
        mem.enable_page_table();
        mem.write16(0x100, kAdd1);
        mem.write16(0x102, 0x0010);  // RTS
        Core core(&cpu, &mem);
        core.set_tiering(1);
        core.set_copy_patch(true);
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> jobs;
        core.set_remote_tier([&](uint32_t ticket, std::vector<uint8_t> bytes) {
            jobs.emplace_back(ticket, std::move(bytes));
            return true;
        });
        require(core.init(1), "init");
        cec_init(); evt_init();
        cpu.lb[0] = cpu.lb[1] = 0x3f00;
        cpu.rets = 0x800;
        require(core.run(0x100), "run");
        require(jobs.size() == 1, "no remote job after the first start");
        // The code changes and the engine goes: the next start compiles R0 += 2.
        mem.write16(0x100, kAdd2);
        core.set_chain_filter([](uint32_t) { return true; });
        cpu.rets = 0x800;
        cpu.dpregs[0] = 0;
        require(core.run(0x100), "run");
        RemoteCode code;
        RemoteCompiler compiler;
        require(compiler.compile(jobs[0].second.data(), jobs[0].second.size(), code), "the job did not compile");
        require(!core.deliver(jobs[0].first, !failed, code.object, code.name, code.fallthrough_pc),
                "a stale promotion was installed");
        cpu.rets = 0x800;
        cpu.dpregs[0] = 0;
        require(core.run(0x100), "run");
        require(cpu.dpregs[0] == 2, "the old engine's promotion ran");
        require(core.remote_stats().installed == 0 && core.remote_stats().stale + core.remote_stats().failed == 1,
                "the stale delivery was not counted");
    }
}

// A step limit at init, none when LLVM is made (on demand, at the first
// promotion): a remote promotion compiles as one here would, with the step
// checks init's limit asks for (found in review: a job named only the
// later limit, and the remote code ran the block's three packets on a
// budget of one). The same with the limit the other way round.
static void test_remote_step_limit() {
    for (const bool limit_first : {true, false}) {
        uint32_t r0[2], pc[2];
        for (const bool remote : {false, true}) {
            CpuState cpu{};
            cpu.steps_remaining = limit_first ? 5 : 0;
            FlatMemory mem(0, 0x4000, &cpu);
            mem.enable_page_table();
            for (uint32_t at = 0x100; at < 0x106; at += 2) mem.write16(at, kAdd1);
            mem.write16(0x106, 0x0010);  // RTS
            Core core(&cpu, &mem);
            core.set_llvm_on_demand(true);
            core.set_tiering(1);
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> jobs;
            if (remote)
                core.set_remote_tier([&](uint32_t ticket, std::vector<uint8_t> bytes) {
                    jobs.emplace_back(ticket, std::move(bytes));
                    return true;
                });
            require(core.init(1), "init");
            cec_init(); evt_init();
            cpu.lb[0] = cpu.lb[1] = 0x3f00;
            cpu.rets = 0x800;
            cpu.steps_remaining = limit_first ? 0 : 5;
            require(core.run(0x100), "run");
            if (remote) {
                require(jobs.size() == 1, "no remote job");
                RemoteCompiler compiler;
                RemoteCode code;
                require(compiler.compile(jobs[0].second.data(), jobs[0].second.size(), code), "the job did not compile");
                require(core.deliver(jobs[0].first, true, code.object, code.name, code.fallthrough_pc),
                        "the promotion was not installed");
            }
            cpu.steps_remaining = 1;
            cpu.dpregs[0] = 0;
            cpu.did_jump = false;
            cpu.rets = 0x800;
            require(core.run(0x100), "run");
            r0[remote] = cpu.dpregs[0];
            pc[remote] = cpu.pc;
        }
        if (r0[0] != r0[1] || pc[0] != pc[1]) {
            fprintf(stderr, "FAIL: step limit %s: R0 %u pc %x here, R0 %u pc %x remote\n",
                    limit_first ? "at init only" : "after init", r0[0], pc[0], r0[1], pc[1]);
            std::exit(1);
        }
    }
}

#if defined(__EMSCRIPTEN__)
// The code delivered as a module (CodeLoader::install): one under a name it
// has no function of fails, its slot back and its module out of the map;
// one delivered after its engine went is dropped, out of the map too; a
// current one runs. The block's work is the same throughout.
static void test_remote_modules() {
    CpuState cpu{};
    FlatMemory mem(0, 0x4000, &cpu);
    mem.enable_page_table();
    mem.write16(0x100, kAdd1);
    mem.write16(0x102, 0x0010);  // RTS
    Core core(&cpu, &mem);
    core.set_tiering(1);
    core.set_copy_patch(true);
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> jobs;
    core.set_remote_tier([&](uint32_t ticket, std::vector<uint8_t> bytes) {
        jobs.emplace_back(ticket, std::move(bytes));
        return true;
    });
    require(core.init(1), "init");
    cec_init(); evt_init();
    cpu.lb[0] = cpu.lb[1] = 0x3f00;
    RemoteCompiler compiler;
    // A start, which asks for a promotion (threshold 1), and its code.
    auto start = [&](const char* why) {
        cpu.rets = 0x800;
        cpu.dpregs[0] = 0;
        require(core.run(0x100), "run");
        require(cpu.dpregs[0] == 1, why);
    };
    auto code_of = [&](size_t job) {
        RemoteCode code;
        require(compiler.compile(jobs[job].second.data(), jobs[job].second.size(), code), "the job did not compile");
        return code;
    };
    // A new engine: the next start compiles the block again, and asks again.
    auto invalidate = [&] { core.set_chain_filter([](uint32_t) { return true; }); };

    start("the first start");
    RemoteCode code = code_of(0);
    put_module(jobs[0].first, code.object);
    require(!core.deliver(jobs[0].first, true, {}, "no_such_function", code.fallthrough_pc),
            "a module without the function was installed");
    require(modules_held() == 0 && core.remote_stats().failed == 1, "the failed module stayed in the map");
    start("after the failed delivery");

    invalidate();
    start("the second engine's first start");
    require(jobs.size() == 2, "no job from the second engine");
    invalidate();
    start("the third engine's first start");
    require(jobs.size() == 3, "no job from the third engine");
    code = code_of(1);
    put_module(jobs[1].first, code.object);
    require(!core.deliver(jobs[1].first, true, {}, code.name, code.fallthrough_pc), "a stale module was installed");
    require(modules_held() == 0 && core.remote_stats().stale == 1, "the stale module stayed in the map");

    code = code_of(2);
    put_module(jobs[2].first, code.object);
    require(core.deliver(jobs[2].first, true, {}, code.name, code.fallthrough_pc), "the module was not installed");
    require(modules_held() == 0 && core.remote_stats().installed == 1, "the installed module stayed in the map");
    for (int r = 0; r < 3; ++r) start("the installed promotion's start");
}
#endif

int main() {
#if defined(__EMSCRIPTEN__)
    test_cold_code();
#endif
    test_no_worker();
    test_result_order();
    test_size_ref();
    test_throwing_callback();
    test_throwing_stencils();
    test_toggle_stencils();
    test_hidden_page_table();
    test_late_page_table();
    test_stale_promotion();
    std::vector<Case> cases;
    // 300 blocks of R0 += 1 that jump on to the next, the last back to the
    // first: the second time round, a chained run brings each to a count of 2.
    constexpr uint32_t kBlocks = 300;
    cases.push_back({"ring", [](FlatMemory& m) {
        for (uint32_t i = 0; i < kBlocks; ++i) {
            const uint32_t at = 0x100 + 4 * i;
            m.write16(at, kAdd1);
            m.write16(at + 2, jump_s(at + 2, i + 1 < kBlocks ? at + 4 : 0x100));
        }
    }, [](CpuState&, FlatMemory&, int) {}, 0x100, kBlocks + 40, 1000, nullptr, true});
    // A block that jumps to itself (a native loop), with budgets that end
    // runs in the middle of it.
    auto self = [](FlatMemory& m) {
        m.write16(0x100, kAdd1); m.write16(0x102, kAdd1); m.write16(0x104, jump_s(0x104, 0x100));
    };
    cases.push_back({"self loop", self, [](CpuState&, FlatMemory&, int) {}, 0x100, 12, 37, nullptr, true});
    // The same, its PC rejected by the chain filter: the host runs it
    // every time.
    cases.push_back({"filtered", self, [](CpuState&, FlatMemory&, int) {}, 0x100, 30, 37,
                     [](uint32_t pc) { return pc != 0x100; }, true});
    // Its first instruction rewritten after the first run, one pass round
    // the loop per run (so that the block is promoted after that): the block
    // keeps running what it was compiled from.
    cases.push_back({"rewritten", self, [](CpuState&, FlatMemory& m, int r) { if (r == 1) m.write16(0x100, kAdd2); },
                     0x100, 12, 1, nullptr});
    // A loop bottom inside the block from the third run on: its guard
    // misses, it is compiled again careful, which counts and is promoted.
    auto bottoms = [](CpuState& cpu, FlatMemory&, int r) {
        if (r < 2) return;
        cpu.lt[0] = 0x100; cpu.lb[0] = 0x102;
        if (cpu.lc[0] == 0) cpu.lc[0] = 3;
    };
    cases.push_back({"guard miss", self, bottoms, 0x100, 20, 37, nullptr});

    // Each against the untiered run with the same memory and chaining; the
    // dry and uncounted chains with stencils at -O1 only.
    const Mode modes[] = {
        {"raw"},
        {"paged", true},
        {"stencils", true, true},
        {"stencils dry", true, true, true},
        {"stencils uncounted", true, true, false, false},
        // Helper jumps and exits for every block or so (far from the
        // first, as on AArch64 once an arena holds 120 MiB).
        {"stencils near", true, true, false, true, 64},
        {"stencils, LLVM on demand", true, true, false, true, 0, 0, 1, 0, true},
        {"raw, LLVM on demand", false, false, false, true, 0, 0, 1, 0, true},
#if defined(__EMSCRIPTEN__)
        // Cold for two starts, compiled alone or four to a module, a batch
        // that does not fill after 40 packets (the promotion at threshold 1
        // asks at once).
        {"stencils cold", true, true, false, true, 0, 2},
        {"stencils cold batch", true, true, false, true, 0, 2, 4, 40},
#endif
    };
    unsigned compared = 0;
    uint64_t promoted = 0, overflows = 0, background_promoted = 0, stencil_blocks = 0, near_sets = 0;
    for (const Case& c : cases)
        for (int opt : {0, 1, 2})
        for (const Mode& mode : modes) {
            if ((mode.dry || !mode.count || mode.reach || mode.cold || mode.on_demand) && opt != 1) continue;
            if (mode.cold && !c.same_any_run) continue;
            const Outcome want = run(c, opt, 0, -1, mode);
            uint64_t packets = 0;  // a cold mode's: as many as the untiered runs
            if (mode.cold)
                for (const Run& r : want.runs) packets += uint64_t(r.chain_packets) + r.packets;
            // Thresholds with and without a size_ref (1: a block of n
            // packets n times the threshold), and promotions with the light
            // pipeline (Core::set_light_tier1).
            struct Tiers {
                uint32_t threshold, size_ref;
                bool light;
            };
            for (const Tiers& t : {Tiers{1, 0, false}, {2, 0, false}, {7, 0, false}, {1, 1, false}, {2, 1, true}})
            // Background -2 to -5 (-O1): promotions compiled remotely,
            // delivered before the next run, a run later, three later (the
            // latest asked first), never; -6 (WebAssembly) a run later as
            // modules.
            for (int background : {-1, 0, 1, 50, -2, -3, -4, -5, -6}) {
                if (background >= 0 && !Core::kHasThreads) continue;
                if (background <= -2 && opt != 1) continue;
#if !defined(__EMSCRIPTEN__)
                if (background == -6) continue;
#endif
                Remote remote;
                if (background <= -2)
                    remote = {background == -2 ? 0 : background == -3 || background == -6 ? 1 : background == -4 ? 3 : 1000,
                              background == -4, background == -6};
                const uint32_t threshold = t.threshold;
                const Outcome got = run(c, opt, threshold, background < 0 ? -1 : background, mode, t.size_ref,
                                        t.light, packets, remote);
                if (packets ? got.state != want.state || got.data != want.data : !(got == want)) {
                    fprintf(stderr, "FAIL: %s (%s) -O%d threshold %u size_ref %u%s background %d: %zu/%zu runs\n",
                            c.name, mode.name, opt, threshold, t.size_ref, t.light ? " light" : "", background,
                            want.runs.size(), got.runs.size());
                    for (size_t i = 0; i < want.runs.size() && i < got.runs.size(); ++i)
                        if (!(want.runs[i] == got.runs[i]))
                            fprintf(stderr, "  run %zu: pc %x/%x packets %u/%u chain %u/%u blocks %u/%u\n", i,
                                    want.runs[i].pc, got.runs[i].pc, want.runs[i].packets, got.runs[i].packets,
                                    want.runs[i].chain_packets, got.runs[i].chain_packets,
                                    want.runs[i].chain_blocks, got.runs[i].chain_blocks);
                    for (size_t i = 0; i + 4 <= want.state.size(); i += 4)
                        if (std::memcmp(&want.state[i], &got.state[i], 4))
                            fprintf(stderr, "  CpuState+%zu: %08x/%08x\n", i,
                                    *reinterpret_cast<const uint32_t*>(&want.state[i]),
                                    *reinterpret_cast<const uint32_t*>(&got.state[i]));
                    if (want.data != got.data) fprintf(stderr, "  guest memory differs\n");
                    return 1;
                }
                promoted += got.stats.blocks_promoted;
                overflows += got.stats.tier_request_overflows;
                require(got.stats.promotions_dropped == 0, "a promotion read code its tier-0 block had not");
                if (background > 0) background_promoted += got.stats.blocks_promoted;
                stencil_blocks += got.stats.copy_patch_blocks;
                if (mode.reach && got.stats.copy_patch_blocks > 1 && got.support_sets < 2) {
                    fprintf(stderr, "FAIL: %s (%s): %llu blocks of stencils share %u set of helper jumps\n", c.name,
                            mode.name, (unsigned long long)got.stats.copy_patch_blocks, got.support_sets);
                    return 1;
                }
                if (mode.reach) near_sets += got.support_sets;
                ++compared;
            }
        }
#if BCORE_ENABLE_PROFILE
    require(promoted > 0, "no block was promoted");
    require(overflows > 0, "no run filed more requests than the table holds");
    require(background_promoted > 0, "no block was promoted on the worker");
#if BCORE_COPY_PATCH_TEST
    require(stencil_blocks > 0, "no tier-0 block was made of stencils");
    require(near_sets > 0, "no set of helper jumps was made near a block");
#endif
#endif
    require(g_remote_installed > 0, "no remote promotion was installed");
    require(g_remote_refused > 0, "no remote promotion was refused (raw memory)");
    test_remote_stale();
    test_remote_step_limit();
#if defined(__EMSCRIPTEN__)
    require(modules_held() == 0, "a delivered module stayed in Module.bcoreModules");
    test_remote_modules();
#endif
    printf("tier test passed (%u cases, %llu promotions, %llu request overflows, %llu remote)\n", compared,
           (unsigned long long)promoted, (unsigned long long)overflows, (unsigned long long)g_remote_installed);
    return 0;
}
