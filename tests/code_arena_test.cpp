// Compiled blocks share one arena (CodeArena): their code lies next to each
// other, not each at the start of pages of its own, and a new engine
// (Core::invalidate) gets a new arena that works the same.
#include "core.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr int kBlocks = 64;
static constexpr uint32_t kStride = 0x10;

// Block i: R1 = i; JUMP.S 0 (to itself: one block per run, chaining is off).
static void write_blocks(FlatMemory& mem) {
    for (int i = 0; i < kBlocks; ++i) {
        const uint32_t pc = 0x100 + i * kStride;
        mem.write16(pc, uint16_t(0x6000 | (i << 3) | 1));
        mem.write16(pc + 2, 0x2000);
    }
}

static void run_all(Core& core, CpuState& cpu) {
    for (int i = 0; i < kBlocks; ++i) {
        require(core.run(0x100 + i * kStride), "run");
        require(cpu.dpregs[1] == uint32_t(i), "R1 = i");
    }
}

static void require_packed(const Core& core) {
    std::vector<uintptr_t> addrs;
    core.forEachCompiledBlock([&](const BcoreBlockInfo& b) {
        addrs.push_back(reinterpret_cast<uintptr_t>(b.host_addr));
    });
    require(addrs.size() == size_t(kBlocks), "every block compiled");
    std::set<uintptr_t> pages;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    bool unaligned = false;
    for (uintptr_t a : addrs) {
        pages.insert(a >> 12);
        lo = std::min(lo, a);
        hi = std::max(hi, a);
        unaligned |= (a & 0xfff) != 0;
    }
    require(unaligned, "blocks do not all start a page");
    require(pages.size() < addrs.size() / 4, "blocks share pages");
    require(hi - lo < (uintptr_t(1) << 20), "blocks lie together");
}

static void test(int opt) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    write_blocks(mem);
    run_all(core, cpu);
    require_packed(core);
    run_all(core, cpu);  // compiled code runs again from the arena

    core.invalidate();
    run_all(core, cpu);
    require_packed(core);
}

int main() {
    for (int opt = 0; opt <= 2; ++opt) test(opt);
    printf("code arena test passed\n");
    return 0;
}
