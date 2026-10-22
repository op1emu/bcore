// Tiered compilation (Core::set_tiering): every block is compiled cheaply
// first and again at tier 1 once it started threshold times. The guest must
// do exactly what it does untiered, run for run: a block promoted in the
// middle of its native loop, so many blocks asking at once that the table
// overflows, a PC the chain filter keeps out of the chain table, a block
// compiled again careful after its loop-bottom guard missed, and code that
// changed in memory after tier 0 read it (an untiered block keeps running
// the code it was compiled from, and so does its promotion); and cases found
// in review (below).
#include "core.h"
#include "bcore_profile.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

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
};

// background: -1 promotions on this thread; else on the worker, published
// after that many packets (0: as soon as the worker is done).
static Outcome run(const Case& c, int opt, uint32_t threshold, int background = -1) {
    CpuState cpu{};
    FlatMemory mem(0, 0x4000, &cpu);
    c.load(mem);
    Core core(&cpu, &mem);
    core.set_chaining(true, false);
    core.set_tiering(threshold);
    if (background >= 0) core.set_tier_background(true, uint64_t(background));
    require(core.init(opt), "init");
    if (c.filter) core.set_chain_filter(c.filter);
    cec_init(); evt_init();
    cpu.lb[0] = cpu.lb[1] = 0x3f00;  // no loop bottom anywhere near the code
    Outcome out;
    for (int r = 0; r < c.runs; ++r) {
        c.before(cpu, mem, r);
        cpu.chain_budget = c.budget;
        require(core.run(r ? cpu.pc : c.start), "run");
        out.runs.push_back({cpu.pc, cpu.packets, cpu.chain_packets, cpu.chain_blocks});
        cpu.did_jump = false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(&cpu);
    out.state.assign(bytes, bytes + offsetof(CpuState, insn_len));
    out.data.assign(mem.raw(), mem.raw() + 0x4000);
    out.stats = core.stats();
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

static void test_throwing_callback() {
    for (int opt : {0, 1, 2})
        for (int mode : {0, 1, 2}) {  // untiered; tiered; tiered on the worker
            CpuState cpu{};
            ThrowingMemory mem(&cpu);
            mem.write16(0x100, 0x9000);  // R0 = [P0++]
            mem.write16(0x102, 0x2fff);  // JUMP.S 0x100
            Core core(&cpu, &mem);
            core.set_chaining(true, false);
            if (mode) core.set_tiering(1);
            if (mode == 2) core.set_tier_background(true, 1);
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
        }
}

int main() {
    test_throwing_callback();
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
    }, [](CpuState&, FlatMemory&, int) {}, 0x100, kBlocks + 40, 1000, nullptr});
    // A block that jumps to itself (a native loop), with budgets that end
    // runs in the middle of it.
    auto self = [](FlatMemory& m) {
        m.write16(0x100, kAdd1); m.write16(0x102, kAdd1); m.write16(0x104, jump_s(0x104, 0x100));
    };
    cases.push_back({"self loop", self, [](CpuState&, FlatMemory&, int) {}, 0x100, 12, 37, nullptr});
    // The same, its PC rejected by the chain filter: the host runs it
    // every time.
    cases.push_back({"filtered", self, [](CpuState&, FlatMemory&, int) {}, 0x100, 30, 37,
                     [](uint32_t pc) { return pc != 0x100; }});
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

    unsigned compared = 0;
    uint64_t promoted = 0, overflows = 0, background_promoted = 0;
    for (const Case& c : cases)
        for (int opt : {0, 1, 2}) {
            const Outcome want = run(c, opt, 0);
            for (uint32_t threshold : {1u, 2u, 7u})
            for (int background : {-1, 0, 1, 50}) {
                const Outcome got = run(c, opt, threshold, background);
                if (!(got == want)) {
                    fprintf(stderr, "FAIL: %s -O%d threshold %u background %d: %zu/%zu runs\n", c.name, opt,
                            threshold, background, want.runs.size(), got.runs.size());
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
                ++compared;
            }
        }
#if BCORE_ENABLE_PROFILE
    require(promoted > 0, "no block was promoted");
    require(overflows > 0, "no run filed more requests than the table holds");
    require(background_promoted > 0, "no block was promoted on the worker");
#endif
    printf("tier test passed (%u cases, %llu promotions, %llu request overflows)\n", compared,
           (unsigned long long)promoted, (unsigned long long)overflows);
    return 0;
}
