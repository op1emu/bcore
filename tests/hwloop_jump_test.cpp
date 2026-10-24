// A block's last instruction set did_jump itself: its epilog counts the
// hardware loops down at a loop bottom (LiftVisitor::emit_epilog_jumped) as
// bfin_hwloop_step does, however the loop registers have changed since the
// block was compiled -- whether or not a loop ended there then. Also as a
// copy-and-patch block (Core::set_copy_patch), whose stencils step inline.
#include "core.h"
#include "bcore_profile.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr uint32_t kPc = 0x100;      // the block: one instruction, then JUMP.S 0
static constexpr uint32_t kTarget = 0x200;  // where each of them goes when it jumps
static constexpr uint32_t kElsewhere = 0x300;

struct Ending {
    uint16_t op;
    const char* name;
    bool call;     // sets RETS (to the loop top at a bottom)
    int cond;      // -1 always taken, else taken when CC is this
};

static constexpr Ending kEndings[] = {
    {0x2080, "JUMP.S", false, -1},
    {0x0010, "RTS", false, -1},
    {0x0050, "JUMP (P0)", false, -1},
    {0x0060, "CALL (P0)", true, -1},
    {0x1880, "IF CC JUMP", false, 1},
    {0x1080, "IF !CC JUMP", false, 0},
};

static unsigned test(int opt, bool chained, const Ending& e, bool hinted, bool copy_patch) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    mem.write16(kPc, e.op);
    mem.write16(kPc + 2, 0x2000);  // JUMP.S 0: ends a branch's fallthrough
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    if (copy_patch) {
        mem.enable_page_table();  // as copy-and-patch needs
        core.set_tiering(0x7fffffff);  // tier 0 throughout
        core.set_copy_patch(true);
    }
    require(core.init(opt), "init");
    cec_init(); evt_init();

    // Compiled with loop 0 ending at the block's last instruction, or not.
    cpu.lb[0] = hinted ? kPc : kElsewhere;
    cpu.lb[1] = kElsewhere;
    cpu.rets = cpu.dpregs[8] = kTarget;
    require(core.run(kPc), "compile");
#if BCORE_COPY_PATCH_TEST && BCORE_ENABLE_PROFILE
    if (copy_patch) require(core.stats().copy_patch_blocks == 1, "a copy-and-patch block");
#endif

    unsigned cases = 0;
    for (unsigned bottoms = 0; bottoms < 4; ++bottoms)
    for (uint32_t lc0 : {0u, 1u, 2u, 0xffffffffu})
    for (uint32_t lc1 : {0u, 1u, 2u, 0xffffffffu})
    for (uint32_t cc : {0u, 1u}) {
        cpu = {};
        cpu.chain_budget = 0;  // chained: one block, through the chain's exit
        cpu.rets = cpu.dpregs[8] = kTarget;
        cpu.cc = cc;
        cpu.lb[0] = bottoms & 1 ? kPc : kElsewhere;
        cpu.lb[1] = bottoms & 2 ? kPc : kElsewhere;
        cpu.lt[0] = 0x400; cpu.lt[1] = 0x500;
        cpu.lc[0] = lc0; cpu.lc[1] = lc1;

        CpuState expected = cpu;
        const bool taken = e.cond < 0 || cc == uint32_t(e.cond);
        expected.did_jump = taken;
        expected.pc = kTarget;
        if (e.call) {
            // At a bottom the subroutine returns to the loop top: where the
            // step's phase A goes from an instruction that did not jump.
            CpuState next = cpu;
            bfin_hwloop_step(&next, kPc, kPc + 2);
            expected.rets = next.pc;
        }
        bfin_hwloop_step(&expected, kPc, kPc + 2);
        if (!expected.did_jump) {
            // Neither the branch nor a loop: on to the JUMP.S 0.
            expected.did_jump = true;
            expected.pc = kPc + 2;
        }

        require(core.run(kPc), "run");
        if (cpu.pc != expected.pc || cpu.lc[0] != expected.lc[0] || cpu.lc[1] != expected.lc[1] ||
            cpu.rets != expected.rets || cpu.did_jump != expected.did_jump) {
            fprintf(stderr, "FAIL: -O%d%s%s %s compiled %s a bottom, bottoms=%u lc=%u,%u cc=%u: "
                            "pc=%x lc=%u,%u rets=%x did_jump=%d, expected pc=%x lc=%u,%u rets=%x did_jump=%d\n",
                    opt, chained ? " chained" : "", copy_patch ? " copy-patch" : "", e.name, hinted ? "at" : "not at", bottoms, lc0, lc1, cc,
                    cpu.pc, cpu.lc[0], cpu.lc[1], cpu.rets, cpu.did_jump,
                    expected.pc, expected.lc[0], expected.lc[1], expected.rets, expected.did_jump);
            std::exit(1);
        }
        ++cases;
    }
    return cases;
}

int main() {
    unsigned cases = 0;
    for (int opt : {0, 1, 2, 3})
        for (bool chained : {false, true})
            for (const Ending& e : kEndings)
                for (bool hinted : {false, true})
                    cases += test(opt, chained, e, hinted, false);
#if BCORE_COPY_PATCH_TEST
    for (bool chained : {false, true})
        for (const Ending& e : kEndings)
            cases += test(1, chained, e, false, true);
#endif
    printf("hwloop jump test passed (%u cases)\n", cases);
    return 0;
}
