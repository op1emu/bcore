// Inline access through Memory::page_table(): a load or store to a mapped page
// reads and writes the host page directly, one to an unmapped page calls the
// Memory, and both give the guest the same values.
#include "core.h"
#include "mmr.h"
#include "../src/evt.h"
#include "../src/cec.h"
#include "../tools/flat_memory.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// Two guest pages: 0x0000 is in the page table (when it is on), 0x1000 never
// is, so every access there is a call. Calls at or above kData count as data
// accesses; the code at 0x100 is fetched through read16 while translating.
class TwoPages : public Memory {
public:
    static constexpr uint32_t kData = 0x200;
    std::array<uint8_t, 2 * kPageSize> bytes{};
    std::vector<uint8_t*> pages;
    mutable int data_calls = 0;
    // The last value a write callback got, widened as the callee sees it: a
    // caller that leaves a narrow argument's upper bits as they are passes
    // the whole register to a callee that relies on them (Apple's arm64 ABI,
    // and clang's code on x86-64), which the bytes stored would hide.
    uint32_t written = 0;

    explicit TwoPages(bool table) {
        if (table) {
            pages.assign(kPageTableEntries, nullptr);
            pages[0] = bytes.data();
        }
    }
    uint32_t base() const override { return 0; }
    uint32_t size() const override { return bytes.size(); }
    const uint8_t* raw() const override { return bytes.data(); }
    uintptr_t fast_base() const override { return reinterpret_cast<uintptr_t>(bytes.data()); }
    uint32_t rawmem_limit() const override { return 0; }
    uint8_t* const* page_table() const override { return pages.empty() ? nullptr : pages.data(); }

    void count(uint32_t a) const { if (a >= kData) ++data_calls; }
    uint8_t read8(uint32_t a) const override { count(a); return bytes.at(a); }
    uint16_t read16(uint32_t a) const override { count(a); uint16_t v; memcpy(&v, &bytes.at(a), 2); return v; }
    uint32_t read32(uint32_t a) const override { count(a); uint32_t v; memcpy(&v, &bytes.at(a), 4); return v; }
    void write8(uint32_t a, uint8_t v) override { count(a); written = v; bytes.at(a) = v; }
    void write16(uint32_t a, uint16_t v) override { count(a); written = v; memcpy(&bytes.at(a), &v, 2); }
    void write32(uint32_t a, uint32_t v) override { count(a); written = v; memcpy(&bytes.at(a), &v, 4); }
};

struct Case {
    uint16_t op;       // LDST: R0 <-> [P0++]
    const char* name;
    bool write;
    unsigned width;
    bool sign;
};

static const Case kCases[] = {
    {0x9000, "R0 = [P0++]", false, 4, false},
    {0x9400, "R0 = W[P0++] (Z)", false, 2, false},
    {0x9440, "R0 = W[P0++] (X)", false, 2, true},
    {0x9800, "R0 = B[P0++] (Z)", false, 1, false},
    {0x9840, "R0 = B[P0++] (X)", false, 1, true},
    {0x9200, "[P0++] = R0", true, 4, false},
    {0x9600, "W[P0++] = R0", true, 2, false},
    {0x9a00, "B[P0++] = R0", true, 1, false},
};

// FlatMemory that calls for every access, or inlines its one RAM page.
class CallingFlat : public FlatMemory {
public:
    explicit CallingFlat(CpuState* cpu, bool table) : FlatMemory(0, kPageSize, cpu) {
        if (table) enable_page_table();
    }
    uint32_t rawmem_limit() const override { return 0; }
};

// A store at a loop bottom that unmasks a latched interrupt: the interrupt is
// taken after the loop's back edge, with RETI at the loop top, as when the
// epilog calls both helpers. (The block's own did_jump exit used to skip the
// check; bcore's emu does not check between blocks.)
static void test_interrupt_at_loop_bottom(int opt, bool table) {
    CpuState cpu{};
    CallingFlat mem(&cpu, table);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cpu_state_init(&cpu);
    mem.write16(0x100, 0x9200);  // [P0++] = R0
    mem.write16(0x102, 0x2000);  // JUMP.S 0
    evt_write(EVT_BASE + 4 * 15, 0x400);
    evt_write(EVT_BASE + 4 * 7, 0x300);
    // From reset into IVG15 with nesting enabled, IVG7 latched but masked.
    cec_mmr_write(&cpu, CEC_MMR_BASE + 4, 1u << 15);
    cec_raise(&cpu, 15);
    cec_return_rti(&cpu);
    cpu.did_jump = false;
    cec_push_reti(&cpu);
    cec_check_pending(&cpu);
    cpu.did_jump = false;
    cec_raise(&cpu, 7);
    cpu.lt[0] = 0x100; cpu.lb[0] = 0x100; cpu.lc[0] = 2;
    cpu.dpregs[8] = CEC_MMR_BASE + 4;           // P0 = &IMASK
    cpu.dpregs[0] = (1u << 7) | (1u << 15);     // unmask IVG7
    require(core.run(0x100), "loop block");
    if (cpu.pc != 0x300 || cpu.reti != 0x100) {
        fprintf(stderr, "FAIL: interrupt at loop bottom, O%d, table %d: pc 0x%x, reti 0x%x\n",
                opt, table, cpu.pc, cpu.reti);
        std::exit(1);
    }
}

// A Memory that does not promise callbacks_preserve_registers() may change a
// register in read32; the next instruction sees the new value.
class RegisterWriter : public TwoPages {
public:
    CpuState* cpu;
    RegisterWriter(CpuState* c, bool table) : TwoPages(table), cpu(c) {}
    uint32_t read32(uint32_t a) const override {
        if (a >= kData) cpu->dpregs[1] = 99;
        return TwoPages::read32(a);
    }
};

static void test_callback_writes_register(int opt, bool table) {
    CpuState cpu{};
    RegisterWriter mem(&cpu, table);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    mem.write16(0x100, 0x6009);  // R1 = 1
    mem.write16(0x102, 0x9000);  // R0 = [P0++]
    mem.write16(0x104, 0x3011);  // R2 = R1
    mem.write16(0x106, 0x2000);  // JUMP.S 0
    cpu.dpregs[8] = 0x1300;      // never in the table: always a call
    require(core.run(0x100), "callback block");
    if (cpu.dpregs[2] != 99) {
        fprintf(stderr, "FAIL: callback register write, O%d, table %d: R2 = %u\n", opt, table, cpu.dpregs[2]);
        std::exit(1);
    }
}

// Blocks compiled before cpu_state_init() still find the page table and the
// CEC's pending flag after it.
static void test_reset_between_runs(int opt) {
    CpuState cpu{};
    TwoPages mem(true);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cpu_state_init(&cpu);
    mem.write16(0x100, 0x9200);  // [P0++] = R0
    mem.write16(0x102, 0x2000);  // JUMP.S 0
    for (int run = 0; run < 2; ++run) {
        cpu_state_init(&cpu);
        cpu.dpregs[0] = 0x55667788;
        cpu.dpregs[8] = TwoPages::kData + 0x100;
        require(core.run(0x100), "block after reset");
        require(cpu.page_table == mem.pages.data(), "CpuState::page_table after reset");
        uint32_t stored;
        memcpy(&stored, &mem.bytes[TwoPages::kData + 0x100], 4);
        require(stored == 0x55667788, "store after reset");
    }
}

int main() {
    for (int opt : {0, 2}) {
        for (bool table : {false, true}) {
            test_interrupt_at_loop_bottom(opt, table);
            test_callback_writes_register(opt, table);
        }
        test_reset_between_runs(opt);
    }
    int count = 0;
    // Each block at tier 0 made of stencils too (with the table, which the
    // stencils need): their helper calls extend a narrow value as LLVM's do.
    for (int opt : {0, 2}) for (bool table : {false, true}) for (bool stencils : {false, true})
    for (uint32_t page : {0u, 0x1000u})
    for (const Case& c : kCases) {
        if (stencils && !table) continue;
        TwoPages mem(table);
        CpuState cpu{};
        Core core(&cpu, &mem);
        if (stencils) {
            core.set_tiering(0x7fffffff);
            core.set_copy_patch(true);
        }
        require(core.init(opt), "init");
        cec_init(); evt_init();
        mem.write16(0x100, c.op); mem.write16(0x102, 0x2000);  // op; JUMP.S 0
        const uint32_t addr = page + 0x300;
        mem.write32(addr, 0x8382f1f0);
        mem.data_calls = 0;
        cpu.dpregs[0] = 0x11223344;
        cpu.dpregs[8] = addr;
        require(core.run(0x100), c.name);

        const bool inline_access = table && page == 0;
        if (inline_access) require(cpu.page_table == mem.pages.data(), "CpuState::page_table set");
        if (mem.data_calls != (inline_access ? 0 : 1)) {
            fprintf(stderr, "FAIL: %s at 0x%x, table %d, O%d%s: %d calls\n", c.name, addr, table, opt,
                    stencils ? " stencils" : "", mem.data_calls);
            return 1;
        }
        require(cpu.dpregs[8] == addr + c.width, "post-increment");
        uint32_t stored;
        memcpy(&stored, &mem.bytes[addr], 4);
        if (c.write) {
            const uint32_t mask = c.width == 4 ? ~0u : (1u << (8 * c.width)) - 1;
            require(stored == ((0x8382f1f0 & ~mask) | (0x11223344 & mask)), "store");
            if (!inline_access && mem.written != (0x11223344 & mask)) {
                fprintf(stderr, "FAIL: %s at 0x%x, table %d, O%d%s: the callback got 0x%08x\n", c.name, addr, table,
                        opt, stencils ? " stencils" : "", mem.written);
                return 1;
            }
        } else {
            uint32_t want = c.width == 4 ? 0x8382f1f0 : c.width == 2 ? 0xf1f0 : 0xf0;
            if (c.sign) want = c.width == 2 ? uint32_t(int32_t(int16_t(want))) : uint32_t(int32_t(int8_t(want)));
            require(cpu.dpregs[0] == want, "loaded value");
            require(stored == 0x8382f1f0, "load leaves memory alone");
        }
#if BCORE_ENABLE_PROFILE && BCORE_COPY_PATCH_TEST
        if (stencils) require(core.stats().copy_patch_blocks == 1, "the block not made of stencils");
#endif
        ++count;
    }
    printf("PASS: %d page-table cases (mapped/unmapped page, table on/off, stencils, O0/O2), "
           "interrupt at a loop bottom, callback register write, reset between runs\n", count);
}
