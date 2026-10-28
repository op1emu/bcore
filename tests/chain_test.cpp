// The chain table: which compiled blocks generated code may run next itself
// (CpuState::chain_table, JitEngine::ChainSlot). The chained runs also as
// copy-and-patch blocks (opt kCopyPatch), whose own exit runs the block
// again without the table (CpTranslator::Config::self_loop).
#include "chain_table.h"
#include "core.h"
#include "bcore_profile.h"
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

// Two blocks with the same chain index: 1 << (kChainBits + 1) bytes apart.
static constexpr uint32_t kA = 0x100;
static constexpr uint32_t kB = kA + (1u << (kChainBits + 1));

static const ChainSlot& slot(const CpuState& cpu, uint32_t pc) {
    return static_cast<const ChainSlot*>(cpu.chain_table)[chainIndex(pc)];
}

static void test_table(int opt) {
    CpuState cpu{};
    FlatMemory mem(0, 2 * kB, &cpu);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    mem.write16(kA, 0x6009);  // R1 = 1
    mem.write16(kA + 2, 0x2000);  // JUMP.S 0
    mem.write16(kB, 0x6011);  // R1 = 2
    mem.write16(kB + 2, 0x2000);
    require(chainIndex(kA) == chainIndex(kB), "same index");

    require(core.run(kA) && cpu.dpregs[1] == 1, "run A");
    require(slot(cpu, kA).pc == kA, "A published when compiled");
    require(core.run(kB) && cpu.dpregs[1] == 2, "run B");
    require(slot(cpu, kB).pc == kB, "B takes the slot");
    require(core.run(kA) && cpu.dpregs[1] == 1, "run A again");
    require(slot(cpu, kA).pc == kA, "A gets the slot back when looked up");

    // A new engine starts empty; a filtered PC is never published.
    core.set_chain_filter([](uint32_t pc) { return pc != kA; });
    core.invalidate();
    require(core.run(kA) && cpu.dpregs[1] == 1, "run A filtered");
    require(slot(cpu, kA).pc == kChainEmpty, "filtered A not published");
    require(core.run(kB) && cpu.dpregs[1] == 2, "run B after invalidate");
    require(slot(cpu, kB).pc == kB, "B published");
    require(core.run(kA) && slot(cpu, kA).pc == kB, "a filtered lookup leaves the slot");
}

// The copy-and-patch tier 0, as opt: blocks of stencils (tier 0 throughout).
static constexpr int kCopyPatch = -1;

struct Chained {
    CpuState cpu{};
    FlatMemory mem{0, 0x10000, &cpu};
    Core core{&cpu, &mem};
    explicit Chained(int opt, bool dry = false, bool host_cycles = false) {
        core.set_chaining(true, dry);
        core.set_host_cycles(host_cycles);
        if (opt == kCopyPatch) {
            mem.enable_page_table();  // as copy-and-patch needs
            core.set_tiering(0x7fffffff);
            core.set_copy_patch(true);
        }
        require(core.init(opt == kCopyPatch ? 1 : opt), "init");
        cec_init(); evt_init();
    }
    void code(uint32_t pc, std::initializer_list<uint16_t> ops) {
        for (uint16_t op : ops) { mem.write16(pc, op); pc += 2; }
    }
    // One run from pc with this budget; the packets it retired.
    uint32_t run(uint32_t pc, uint32_t budget) {
        cpu.chain_budget = budget;
        require(core.run(pc), "run");
        return cpu.chain_packets + cpu.packets;
    }
};

static constexpr uint16_t kIncR0 = 0x6408;  // R0 += 1

// A block that jumps to itself loops in its own code while the budget
// lasts: a run ends with the first block that reaches it.
static void test_budget(int opt) {
    Chained c(opt);
    c.code(0x100, {kIncR0, 0x2fff});  // R0 += 1; JUMP.S 0x100
    require(c.run(0x100, 0) == 2 && c.cpu.dpregs[0] == 1 && c.cpu.chain_blocks == 0, "budget 0: one block");
    require(c.run(0x100, 7) == 8 && c.cpu.dpregs[0] == 5 && c.cpu.chain_blocks == 3, "budget 7: four blocks");
    require(c.run(0x100, 8) == 8 && c.cpu.dpregs[0] == 9 && c.cpu.pc == 0x100, "budget 8: four blocks");
}

// With set_idle_waits an IDLE ends its block and the run, with
// CpuState::idle set and pc past it; without, while the host's wake flag or
// cec_pending is set, or where a hardware loop may end at it, it does
// nothing.
static void test_idle(int opt) {
    {
        Chained c(opt);
        uint8_t wake = 1;
        c.core.set_idle_waits(true, &wake);
        c.code(0x100, {kIncR0, 0x0020, 0x2ffe});  // R0 += 1; IDLE; JUMP.S 0x100
        require(c.run(0x100, 1000) >= 1000 && !c.cpu.idle, "a wakeup due now: IDLE is a NOP");
        wake = 0;
        c.cpu.dpregs[0] = 0;
        require(c.run(0x100, 1000) == 2 && c.cpu.idle && c.cpu.dpregs[0] == 1, "none: the same code waits");
    }
    for (bool waits : {false, true}) {
        Chained c(opt);
        c.core.set_idle_waits(waits);
        c.code(0x100, {kIncR0, 0x0020, 0x2ffe});  // R0 += 1; IDLE; JUMP.S 0x100
        const uint32_t retired = c.run(0x100, 1000);
        if (waits) {
            require(retired == 2 && c.cpu.idle && c.cpu.pc == 0x104 && c.cpu.dpregs[0] == 1,
                    "IDLE ends the run past it");
            // The wakeup resumes past the IDLE: the jump back chains into
            // the top, which runs up to the IDLE again.
            require(c.run(0x104, 1000) == 3 && c.cpu.idle && c.cpu.pc == 0x104 && c.cpu.dpregs[0] == 2,
                    "the wakeup runs on to the next IDLE");
        } else {
            require(retired >= 1000 && !c.cpu.idle, "IDLE is a NOP");
        }
    }
    {
        // An interrupt to check (cec_pending): the core goes on to take it.
        Chained c(opt);
        c.core.set_idle_waits(true);
        c.code(0x100, {kIncR0, 0x0020, 0x2ffe});  // R0 += 1; IDLE; JUMP.S 0x100
        bcore_cec_pending = true;
        require(c.run(0x100, 1000) >= 1000 && !c.cpu.idle, "cec_pending: IDLE is a NOP");
        bcore_cec_pending = false;
        c.cpu.dpregs[0] = 0;
        require(c.run(0x100, 1000) == 2 && c.cpu.idle && c.cpu.dpregs[0] == 1, "none: the same code waits");
    }
    // A hardware loop that ends at the IDLE: its next PC is the loop's, so
    // the IDLE is a NOP and the loop runs its iterations. Known when the
    // block is compiled, or found by the entry guard of a block compiled to
    // wait there, which misses and runs the block compiled again careful.
    for (bool compiledToWait : {false, true}) {
        Chained c(opt);
        c.core.set_idle_waits(true);
        c.code(0x100, {kIncR0, 0x0020, 0x0010});  // R0 += 1; IDLE (loop bottom); RTS
        c.cpu.rets = 0x800;
        if (compiledToWait) {
            require(c.run(0x100, 1000) == 2 && c.cpu.idle && c.cpu.pc == 0x104, "no loop: the IDLE waits");
            c.cpu.dpregs[0] = 0;
        }
        c.cpu.lt[0] = 0x100; c.cpu.lb[0] = 0x102; c.cpu.lc[0] = 3;
        c.cpu.pc = 0x100;
        for (int runs = 0; c.cpu.pc != 0x800 && runs < 100; ++runs) {
            c.run(c.cpu.pc, 1000);
            require(!c.cpu.idle, "an IDLE at a loop bottom does not wait");
        }
        require(c.cpu.pc == 0x800 && c.cpu.dpregs[0] == 3 && c.cpu.lc[0] == 0, "three iterations, then RTS");
#if BCORE_ENABLE_PROFILE
        if (compiledToWait) require(c.core.stats().lb_guard_misses > 0, "the waiting block's guard missed");
#endif
    }
}

// Not counting chained blocks changes nothing else.
static void test_uncounted(int opt) {
    Chained c(opt);
    c.core.set_count_chained(false);
    c.code(0x200, {kIncR0, 0x207f});  // R0 += 1; JUMP.S 0x300
    c.code(0x300, {kIncR0, 0x2f7f});  // R0 += 1; JUMP.S 0x200
    c.run(0x200, 0);
    c.run(0x300, 0);
    require(c.run(0x200, 100) == 100 && c.cpu.dpregs[0] == 52 && c.cpu.chain_blocks == 0, "uncounted chain");
}

// A hardware loop's back edge the helper takes (bfin_hwloop_step, after a
// CLI, sets did_jump itself) shares its exit with the DBGA's failure path,
// whose store of did_jump that exit makes only where it returns: chaining
// on, the loop must still see did_jump clear in its next iteration.
static uint32_t helper_back_edge(int opt, uint32_t budget) {
    Chained c(opt);
    // R0 += 1; CLI R2 (loop bottom); DBGA (R3.L, 0); RTS
    c.code(0x100, {kIncR0, 0x0032, 0xf003, 0x0000, 0x0010});
    c.cpu.lt[0] = 0x100; c.cpu.lb[0] = 0x102; c.cpu.lc[0] = 4;
    c.cpu.rets = 0x800;
    c.cpu.pc = 0x100;
    while (c.cpu.pc != 0x800 && !c.cpu.halted) c.run(c.cpu.pc, budget);
    require(c.cpu.dpregs[0] == 4 && c.cpu.lc[0] == 0 && !c.cpu.halted, "four iterations, then RTS");
    return c.cpu.dpregs[0];
}

static void test_helper_back_edge(int opt) {
    require(helper_back_edge(opt, 0) == helper_back_edge(opt, 100), "chained as unchained");
}

// Two blocks run each other through the table once both are compiled.
static void test_table_chain(int opt) {
    Chained c(opt);
    c.code(0x200, {kIncR0, 0x207f});  // R0 += 1; JUMP.S 0x300
    c.code(0x300, {kIncR0, 0x2f7f});  // R0 += 1; JUMP.S 0x200
    require(c.run(0x200, 100) == 2 && c.cpu.pc == 0x300, "0x300 not compiled yet: return");
    require(c.run(0x300, 100) == 100 && c.cpu.dpregs[0] == 51 && c.cpu.pc == 0x300, "chained through the table");
}

// A block that returns to itself through a register has no native loop
// (nothing says it will) and chains to itself through the table.
static void test_self_through_table(int opt) {
    Chained c(opt);
    c.code(0x600, {kIncR0, 0x0010});  // R0 += 1; RTS
    c.cpu.rets = 0x600;
    require(c.run(0x600, 10) == 10 && c.cpu.dpregs[0] == 5 && c.cpu.chain_blocks == 4, "self through the table");
}

// The filter holds for a block's own native loop too, a jump to itself or
// a hardware loop at its start: a rejected block runs once and returns.
static void test_filtered_self(int opt) {
    {
        Chained c(opt);
        c.core.set_chain_filter([](uint32_t) { return false; });
        c.code(0x100, {kIncR0, 0x2fff});  // R0 += 1; JUMP.S 0x100
        require(c.run(0x100, 8) == 2 && c.cpu.dpregs[0] == 1 && c.cpu.chain_blocks == 0, "filtered self jump");
    }
    {
        Chained c(opt);
        c.core.set_chain_filter([](uint32_t) { return false; });
        c.cpu.lt[0] = 0x400; c.cpu.lb[0] = 0x400; c.cpu.lc[0] = 4;
        c.code(0x400, {kIncR0, 0x2000});  // one-packet hardware loop; JUMP.S 0
        require(c.run(0x400, 3) == 1 && c.cpu.dpregs[0] == 1 && c.cpu.pc == 0x400, "filtered hardware loop");
    }
}

// Setting a filter applies to blocks already in the table: a lookup asks it
// again, and no block chains into one it now rejects.
static void test_refilter(int opt) {
    Chained c(opt);
    c.code(0x200, {kIncR0, 0x207f});  // R0 += 1; JUMP.S 0x300
    c.code(0x300, {kIncR0, 0x2f7f});  // R0 += 1; JUMP.S 0x200
    c.run(0x200, 0);
    c.run(0x300, 0);
    int calls = 0;
    c.core.set_chain_filter([&](uint32_t pc) { ++calls; return pc != 0x200; });
    c.run(0x200, 0);
    require(calls > 0 && slot(c.cpu, 0x200).pc == kChainEmpty, "a lookup asks the new filter");
    const uint32_t before = c.cpu.dpregs[0];
    c.run(0x300, 6);
    require(c.cpu.dpregs[0] - before == 1 && c.cpu.chain_blocks == 0 && c.cpu.pc == 0x200, "no chain into a rejected block");
}

// A native loop holds the filter's answer from when its block was compiled:
// setting a filter that rejects a block already compiled drops it, so it
// runs once and returns from then on.
static void test_refilter_self(int opt) {
    Chained c(opt);
    c.code(0x100, {kIncR0, 0x2fff});  // R0 += 1; JUMP.S 0x100
    require(c.run(0x100, 8) == 8 && c.cpu.chain_blocks == 3, "native loop before the filter");
    c.core.set_chain_filter([](uint32_t pc) { return pc != 0x100; });
    const uint32_t before = c.cpu.dpregs[0];
    require(c.run(0x100, 8) == 2 && c.cpu.dpregs[0] - before == 1 && c.cpu.chain_blocks == 0,
            "no native loop once the filter rejects it");
}

// A hardware loop of one packet at its block's start runs while the budget
// lasts, as the LLVM blocks' native loop: the same packets, iterations and
// counts, however the budget falls.
static void test_hwloop_self(int opt) {
    for (uint32_t budget : {0u, 1u, 3u, 4u, 9u}) {
        uint32_t got[2][6];
        for (int side = 0; side < 2; ++side) {
            Chained c(side ? opt : 1);
            c.cpu.lt[0] = 0x400; c.cpu.lb[0] = 0x400; c.cpu.lc[0] = 4;
            c.code(0x400, {kIncR0, 0x0010});  // one-packet hardware loop; RTS
            c.cpu.rets = 0x800;
            const uint32_t retired = c.run(0x400, budget);
            uint32_t values[6] = {retired, c.cpu.dpregs[0], c.cpu.lc[0], c.cpu.pc, c.cpu.chain_blocks, c.cpu.did_jump};
            for (int i = 0; i < 6; ++i) got[side][i] = values[i];
        }
        for (int i = 0; i < 6; ++i) require(got[0][i] == got[1][i], "a hardware loop at the start, as LLVM's");
    }
}

// A pending interrupt returns to the host (which would check it).
static void test_pending(int opt) {
    Chained c(opt);
    c.code(0x400, {kIncR0, 0x009e, 0x2ffe});  // R0 += 1; RAISE 14; JUMP.S 0x400
    require(c.run(0x400, 100) == 2 && c.cpu.chain_blocks == 0 && c.cpu.pc == 0x404, "pending after RAISE");
}

// An interrupt a device latches while a block reads it ends the run: the
// block chains no further, and the host takes the interrupt before the next.
struct LatchingMemory : FlatMemory {
    static constexpr uint32_t kDevice = 0x20000;  // past the memory: every access calls back
    bool armed = false;
    LatchingMemory(CpuState* cpu) : FlatMemory(0, 0x10000, cpu), cpu_(cpu) {}
    uint32_t read32(uint32_t addr) const override {
        if (addr != kDevice) return FlatMemory::read32(addr);
        if (armed) cec_raise(cpu_, 14);
        return 0;
    }
    CpuState* cpu_;
};

static void test_pending_from_device(int opt) {
    CpuState cpu{};
    LatchingMemory mem(&cpu);
    Core core(&cpu, &mem);
    core.set_chaining(true);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    for (auto [pc, op] : {std::pair{0x100u, 0x9100}, {0x102u, 0x207f},  // R0 = [P0]; JUMP.S 0x200
                          {0x200u, 0x6409}, {0x202u, 0x2f7f}})          // R1 += 1; JUMP.S 0x100
        mem.write16(pc, uint16_t(op));
    cpu.dpregs[8] = LatchingMemory::kDevice;  // P0
    auto run = [&](uint32_t pc, uint32_t budget) {
        cpu.chain_budget = budget;
        require(core.run(pc), "run");
    };
    run(0x100, 0);
    run(0x200, 0);
    run(0x100, 100);
    require(cpu.chain_blocks > 0, "chains while nothing latches");
    mem.armed = true;
    run(0x100, 100);
    require(cpu.chain_blocks == 0 && cpu.pc == 0x200, "a latched interrupt ends the run");
}

// A chained block that reads CYCLES sees the cycles the blocks before it
// retired, as with a host that writes CYCLES before every block.
static void test_host_cycles(int opt) {
    Chained c(opt, false, true);
    c.code(0x500, {0x3196, 0x9202, 0x2ffe});  // R2 = CYCLES; [P0++] = R2; JUMP.S 0x500
    c.cpu.cycles[0] = 1000;
    c.cpu.dpregs[8] = 0x1000;
    require(c.run(0x500, 9) == 9, "three blocks");
    require(c.mem.read32(0x1000) == 1000 && c.mem.read32(0x1004) == 1003 && c.mem.read32(0x1008) == 1006,
            "CYCLES advances by the chained packets");
}

// Dry: the block says it would chain and returns.
static void test_dry(int opt) {
    Chained c(opt, true);
    c.code(0x100, {kIncR0, 0x2fff});
    require(c.run(0x100, 100) == 2 && c.cpu.chain_would && c.cpu.dpregs[0] == 1, "dry: would chain");
    require(c.run(0x100, 2) == 2 && !c.cpu.chain_would, "dry: budget reached");
}

int main() {
    for (int opt : {0, 1, 2, 3}) {
        test_table(opt);
        test_budget(opt);
        test_table_chain(opt);
        test_self_through_table(opt);
        test_filtered_self(opt);
        test_refilter(opt);
        test_refilter_self(opt);
        test_uncounted(opt);
        test_helper_back_edge(opt);
        test_pending_from_device(opt);
        test_pending(opt);
        test_host_cycles(opt);
        test_dry(opt);
        test_idle(opt);
        test_hwloop_self(opt);
    }
#if BCORE_COPY_PATCH_TEST
    {
        const int opt = kCopyPatch;
        test_budget(opt);
        test_table_chain(opt);
        test_self_through_table(opt);
        test_filtered_self(opt);
        test_refilter(opt);
        test_refilter_self(opt);
        test_uncounted(opt);
        test_helper_back_edge(opt);
        test_pending(opt);
        test_host_cycles(opt);
        test_dry(opt);
        test_idle(opt);
        test_hwloop_self(opt);
#if BCORE_ENABLE_PROFILE
        Chained c(opt);
        c.code(0x100, {kIncR0, 0x2fff});
        c.run(0x100, 8);
        require(c.core.stats().copy_patch_blocks == 1, "copy-and-patch blocks");
#endif
    }
#endif
    printf("chain test passed\n");
    return 0;
}
