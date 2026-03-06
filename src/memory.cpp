#include "mem.h"

extern "C" {

__attribute__((hot)) uint8_t mem_read8(Memory* mem, uint32_t addr) {
    return mem->read8(addr);
}

__attribute__((hot)) uint16_t mem_read16(Memory* mem, uint32_t addr) {
    return mem->read16(addr);
}

__attribute__((hot)) uint32_t mem_read32(Memory* mem, uint32_t addr) {
    return mem->read32(addr);
}

__attribute__((hot)) void mem_write8(Memory* mem, uint32_t addr, uint8_t val) {
    mem->write8(addr, val);
}

__attribute__((hot)) void mem_write16(Memory* mem, uint32_t addr, uint16_t val) {
    mem->write16(addr, val);
}

__attribute__((hot)) void mem_write32(Memory* mem, uint32_t addr, uint32_t val) {
    mem->write32(addr, val);
}

} // extern "C"
