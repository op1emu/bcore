// The chain table: which compiled blocks generated code may run next itself
// (CpuState::chain_table, JitEngine::ChainSlot).
#include "chain_table.h"
#include "core.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <cstdio>
#include <cstdlib>

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

int main() {
    for (int opt : {0, 1}) test_table(opt);
    printf("chain test passed\n");
    return 0;
}
