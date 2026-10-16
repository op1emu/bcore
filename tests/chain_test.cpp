// The chain table: which compiled blocks generated code may run next itself
// (CpuState::chain_table, JitEngine::ChainSlot).
#include "chain_table.h"
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

struct Chained {
    CpuState cpu{};
    FlatMemory mem{0, 0x10000, &cpu};
    Core core{&cpu, &mem};
    explicit Chained(int opt, bool dry = false, bool host_cycles = false) {
        core.set_chaining(true, dry);
        core.set_host_cycles(host_cycles);
        require(core.init(opt), "init");
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

// Two blocks run each other through the table once both are compiled.
static void test_table_chain(int opt) {
    Chained c(opt);
    c.code(0x200, {kIncR0, 0x207f});  // R0 += 1; JUMP.S 0x300
    c.code(0x300, {kIncR0, 0x2f7f});  // R0 += 1; JUMP.S 0x200
    require(c.run(0x200, 100) == 2 && c.cpu.pc == 0x300, "0x300 not compiled yet: return");
    require(c.run(0x300, 100) == 100 && c.cpu.dpregs[0] == 51 && c.cpu.pc == 0x300, "chained through the table");
}

// A pending interrupt returns to the host (which would check it).
static void test_pending(int opt) {
    Chained c(opt);
    c.code(0x400, {kIncR0, 0x009e, 0x2ffe});  // R0 += 1; RAISE 14; JUMP.S 0x400
    require(c.run(0x400, 100) == 2 && c.cpu.chain_blocks == 0 && c.cpu.pc == 0x404, "pending after RAISE");
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
    for (int opt : {0, 1}) {
        test_table(opt);
        test_budget(opt);
        test_table_chain(opt);
        test_pending(opt);
        test_host_cycles(opt);
        test_dry(opt);
    }
    printf("chain test passed\n");
    return 0;
}
