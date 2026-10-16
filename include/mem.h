#pragma once

#include <cstdint>

class Memory {
public:
    virtual ~Memory() = default;

    virtual uint32_t base() const = 0;
    virtual uint32_t size() const = 0;

    virtual uint8_t  read8(uint32_t addr) const = 0;
    virtual uint16_t read16(uint32_t addr) const = 0;
    virtual uint32_t read32(uint32_t addr) const = 0;

    virtual void write8(uint32_t addr, uint8_t val) = 0;
    virtual void write16(uint32_t addr, uint16_t val) = 0;
    virtual void write32(uint32_t addr, uint32_t val) = 0;

    // Host records fault MMRs before the core enters EVT3. No CPLB model is implied.
    virtual void data_alignment_fault(uint32_t pc, uint32_t addr, uint32_t status) {
        (void)pc; (void)addr; (void)status;
    }
    virtual const uint8_t* raw() const = 0;

    // Host address corresponding to emulator address 0x0.
    // In fastmem mode: (uintptr_t)mmap_base
    // In normal mode: (uintptr_t)data_.data() - base_
    virtual uintptr_t fast_base() const = 0;

    // Returns true if memory uses direct host-address (mmap) mode.
    // When true, JIT emits direct loads/stores using fast_base().
    virtual bool is_fastmem() const { return false; }

    // Upper bound for the inline (non-fastmem) fast-path: addresses below this
    // value can be accessed directly via fast_base() + addr without an extern call.
    // Returns 0 in fastmem mode (not needed there).
    virtual uint32_t rawmem_limit() const { return is_fastmem() ? 0u : (base() + size()); }

    // Guest page table for inline access when neither fastmem nor rawmem
    // applies: kPageTableEntries entries, one per kPageSize-byte guest page,
    // each the host address of that page's first byte, or null when the page
    // must go through read*/write* (MMRs, devices with side effects, unmapped
    // space). An aligned 16- or 32-bit access never crosses a page, and every
    // access the JIT makes inline is aligned (a misaligned one raises
    // EXCAUSE 0x24 first). Return null to make every access a call.
    //
    // The table's address must stay the same for the Memory's lifetime; its
    // entries may change only while no compiled block is running, since the
    // JIT reads an entry once per access. The JIT samples this when it
    // translates a block: a block translated while it returns null calls
    // read*/write* for every access, so a host can turn inline access off
    // (to count every access, say) before translating.
    static constexpr unsigned kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    static constexpr uint32_t kPageTableEntries = 1u << (32 - kPageBits);
    virtual uint8_t* const* page_table() const { return nullptr; }

    // Whether read*/write* leave the guest registers alone: the data, pointer,
    // DAG and loop registers, RETS, the accumulators and the ASTAT bits in
    // CpuState. They may still raise an exception (which sets did_jump; the
    // block leaves right after the call) or latch an interrupt. Return true
    // only if every callback keeps to that: generated code then reuses register
    // values it loaded before an access instead of reloading them after it. The
    // JIT samples this when it translates a block, like page_table().
    virtual bool callbacks_preserve_registers() const { return false; }
};

// extern "C" wrappers for JIT-generated code
extern "C" {
    uint8_t  mem_read8(Memory* mem, uint32_t addr);
    uint16_t mem_read16(Memory* mem, uint32_t addr);
    uint32_t mem_read32(Memory* mem, uint32_t addr);
    void     mem_write8(Memory* mem, uint32_t addr, uint8_t val);
    void     mem_write16(Memory* mem, uint32_t addr, uint16_t val);
    void     mem_write32(Memory* mem, uint32_t addr, uint32_t val);
}
