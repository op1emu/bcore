#pragma once

// The helpers generated code reads and writes guest memory through
// (helper_abi.h): JitEngine registers each by its address under its name
// in kHelpers, so they need no C name of their own.

#include <cstdint>

#include "mem.h"

namespace bcore {

uint8_t  mem_read8(Memory* mem, uint32_t addr);
uint16_t mem_read16(Memory* mem, uint32_t addr);
uint32_t mem_read32(Memory* mem, uint32_t addr);
void     mem_write8(Memory* mem, uint32_t addr, uint8_t val);
void     mem_write16(Memory* mem, uint32_t addr, uint16_t val);
void     mem_write32(Memory* mem, uint32_t addr, uint32_t val);

}  // namespace bcore
