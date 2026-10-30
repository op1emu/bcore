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
#include <utility>

using namespace bcore;

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

// The guard misses in a block chained to: while Core::run compiles it again,
// the host's callbacks (its code fetches) see the run's packets so far, as
// chain_packets + packets, with nothing of the block that missed counted.
struct FetchClock : FlatMemory {
    CpuState* cpu;
    uint32_t watch = 0;
    mutable bool seen = false;
    mutable uint32_t retired = 0;
    explicit FetchClock(CpuState* c) : FlatMemory(0, 0x1000, c), cpu(c) {}
    uint32_t rawmem_limit() const override { return 0; }
    uint16_t read16(uint32_t addr) const override {
        if (watch && addr == watch && !seen) {
            seen = true;
            retired = cpu->chain_packets + cpu->packets;
        }
        return FlatMemory::read16(addr);
    }
};

static void test_fetch_during_careful(int opt) {
    CpuState cpu{};
    FetchClock mem(&cpu);
    Core core(&cpu, &mem);
    core.set_chaining(true);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    // R0 += 1; R0 = [P0]; JUMP.S 0x200, then R1 += 1; RTS
    for (auto [pc, op] : {std::pair{0x100u, 0x6408}, {0x102u, 0x9100}, {0x104u, 0x207e},
                          {0x200u, 0x6409}, {0x202u, 0x0010}})
        mem.write16(pc, uint16_t(op));
    cpu.lb[0] = cpu.lb[1] = kElsewhere;
    cpu.rets = kElsewhere;
    cpu.chain_budget = 0;
    require(core.run(0x100) && core.run(0x200), "compile");
    // A loop now ends at 0x200: its guard misses when 0x100 chains to it.
    cpu.dpregs[8] = 0x900;
    cpu.lt[0] = kElsewhere; cpu.lb[0] = 0x200; cpu.lc[0] = 1;
    cpu.rets = kElsewhere;
    mem.watch = 0x200;
    cpu.chain_budget = 8;
    require(core.run(0x100) && mem.seen, "careful compilation fetched the block");
    require(mem.retired == 3, "the fetch saw the three packets the run retired before the miss");
    require(cpu.chain_packets + cpu.packets == 5 && cpu.pc == kElsewhere, "then the careful block ran");
}

// A block compiled while loop 0 runs from its start to its packet `bottom`
// takes the back edge to that top as a constant (the guard pins LB0 and
// LT0). Run with another loop 0 -- its top or its bottom moved, or no loop
// -- it must miss and behave as a block compiled for that loop does.
struct PinOutcome { uint32_t pc, r0, lc0, packets, chain_packets; uint64_t misses; };

static PinOutcome run_pinned(int opt, bool chained, bool compile_running, uint32_t lt, uint32_t lb, uint32_t lc) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    for (int i = 0; i < kAdds; ++i) mem.write16(kPc + 2 * i, 0x6408);
    mem.write16(kPc + 2 * kAdds, 0x2000);
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    // Compiled for loop 0 from kPc to its fourth packet, running (or for
    // the loop the run has, which then needs no guard to miss).
    cpu.lt[0] = compile_running ? kPc : lt;
    cpu.lb[0] = compile_running ? kPc + 6 : lb;
    cpu.lc[0] = compile_running ? 9 : lc;
    cpu.lb[1] = kElsewhere;
    cpu.chain_budget = 0;
    require(core.run(kPc), "compile");

    cpu = {};
    cpu.lt[0] = lt; cpu.lb[0] = lb; cpu.lc[0] = lc;
    cpu.lb[1] = kElsewhere;
    PinOutcome out{};
    for (int runs = 0; runs < 64 && cpu.pc != kPc + 2 * kAdds; ++runs) {
        cpu.chain_budget = chained ? 1000 : 0;
        require(core.run(runs ? cpu.pc : kPc), "run");
        out.packets += cpu.packets;
        out.chain_packets += cpu.chain_packets;
        cpu.did_jump = false;
    }
    out.pc = cpu.pc;
    out.r0 = cpu.dpregs[0];
    out.lc0 = cpu.lc[0];
    out.misses = core.stats().lb_guard_misses;
    return out;
}

static unsigned test_pinned() {
    unsigned cases = 0;
    const uint32_t bottom = kPc + 6;
    struct Loop { uint32_t lt, lb, lc; bool same; };
    for (int opt : {0, 1, 2})
        for (bool chained : {false, true})
            for (Loop loop : {Loop{kPc, bottom, 3, true}, Loop{kPc, bottom, 1, true}, Loop{kPc, bottom, 0, true},
                              Loop{kPc + 2, bottom, 3, false}, Loop{kPc, bottom + 2, 3, false},
                              Loop{kPc + 4, bottom, 2, false}, Loop{kElsewhere, kElsewhere, 0, false}}) {
                const PinOutcome want = run_pinned(opt, chained, false, loop.lt, loop.lb, loop.lc);
                const PinOutcome got = run_pinned(opt, chained, true, loop.lt, loop.lb, loop.lc);
                if (got.pc != want.pc || got.r0 != want.r0 || got.lc0 != want.lc0 || got.packets != want.packets ||
                    got.chain_packets != want.chain_packets) {
                    fprintf(stderr, "FAIL: -O%d%s pinned loop lt %x lb %x lc %u: pc %x r0 %u lc0 %u packets %u/%u, "
                                    "expected pc %x r0 %u lc0 %u packets %u/%u\n",
                            opt, chained ? " chained" : "", loop.lt, loop.lb, loop.lc, got.pc, got.r0, got.lc0,
                            got.packets, got.chain_packets, want.pc, want.r0, want.lc0, want.packets,
                            want.chain_packets);
                    std::exit(1);
                }
#if BCORE_ENABLE_PROFILE
                require(got.misses == (loop.same ? 0u : 1u), "a pinned loop missed other than when it moved");
#endif
                ++cases;
            }
    return cases;
}

// Both loops: a block compiled with loop 1 (or both, at one bottom)
// running pins those, and must behave as one compiled for whatever loops it
// then runs with.
struct Loops { uint32_t lt[2], lb[2], lc[2]; };

static PinOutcome run_loops(int opt, bool chained, const Loops& compiled, const Loops& running) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    for (int i = 0; i < kAdds; ++i) mem.write16(kPc + 2 * i, 0x6408);
    mem.write16(kPc + 2 * kAdds, 0x2000);
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    auto set = [&](const Loops& l) {
        for (int i = 0; i < 2; ++i) { cpu.lt[i] = l.lt[i]; cpu.lb[i] = l.lb[i]; cpu.lc[i] = l.lc[i]; }
    };
    set(compiled);
    cpu.chain_budget = 0;
    require(core.run(kPc), "compile");
    cpu = {};
    set(running);
    PinOutcome out{};
    for (int runs = 0; runs < 128 && cpu.pc != kPc + 2 * kAdds; ++runs) {
        cpu.chain_budget = chained ? 1000 : 0;
        require(core.run(runs ? cpu.pc : kPc), "run");
        out.packets += cpu.packets;
        out.chain_packets += cpu.chain_packets;
        cpu.did_jump = false;
    }
    out.pc = cpu.pc;
    out.r0 = cpu.dpregs[0];
    out.lc0 = cpu.lc[0] | cpu.lc[1] << 16;
    out.misses = core.stats().lb_guard_misses;
    return out;
}

static unsigned test_pinned_both() {
    unsigned cases = 0;
    const uint32_t b = kPc + 6, e = kElsewhere;
    const Loops shapes[] = {
        {{e, kPc + 2}, {e, b}, {0, 3}},            // loop 1 alone
        {{kPc, kPc + 2}, {b, b}, {4, 3}},          // both at one bottom
        {{kPc, kPc + 2}, {b, b}, {5, 1}},          // both, loop 1 on its last pass
        {{kPc, kPc + 2}, {b, b}, {3, 0}},          // both, loop 1 idle
        {{kPc + 2, kPc}, {b, b + 2}, {2, 6}},      // loop 1 ends a packet later
        {{e, e}, {e, e}, {0, 0}},                  // none
    };
    for (int opt : {0, 1, 2})
        for (bool chained : {false, true})
            for (const Loops& compiled : shapes)
                for (const Loops& running : shapes) {
                    const PinOutcome want = run_loops(opt, chained, running, running);
                    const PinOutcome got = run_loops(opt, chained, compiled, running);
                    if (got.pc != want.pc || got.r0 != want.r0 || got.lc0 != want.lc0 || got.packets != want.packets ||
                        got.chain_packets != want.chain_packets) {
                        fprintf(stderr, "FAIL: -O%d%s loops compiled lc %u,%u run lc %u,%u: r0 %u/%u lc %x/%x "
                                        "packets %u/%u chain %u/%u\n",
                                opt, chained ? " chained" : "", compiled.lc[0], compiled.lc[1], running.lc[0],
                                running.lc[1], got.r0, want.r0, got.lc0, want.lc0, got.packets, want.packets,
                                got.chain_packets, want.chain_packets);
                        std::exit(1);
                    }
                    ++cases;
                }
    return cases;
}

// The same in copy-and-patch blocks (Core::set_copy_patch): three parallel
// packets, each R3 = [P0++] in a slot, then JUMP.S 0. Compiled with no loop
// in the block, a parallel packet's tail compares nothing behind the
// block's entry guard (CpTranslator::Config::hint); run with loop 0 ending
// at one of them, the guard misses once, Core::run compiles the block
// careful (no guard: else it would miss forever), and the run ends as a
// block compiled with no hint does.
struct CpOutcome { uint32_t pc, p0, lc0, packets; uint64_t misses, cp_blocks; };

static CpOutcome run_cp(bool chained, bool hints, uint32_t compiled_lb, uint32_t lb, uint32_t lc) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    mem.enable_page_table();
    constexpr int kPackets = 3;
    for (int i = 0; i < kPackets; ++i) {
        const uint16_t words[] = {0xcc04, 0x000a, 0x9003, 0x0000};  // R0 = R1 + R2 || R3 = [P0++] || NOP
        for (int w = 0; w < 4; ++w) mem.write16(kPc + 8 * i + 2 * w, words[w]);
    }
    const uint32_t end = kPc + 8 * kPackets;
    mem.write16(end, 0x2000);  // JUMP.S 0
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    core.set_copy_patch(true);
    core.set_copy_patch_hints(hints);
    core.set_tiering(1000000000, 0);
    require(core.init(1), "init");
    cec_init(); evt_init();
    cpu.lb[0] = compiled_lb;
    cpu.lt[0] = kPc;
    cpu.lc[0] = 5;
    cpu.lb[1] = kElsewhere;
    cpu.dpregs[8] = 0x800;
    cpu.chain_budget = 0;
    require(core.run(kPc), "compile");

    cpu = {};
    cpu.lb[0] = lb;
    cpu.lt[0] = kPc;
    cpu.lc[0] = lc;
    cpu.lb[1] = kElsewhere;
    cpu.dpregs[8] = 0x800;
    CpOutcome out{};
    for (int runs = 0; runs < 64 && cpu.pc != end; ++runs) {
        cpu.chain_budget = chained ? 1000 : 0;
        require(core.run(runs ? cpu.pc : kPc), "run");
        require(!cpu.lb_guard_miss, "a miss was left behind");
        out.packets += cpu.packets + cpu.chain_packets;
        cpu.did_jump = false;
    }
    out.pc = cpu.pc;
    out.p0 = cpu.dpregs[8];
    out.lc0 = cpu.lc[0];
    out.misses = core.stats().lb_guard_misses;
    out.cp_blocks = core.stats().copy_patch_blocks;
    return out;
}

static unsigned test_copy_patch() {
    unsigned cases = 0;
    for (bool chained : {false, true})
        for (int bottom = 0; bottom < 3; ++bottom)
            for (uint32_t lc : {0u, 1u, 2u, 5u}) {
                const uint32_t lb = kPc + 8 * bottom;
                const CpOutcome want = run_cp(chained, false, kElsewhere, lb, lc);
                for (uint32_t compiled_lb : {kElsewhere, lb}) {
                    const CpOutcome got = run_cp(chained, true, compiled_lb, lb, lc);
                    if (got.pc != want.pc || got.p0 != want.p0 || got.lc0 != want.lc0 || got.packets != want.packets) {
                        fprintf(stderr, "FAIL: copy-patch%s bottom %d lc %u compiled lb %x: pc %x p0 %x lc0 %u "
                                        "packets %u, expected pc %x p0 %x lc0 %u packets %u\n",
                                chained ? " chained" : "", bottom, lc, compiled_lb, got.pc, got.p0, got.lc0,
                                got.packets, want.pc, want.p0, want.lc0, want.packets);
                        std::exit(1);
                    }
#if BCORE_ENABLE_PROFILE
                    require(got.cp_blocks > 0, "no block made of stencils");
                    require(want.misses == 0, "a block with no hint missed a guard");
                    require(got.misses == (compiled_lb == lb ? 0u : 1u), "the guard missed other than once");
#endif
                    ++cases;
                }
            }
    return cases;
}

int main() {
    unsigned cases = 0;
    cases += test_copy_patch();
    cases += test_pinned();
    cases += test_pinned_both();
    for (int opt : {0, 1, 2, 3}) test_fetch_during_careful(opt);
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
