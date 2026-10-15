// Inline access through Memory::page_table(): a load or store to a mapped page
// reads and writes the host page directly, one to an unmapped page calls the
// Memory, and both give the guest the same values.
#include "core.h"
#include "mmr.h"
#include "../src/evt.h"
#include "../src/cec.h"
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
    void write8(uint32_t a, uint8_t v) override { count(a); bytes.at(a) = v; }
    void write16(uint32_t a, uint16_t v) override { count(a); memcpy(&bytes.at(a), &v, 2); }
    void write32(uint32_t a, uint32_t v) override { count(a); memcpy(&bytes.at(a), &v, 4); }
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

int main() {
    int count = 0;
    for (int opt : {0, 2}) for (bool table : {false, true}) for (uint32_t page : {0u, 0x1000u})
    for (const Case& c : kCases) {
        TwoPages mem(table);
        CpuState cpu{};
        Core core(&cpu, &mem);
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
            fprintf(stderr, "FAIL: %s at 0x%x, table %d, O%d: %d calls\n", c.name, addr, table, opt, mem.data_calls);
            return 1;
        }
        require(cpu.dpregs[8] == addr + c.width, "post-increment");
        uint32_t stored;
        memcpy(&stored, &mem.bytes[addr], 4);
        if (c.write) {
            const uint32_t mask = c.width == 4 ? ~0u : (1u << (8 * c.width)) - 1;
            require(stored == ((0x8382f1f0 & ~mask) | (0x11223344 & mask)), "store");
        } else {
            uint32_t want = c.width == 4 ? 0x8382f1f0 : c.width == 2 ? 0xf1f0 : 0xf0;
            if (c.sign) want = c.width == 2 ? uint32_t(int32_t(int16_t(want))) : uint32_t(int32_t(int8_t(want)));
            require(cpu.dpregs[0] == want, "loaded value");
            require(stored == 0x8382f1f0, "load leaves memory alone");
        }
        ++count;
    }
    printf("PASS: %d page-table cases (mapped/unmapped page, table on/off, O0/O2)\n", count);
}
