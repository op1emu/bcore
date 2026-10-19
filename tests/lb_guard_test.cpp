// A block compiled while no hardware loop ended inside it skips the loop
// bottom compare at its packets, behind an entry guard (BBTranslator's
// lb_guard). Run later with a loop bottom inside it, it must behave exactly
// as a block compiled with that bottom known: the guard misses, Core::run
// compiles the block again careful and runs that in its place, in the same
// run, once.
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
    printf("lb guard test passed (%u cases)\n", cases);
    return 0;
}
