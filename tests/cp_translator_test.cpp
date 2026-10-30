// The copy-and-patch translator itself, without Core: a block whose packets
// all have stencils is made of them (CpTranslator::translate places code),
// and that code does what the block does. The other tests see a block made
// of stencils only through a profiling counter (copy_patch_blocks), which a
// build without BCORE_ENABLE_PROFILE -- WebAssembly's -- does not keep: there
// they would pass all the same were every block compiled with LLVM (found in
// review).
#include "../src/cp_translator.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace bcore;

static void require(bool ok, const std::string& why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why.c_str()); std::exit(1); }
}

static constexpr uint32_t kPc = 0x100;
static constexpr uint32_t kReturn = 0x8000;  // RETS
static constexpr uint32_t kTierOnly = 1000000000;  // tier 0 throughout

struct Setup {
    CpuState cpu{};
    FlatMemory mem{0, 0x10000, &cpu};
    JitEngine jit;
    Setup() {
        mem.enable_page_table();  // as copy-and-patch needs
        require(jit.init(), "jit init");
        cpu.chain_table = jit.chainTable();
        cpu.page_table = mem.page_table();
        cpu.rets = kReturn;
        cpu.pc = kPc;
        cec_init(); evt_init();
    }
    void code(std::initializer_list<uint16_t> words) {
        uint32_t at = kPc;
        for (uint16_t w : words) { mem.write16(at, w); at += 2; }
    }
};

// 16-bit, 32-bit and parallel packets, a load through the page table, RTS.
static void test_packets() {
    Setup s;
    s.code({0x6008,                          // R0 = 1
            0x6408,                          // R0 += 1
            0xe101, 0x1234,                  // R1.L = 0x1234
            0xcc04, 0x000a, 0x9003, 0x0000,  // R0 = R1 + R2 || R3 = [P0++] || NOP
            0x0010});                        // RTS
    s.mem.write32(0x2000, 0xcafef00d);
    s.cpu.dpregs[1] = 0x5670000;
    s.cpu.dpregs[2] = 2;
    s.cpu.dpregs[8] = 0x2000;  // P0
    CpTranslator cp(s.jit);
    CpTranslator::Config config;
    config.threshold = kTierOnly;
    const CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
    require(block.fn, std::string("no block made of stencils: ") + (block.why ? block.why : "?"));
    require(block.packets == 5, "five packets");
    s.jit.enter(block.fn, &s.cpu, &s.mem);
    require(s.cpu.dpregs[1] == 0x5671234, "R1.L");
    require(s.cpu.dpregs[0] == 0x5671236, "R0 = R1 + R2");
    require(s.cpu.dpregs[3] == 0xcafef00d && s.cpu.dpregs[8] == 0x2004, "R3 = [P0++]");
    require(s.cpu.pc == kReturn && s.cpu.packets == 5, "returned after five packets");
}

// A block the packet cap cuts goes on to the next PC.
static void test_cap() {
    for (unsigned cap : {1u, 2u, 31u, 64u, 256u}) {
        Setup s;
        for (unsigned n = 0; n < 64; ++n) s.mem.write16(kPc + 2 * n, 0x6408);  // R0 += 1
        s.mem.write16(kPc + 128, 0x0010);  // RTS
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.max_packets = cap;
        const CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        const std::string what = "cap " + std::to_string(cap);
        require(block.fn, what + ": no block made of stencils");
        s.jit.enter(block.fn, &s.cpu, &s.mem);
        const unsigned adds = cap < 64 ? cap : 64;
        require(s.cpu.dpregs[0] == adds, what + ": R0");
        require(s.cpu.pc == (cap <= 64 ? kPc + 2 * cap : kReturn), what + ": next PC");
    }
}

// A block that jumps to itself: through the chain table, its own exit, or
// back to the host, as configured.
static void test_loop() {
    for (bool chaining : {false, true})
    for (bool self_loop : {false, true})
    for (bool dry : {false, true})
    for (bool counted : {false, true}) {
        Setup s;
        s.code({0x6408, 0x2fff});  // R0 += 1; JUMP.S 0x100
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.chaining = chaining;
        config.self_loop = self_loop;
        config.dry = dry;
        config.count_chained = counted;
        const CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        const std::string what = std::string(chaining ? "chained" : "unchained") + (self_loop ? " self" : "") +
                                 (dry ? " dry" : "") + (counted ? " counted" : "");
        require(block.fn, what + ": no block made of stencils");
        s.jit.addCode(kPc, block.fn, block.fallthrough_pc);
        s.cpu.chain_budget = 7;
        s.jit.enter(block.fn, &s.cpu, &s.mem);
        const unsigned runs = chaining && !dry ? 4 : 1;
        require(s.cpu.dpregs[0] == runs && s.cpu.pc == kPc, what + ": runs");
        require(s.cpu.packets + s.cpu.chain_packets == 2 * runs, what + ": packets");
        require(s.cpu.chain_blocks == (counted ? runs - 1 : 0), what + ": chained blocks");
    }
}

// A hardware loop whose bottom is a parallel packet, R3 = [P0++] in a slot
// counting its iterations, in a block translated with a hint
// (Config::hint): the tails it pins, or that compare nothing, behind the
// block's entry guard. Run until it returns, from loop registers the hint
// may not have (a guard that misses compiles the block careful, as
// Core::run does), it must end as the block with no hint does.
struct LoopRun {
    uint32_t r0, r3, p0, lc0, lb0, pc, misses, starts;
    bool operator==(const LoopRun& o) const {
        return r0 == o.r0 && r3 == o.r3 && p0 == o.p0 && lc0 == o.lc0 && lb0 == o.lb0 && pc == o.pc;
    }
};

static LoopRun run_loop(unsigned packets, const CpuState* hint, bool careful, uint32_t lb0, uint32_t lt0,
                        uint32_t lc0) {
    Setup s;
    for (unsigned i = 0; i < packets; ++i)
        s.code({0xcc04, 0x000a, 0x9003, 0x0000});  // R0 = R1 + R2 || R3 = [P0++] || NOP
    if (packets == 2) {
        s.mem.write16(kPc + 8, 0xcc04); s.mem.write16(kPc + 10, 0x000a);
        s.mem.write16(kPc + 12, 0x9003); s.mem.write16(kPc + 14, 0x0000);
    }
    s.mem.write16(kPc + 8 * packets, 0x0010);  // RTS
    for (uint32_t i = 0; i < 16; ++i) s.mem.write32(0x2000 + 4 * i, 0x100 + i);
    s.cpu.dpregs[1] = 7;
    s.cpu.dpregs[2] = 1;
    s.cpu.dpregs[8] = 0x2000;  // P0
    s.cpu.lb[0] = lb0;
    s.cpu.lt[0] = lt0;
    s.cpu.lc[0] = lc0;
    s.cpu.lb[1] = 0x9000;
    CpTranslator cp(s.jit);
    CpTranslator::Config config;
    config.threshold = kTierOnly;
    config.hint = hint;
    config.careful = careful;
    CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
    require(block.fn, "no block made of stencils");
    LoopRun out{};
    for (int runs = 0; runs < 64 && s.cpu.pc != kReturn; ++runs) {
        s.jit.enter(block.fn, &s.cpu, &s.mem);
        if (s.cpu.lb_guard_miss) {
            require(s.cpu.pc == kPc && s.cpu.dpregs[8] == 0x2000 + 4 * out.r3, "a guard miss runs no packet");
            s.cpu.lb_guard_miss = 0;
            ++out.misses;
            config.careful = true;
            block = cp.translate(kPc, s.mem, 1, config);
            require(block.fn, "no careful block");
        }
        s.cpu.did_jump = false;
        out.r3 = (s.cpu.dpregs[8] - 0x2000) / 4;
    }
    out.r0 = s.cpu.dpregs[0];
    out.r3 = s.cpu.dpregs[3];
    out.p0 = s.cpu.dpregs[8];
    out.lc0 = s.cpu.lc[0];
    out.lb0 = s.cpu.lb[0];
    out.pc = s.cpu.pc;
    out.starts = s.jit.tierTable()->count[0];
    return out;
}

static void test_hints() {
    for (unsigned packets : {1u, 2u}) {
        // The loop's bottom (the last parallel packet), another packet of
        // the block, and a PC outside it.
        const uint32_t bottom = kPc + 8 * (packets - 1), elsewhere = 0x800;
        for (uint32_t lc : {0u, 1u, 2u, 5u})
        for (uint32_t lb : {bottom, kPc, elsewhere})
        for (uint32_t lt : {kPc, kPc + 0x40}) {
            const LoopRun want = run_loop(packets, nullptr, false, lb, lt, lc);
            require(want.pc == kReturn && want.misses == 0, "the block with no hint returns");
            for (uint32_t hint_lb : {bottom, kPc, elsewhere})
            for (uint32_t hint_lc : {0u, 5u})
            for (bool careful : {false, true}) {
                CpuState hint{};
                hint.lb[0] = hint_lb;
                hint.lt[0] = kPc;
                hint.lc[0] = hint_lc;
                hint.lb[1] = 0x9000;
                const LoopRun got = run_loop(packets, &hint, careful, lb, lt, lc);
                const std::string what = std::to_string(packets) + " packets, LB0 " + std::to_string(lb) + " LT0 " +
                                         std::to_string(lt) + " LC0 " + std::to_string(lc) + ", hint LB0 " +
                                         std::to_string(hint_lb) + " LC0 " + std::to_string(hint_lc) +
                                         (careful ? " careful" : "");
                require(got == want, what + ": not as the block with no hint");
                // The block has a guard where a packet's tail relies on it:
                // one the hint puts at no bottom, or one whose loop it pins
                // (it runs, LC0 not 0); none careful. The guard misses where
                // the loop registers are not as it relies on: a pinned
                // loop's LB0 and LT0 the hint's, or else LB0 the hint's or
                // outside the block (its RTS in it), and only there.
                const bool pinned = hint_lb != elsewhere && hint_lc != 0;
                const bool absent = packets == 2 || hint_lb != bottom;
                const bool guarded = !careful && (pinned || absent);
                const bool inside = lb >= kPc && lb <= kPc + 8 * packets;
                const bool holds = pinned ? lb == hint_lb && lt == kPc : lb == hint_lb || !inside;
                require(got.misses == (guarded && !holds ? 1u : 0u), what + ": guard misses");
                // Before the prologue: a block that misses counts no start.
                require(!got.misses || got.starts == 0, what + ": a guard miss counted");
            }
        }
    }
}

// A packet that moves a loop bottom (kLoopsMoved) before a parallel packet:
// LB0 = R4 puts loop 0's bottom on it, from a hint with no bottom in the
// block. Its tail must compare from there on, as the block with no hint's.
static void test_loops_moved() {
    auto run = [](const CpuState* hint) {
        Setup s;
        s.code({0x3c14,                          // LB0 = R4
                0xcc04, 0x000a, 0x9003, 0x0000,  // R0 = R1 + R2 || R3 = [P0++] || NOP
                0x0010});                        // RTS
        s.cpu.dpregs[4] = kPc + 2;
        s.cpu.dpregs[8] = 0x2000;
        s.cpu.lb[0] = 0x800;
        s.cpu.lt[0] = kPc;
        s.cpu.lc[0] = 3;
        s.cpu.lb[1] = 0x9000;
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.hint = hint;
        CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        require(block.fn, "no block made of stencils");
        for (int runs = 0; runs < 64 && s.cpu.pc != kReturn; ++runs) {
            s.jit.enter(block.fn, &s.cpu, &s.mem);
            if (s.cpu.lb_guard_miss) {
                s.cpu.lb_guard_miss = 0;
                config.careful = true;
                block = cp.translate(kPc, s.mem, 1, config);
                require(block.fn, "no careful block");
            }
            s.cpu.did_jump = false;
        }
        require(s.cpu.pc == kReturn, "returns");
        return std::make_pair(s.cpu.dpregs[8], s.cpu.lc[0]);
    };
    CpuState hint{};
    hint.lb[0] = 0x800;
    hint.lt[0] = kPc;
    hint.lc[0] = 3;
    hint.lb[1] = 0x9000;
    const auto want = run(nullptr), got = run(&hint);
    require(want.first == 0x2000 + 4 * 3 && want.second == 0, "the loop runs three times");
    require(got == want, "a loop moved in the block: not as the block with no hint");
}

// Both loops ending at a parallel packet that is the block's start, from
// every pair of counts: the hint pins loop 0, loop 1 or both (or neither,
// one count 0) and the tail steps as bfin_hwloop_step would, loop 1 first.
static void test_two_loops() {
    auto run = [](const CpuState* hint, uint32_t lc0, uint32_t lc1) {
        Setup s;
        s.code({0xcc04, 0x000a, 0x9003, 0x0000,  // R0 = R1 + R2 || R3 = [P0++] || NOP
                0x0010});                        // RTS
        s.cpu.dpregs[8] = 0x2000;
        for (int i = 0; i < 2; ++i) {
            s.cpu.lb[i] = kPc;
            s.cpu.lt[i] = kPc;
        }
        s.cpu.lc[0] = lc0;
        s.cpu.lc[1] = lc1;
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.hint = hint;
        CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        require(block.fn, "no block made of stencils");
        unsigned misses = 0;
        for (int runs = 0; runs < 64 && s.cpu.pc != kReturn; ++runs) {
            s.jit.enter(block.fn, &s.cpu, &s.mem);
            if (s.cpu.lb_guard_miss) {
                s.cpu.lb_guard_miss = 0;
                ++misses;
                config.careful = true;
                block = cp.translate(kPc, s.mem, 1, config);
                require(block.fn, "no careful block");
            }
            s.cpu.did_jump = false;
        }
        require(s.cpu.pc == kReturn, "returns");
        return std::make_tuple(s.cpu.dpregs[8], s.cpu.lc[0], s.cpu.lc[1], misses);
    };
    for (uint32_t lc0 : {0u, 1u, 2u, 3u})
    for (uint32_t lc1 : {0u, 1u, 2u, 3u}) {
        const auto want = run(nullptr, lc0, lc1);
        for (uint32_t h0 : {0u, 3u})
        for (uint32_t h1 : {0u, 3u}) {
            CpuState hint{};
            for (int i = 0; i < 2; ++i) {
                hint.lb[i] = kPc;
                hint.lt[i] = kPc;
            }
            hint.lc[0] = h0;
            hint.lc[1] = h1;
            const auto got = run(&hint, lc0, lc1);
            const std::string what = "LC " + std::to_string(lc0) + "," + std::to_string(lc1) + " hint LC " +
                                     std::to_string(h0) + "," + std::to_string(h1);
            require(std::get<0>(got) == std::get<0>(want) && std::get<1>(got) == std::get<1>(want) &&
                        std::get<2>(got) == std::get<2>(want),
                    what + ": not as the block with no hint");
            require(std::get<3>(got) == 0, what + ": the loops as the hint has them, and a guard missed");
        }
    }
}

// A hardware loop at the block's start, ending at its parallel packet, the
// block chaining with its own exit: pinned, it runs itself again past its
// guard (kSelfExitsLoop), its prologue still counting every start. It must
// end as the block with no hint does, the same starts counted; and where a
// packet in the loop moves LB0 (LB0 = R4, to where it is), through its guard
// every time.
static void test_self_loop() {
    auto run = [](const CpuState* hint, bool moves, bool counted) {
        Setup s;
        if (moves) s.code({0x3c14});  // LB0 = R4
        const uint32_t bottom = kPc + (moves ? 2 : 0);
        const uint16_t loop[] = {0xcc04, 0x000a, 0x9003, 0x0000,  // R0 = R1 + R2 || R3 = [P0++] || NOP
                                 0x0010};                          // RTS
        for (int i = 0; i < 5; ++i) s.mem.write16(bottom + 2 * i, loop[i]);
        s.cpu.dpregs[4] = bottom;
        s.cpu.dpregs[8] = 0x2000;
        s.cpu.lb[0] = bottom;
        s.cpu.lt[0] = kPc;
        s.cpu.lc[0] = 9;
        s.cpu.lb[1] = 0x9000;
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.chaining = true;
        config.self_loop = true;
        config.count_chained = counted;
        config.hint = hint;
        CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        require(block.fn, "no block made of stencils");
        s.jit.addCode(kPc, block.fn, block.fallthrough_pc);
        s.cpu.chain_budget = 1000;
        s.jit.enter(block.fn, &s.cpu, &s.mem);
        require(!s.cpu.lb_guard_miss, "a guard missed");
        return std::make_tuple(s.cpu.pc, s.cpu.dpregs[8], s.cpu.lc[0], s.cpu.packets + s.cpu.chain_packets,
                               s.cpu.chain_blocks, s.jit.tierTable()->count[0]);
    };
    for (bool moves : {false, true})
    for (bool counted : {false, true}) {
        CpuState hint{};
        hint.lb[0] = kPc + (moves ? 2 : 0);
        hint.lt[0] = kPc;
        hint.lc[0] = 9;
        hint.lb[1] = 0x9000;
        const auto want = run(nullptr, moves, counted), got = run(&hint, moves, counted);
        const std::string what = std::string(moves ? "moved" : "steady") + (counted ? " counted" : "");
        require(std::get<0>(want) == kReturn && std::get<1>(want) == 0x2000 + 4 * 9 && std::get<5>(want) == 9,
                what + ": the loop runs nine times");
        require(got == want, what + ": not as the block with no hint");
    }
}

// A guarded block that moves a loop and runs itself again: its parallel
// packet compares nothing (the hint's LB0 outside the block), then LB0 = R4
// puts loop 0's bottom on it and JUMP.S goes back to it, through the
// block's own exit. The next start must pass the guard again, which misses
// (a careful block runs on), not run past it: the packet would not count
// the loop down.
static void test_self_loop_moved() {
    auto run = [](const CpuState* hint) {
        Setup s;
        s.code({0xcc04, 0x000a, 0x9003, 0x0000,  // R0 = R1 + R2 || R3 = [P0++] || NOP
                0x3c14,                          // LB0 = R4
                0x2ffb});                        // JUMP.S 0x100
        s.cpu.dpregs[4] = kPc;
        s.cpu.dpregs[8] = 0x2000;
        s.cpu.lb[0] = 0x800;
        s.cpu.lt[0] = kPc + 8;
        s.cpu.lc[0] = 4;
        s.cpu.lb[1] = 0x9000;
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.chaining = true;
        config.self_loop = true;
        config.hint = hint;
        CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        require(block.fn, "no block made of stencils");
        s.jit.addCode(kPc, block.fn, block.fallthrough_pc);
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> runs_seen;  // after each run
        for (int runs = 0; runs < 8; ++runs) {
            s.cpu.chain_budget = 60;
            s.cpu.chain_packets = 0;
            s.jit.enter(block.fn, &s.cpu, &s.mem);
            if (s.cpu.lb_guard_miss) {
                // As Core::run: the careful block in its place, in the same run.
                s.cpu.lb_guard_miss = 0;
                s.cpu.packets = 0;
                config.careful = true;
                block = cp.translate(kPc, s.mem, 1, config);
                require(block.fn, "no careful block");
                s.jit.addCode(kPc, block.fn, block.fallthrough_pc);
                s.jit.enter(block.fn, &s.cpu, &s.mem);
                require(!s.cpu.lb_guard_miss, "the careful block missed");
            }
            s.cpu.did_jump = false;
            runs_seen.emplace_back(s.cpu.pc, s.cpu.dpregs[8], s.cpu.lc[0]);
        }
        return runs_seen;
    };
    CpuState hint{};
    hint.lb[0] = 0x800;
    hint.lt[0] = kPc + 8;
    hint.lc[0] = 4;
    hint.lb[1] = 0x9000;
    const auto want = run(nullptr), got = run(&hint);
    require(std::get<2>(want.front()) == 0, "loop 0 counted down to its end");
    require(got == want, "a guarded block that moves a loop, again: not as the block with no hint");
}

int main() {
    test_packets();
    test_self_loop_moved();
    test_self_loop();
    test_two_loops();
    test_loops_moved();
    test_cap();
    test_loop();
    test_hints();
    printf("PASS\n");
    return 0;
}
