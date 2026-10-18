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
#include <map>
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

// The code map lists a block's function and, where the machine function
// splitter made one, its cold part (with profiling on; without, the
// functions only). The functions lie together; cold parts lie apart from
// them, in the arena's cold region.
static void require_packed(const Core& core, int opt) {
    std::map<uint32_t, std::vector<uintptr_t>> by_pc;
    core.forEachCompiledBlock([&](const BcoreBlockInfo& b) {
        by_pc[b.guest_pc].push_back(reinterpret_cast<uintptr_t>(b.host_addr));
    });
    require(by_pc.size() == size_t(kBlocks), "every block compiled");
    std::set<uintptr_t> pages;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    bool unaligned = false;
    std::vector<uintptr_t> cold;
    for (auto& [pc, addrs] : by_pc) {
        require(addrs.size() <= 2, "a function and at most one cold part");
        std::sort(addrs.begin(), addrs.end());
        const uintptr_t a = addrs[0];
        pages.insert(a >> 12);
        lo = std::min(lo, a);
        hi = std::max(hi, a);
        unaligned |= (a & 0xfff) != 0;
        if (addrs.size() == 2) cold.push_back(addrs[1]);
    }
    require(unaligned, "blocks do not all start a page");
    require(pages.size() < by_pc.size() / 4, "blocks share pages");
    require(hi - lo < (uintptr_t(1) << 20), "blocks lie together");
    // A jump's rarely taken loop-bottom path is cold in every one of them;
    // the backend splits from CodeGenOpt::Less up.
    if (opt > 0 && core.profileHasBlockSizes()) require(cold.size() == size_t(kBlocks), "the splitter made cold parts");
    for (uintptr_t c : cold) require(c - lo >= (uintptr_t(256) << 20), "cold parts lie apart");
}

static void test(int opt) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    write_blocks(mem);
    run_all(core, cpu);
    require_packed(core, opt);
    run_all(core, cpu);  // compiled code runs again from the arena

    core.invalidate();
    run_all(core, cpu);
    require_packed(core, opt);
}

int main() {
    for (int opt = 0; opt <= 2; ++opt) test(opt);
    printf("code arena test passed\n");
    return 0;
}
