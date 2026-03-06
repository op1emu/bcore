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
