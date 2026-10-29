// The copy-and-patch translator itself, without Core: a block whose packets
// all have stencils is made of them (CpTranslator::translate places code),
// and that code does what the block does. The other tests see a block made
// of stencils only through a profiling counter (copy_patch_blocks), which a
// build without BCORE_ENABLE_PROFILE -- WebAssembly's -- does not keep: there
// they would pass all the same were every block compiled with LLVM (found in
// review).
#include "../src/cp_translator.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

using namespace bcore;

static void require(bool ok, const std::string& why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why.c_str()); std::exit(1); }
}

static constexpr uint32_t kPc = 0x100;
static constexpr uint32_t kReturn = 0x8000;  // RETS
static constexpr uint32_t kTierOnly = 1000000000;  // tier 0 throughout

struct Setup {
    CpuState cpu{};
    FlatMemory mem{0, 0x10000, &cpu};
    JitEngine jit;
    Setup() {
        mem.enable_page_table();  // as copy-and-patch needs
        require(jit.init(), "jit init");
        cpu.chain_table = jit.chainTable();
        cpu.page_table = mem.page_table();
        cpu.rets = kReturn;
        cpu.pc = kPc;
        cec_init(); evt_init();
    }
    void code(std::initializer_list<uint16_t> words) {
        uint32_t at = kPc;
        for (uint16_t w : words) { mem.write16(at, w); at += 2; }
    }
};

// 16-bit, 32-bit and parallel packets, a load through the page table, RTS.
static void test_packets() {
    Setup s;
    s.code({0x6008,                          // R0 = 1
            0x6408,                          // R0 += 1
            0xe101, 0x1234,                  // R1.L = 0x1234
            0xcc04, 0x000a, 0x9003, 0x0000,  // R0 = R1 + R2 || R3 = [P0++] || NOP
            0x0010});                        // RTS
    s.mem.write32(0x2000, 0xcafef00d);
    s.cpu.dpregs[1] = 0x5670000;
    s.cpu.dpregs[2] = 2;
    s.cpu.dpregs[8] = 0x2000;  // P0
    CpTranslator cp(s.jit);
    CpTranslator::Config config;
    config.threshold = kTierOnly;
    const CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
    require(block.fn, std::string("no block made of stencils: ") + (block.why ? block.why : "?"));
    require(block.packets == 5, "five packets");
    s.jit.enter(block.fn, &s.cpu, &s.mem);
    require(s.cpu.dpregs[1] == 0x5671234, "R1.L");
    require(s.cpu.dpregs[0] == 0x5671236, "R0 = R1 + R2");
    require(s.cpu.dpregs[3] == 0xcafef00d && s.cpu.dpregs[8] == 0x2004, "R3 = [P0++]");
    require(s.cpu.pc == kReturn && s.cpu.packets == 5, "returned after five packets");
}

// A block the packet cap cuts goes on to the next PC.
static void test_cap() {
    for (unsigned cap : {1u, 2u, 31u, 64u, 256u}) {
        Setup s;
        for (unsigned n = 0; n < 64; ++n) s.mem.write16(kPc + 2 * n, 0x6408);  // R0 += 1
        s.mem.write16(kPc + 128, 0x0010);  // RTS
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.max_packets = cap;
        const CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        const std::string what = "cap " + std::to_string(cap);
        require(block.fn, what + ": no block made of stencils");
        s.jit.enter(block.fn, &s.cpu, &s.mem);
        const unsigned adds = cap < 64 ? cap : 64;
        require(s.cpu.dpregs[0] == adds, what + ": R0");
        require(s.cpu.pc == (cap <= 64 ? kPc + 2 * cap : kReturn), what + ": next PC");
    }
}

// A block that jumps to itself: through the chain table, its own exit, or
// back to the host, as configured.
static void test_loop() {
    for (bool chaining : {false, true})
    for (bool self_loop : {false, true})
    for (bool dry : {false, true})
    for (bool counted : {false, true}) {
        Setup s;
        s.code({0x6408, 0x2fff});  // R0 += 1; JUMP.S 0x100
        CpTranslator cp(s.jit);
        CpTranslator::Config config;
        config.threshold = kTierOnly;
        config.chaining = chaining;
        config.self_loop = self_loop;
        config.dry = dry;
        config.count_chained = counted;
        const CpTranslator::Block block = cp.translate(kPc, s.mem, 0, config);
        const std::string what = std::string(chaining ? "chained" : "unchained") + (self_loop ? " self" : "") +
                                 (dry ? " dry" : "") + (counted ? " counted" : "");
        require(block.fn, what + ": no block made of stencils");
        s.jit.addCode(kPc, block.fn, block.fallthrough_pc);
        s.cpu.chain_budget = 7;
        s.jit.enter(block.fn, &s.cpu, &s.mem);
        const unsigned runs = chaining && !dry ? 4 : 1;
        require(s.cpu.dpregs[0] == runs && s.cpu.pc == kPc, what + ": runs");
        require(s.cpu.packets + s.cpu.chain_packets == 2 * runs, what + ": packets");
        require(s.cpu.chain_blocks == (counted ? runs - 1 : 0), what + ": chained blocks");
    }
}

int main() {
    test_packets();
    test_cap();
    test_loop();
    printf("PASS\n");
    return 0;
}
