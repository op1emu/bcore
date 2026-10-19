// A block compiled while no hardware loop ended inside it skips the loop
// bottom compare at its packets, behind an entry guard (BBTranslator's
// lb_guard). Run later with a loop bottom inside it, it must behave exactly
// as a block compiled with that bottom known: the guard misses, Core::run
// compiles the block again careful and runs that in its place, in the same
// run, once -- also when that careful block chains into another that misses.
#include "core.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr uint32_t kPc = 0x100;   // R0 += 1, six times, then JUMP.S 0
static constexpr int kAdds = 6;
static constexpr uint32_t kElsewhere = 0x800;

struct Outcome { uint32_t pc, r0, lc0, packets; uint64_t misses; };

// The block compiled with LB0 at `compiled_lb`, then run with loop 0 from
// the block's start to its packet `bottom` (LC0 = lc).
static Outcome run(int opt, bool chained, uint32_t compiled_lb, int bottom, uint32_t lc) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    for (int i = 0; i < kAdds; ++i) mem.write16(kPc + 2 * i, 0x6408);
    mem.write16(kPc + 2 * kAdds, 0x2000);
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    require(core.init(opt), "init");
    cec_init(); evt_init();

    cpu.lb[0] = compiled_lb;
    cpu.lb[1] = kElsewhere;
    cpu.chain_budget = 0;
    require(core.run(kPc), "compile");

    cpu = {};
    cpu.chain_budget = 0;  // chained: the block's returns go through its chain exit
    cpu.lb[0] = kPc + 2 * bottom;
    cpu.lt[0] = kPc;
    cpu.lc[0] = lc;
    cpu.lb[1] = kElsewhere;
    Outcome out{};
    // Each run ends at a loop back edge or the JUMP.S; run on to the JUMP.S.
    for (int runs = 0; runs < 64 && cpu.pc != kPc + 2 * kAdds; ++runs) {
        require(core.run(runs ? cpu.pc : kPc), "run");
        out.packets += cpu.packets;
        cpu.did_jump = false;
    }
    out.pc = cpu.pc;
    out.r0 = cpu.dpregs[0];
    out.lc0 = cpu.lc[0];
    out.misses = core.stats().lb_guard_misses;
    return out;
}

// Two blocks that jump to each other, both compiled with no loop inside
// them, then run chained (a nonzero budget) with LB0 in one and LB1 in the
// other: the first misses, its careful compilation chains into the second,
// which misses too. The run must carry on through both, as blocks compiled
// with those bottoms known do, and leave no miss behind: a later run from
// another PC must work. (Found in review: Core::run handled one miss.)
struct PairOutcome { uint32_t pc, r0, lc0, lc1, packets, chain_packets, chain_blocks; bool ok; uint64_t misses; };

static PairOutcome run_pair(int opt, bool hinted) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    mem.write16(0x200, 0x6408); mem.write16(0x202, 0x207f);  // R0 += 1; JUMP.S 0x300
    mem.write16(0x300, 0x6408); mem.write16(0x302, 0x2f7f);  // R0 += 1; JUMP.S 0x200
    mem.write16(0x400, 0x6408); mem.write16(0x402, 0x2eff);  // R0 += 1; JUMP.S 0x200
    Core core(&cpu, &mem);
    core.set_chaining(true, false);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    cpu.lb[0] = hinted ? 0x200 : kElsewhere;
    cpu.lb[1] = hinted ? 0x300 : kElsewhere;
    cpu.chain_budget = 0;
    require(core.run(0x200) && core.run(0x300) && core.run(0x400), "compile");

    cpu = {};
    cpu.lb[0] = 0x200; cpu.lb[1] = 0x300;
    cpu.lt[0] = cpu.lt[1] = 0x800;
    cpu.lc[0] = cpu.lc[1] = 1;
    cpu.chain_budget = 8;
    PairOutcome out{};
    out.ok = core.run(0x200);
    out.pc = cpu.pc; out.r0 = cpu.dpregs[0]; out.lc0 = cpu.lc[0]; out.lc1 = cpu.lc[1];
    out.packets = cpu.packets; out.chain_packets = cpu.chain_packets; out.chain_blocks = cpu.chain_blocks;
    require(!cpu.lb_guard_miss, "a miss was left behind");
    cpu.chain_budget = 0;
    cpu.did_jump = false;
    out.ok = out.ok && core.run(0x400);  // the host may go anywhere next (an interrupt, say)
    out.r0 = cpu.dpregs[0];
    out.misses = core.stats().lb_guard_misses;
    return out;
}

static unsigned test_pair() {
    unsigned cases = 0;
    for (int opt : {0, 1, 2, 3}) {
        const PairOutcome want = run_pair(opt, true);
        const PairOutcome got = run_pair(opt, false);
        require(want.ok && got.ok, "a run failed");
        if (got.pc != want.pc || got.r0 != want.r0 || got.lc0 != want.lc0 || got.lc1 != want.lc1 ||
            got.packets != want.packets || got.chain_packets != want.chain_packets ||
            got.chain_blocks != want.chain_blocks) {
            fprintf(stderr, "FAIL: -O%d chained pair: pc %x r0 %u lc %u,%u packets %u chain %u/%u, "
                            "expected pc %x r0 %u lc %u,%u packets %u chain %u/%u\n",
                    opt, got.pc, got.r0, got.lc0, got.lc1, got.packets, got.chain_packets, got.chain_blocks,
                    want.pc, want.r0, want.lc0, want.lc1, want.packets, want.chain_packets, want.chain_blocks);
            std::exit(1);
        }
#if BCORE_ENABLE_PROFILE
        require(want.misses == 0 && got.misses == 2, "the pair missed other than once each");
#endif
        ++cases;
    }
    return cases;
}

int main() {
    unsigned cases = 0;
    for (int opt : {0, 1, 2})
        for (bool chained : {false, true})
            for (int bottom = 0; bottom < kAdds; ++bottom)
                for (uint32_t lc : {0u, 1u, 2u, 5u}) {
                    const Outcome want = run(opt, chained, kPc + 2 * bottom, bottom, lc);
                    const Outcome got = run(opt, chained, kElsewhere, bottom, lc);
                    if (got.pc != want.pc || got.r0 != want.r0 || got.lc0 != want.lc0 || got.packets != want.packets) {
                        fprintf(stderr, "FAIL: -O%d%s bottom %d lc %u: pc %x r0 %u lc0 %u packets %u, "
                                        "expected pc %x r0 %u lc0 %u packets %u\n",
                                opt, chained ? " chained" : "", bottom, lc, got.pc, got.r0, got.lc0, got.packets,
                                want.pc, want.r0, want.lc0, want.packets);
                        return 1;
                    }
#if BCORE_ENABLE_PROFILE
                    // Compiled again once, however often the loop came back.
                    require(want.misses == 0, "a block compiled at its bottom missed its guard");
                    require(got.misses == 1, "the guard missed other than once");
#endif
                    ++cases;
                }
    cases += test_pair();
    printf("lb guard test passed (%u cases)\n", cases);
    return 0;
}
