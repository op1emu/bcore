#pragma once

#include "cpu_state.h"
#include "mem.h"

namespace bcore {

extern "C" void bfin_syscall(CpuState* cpu, Memory* mem);
extern "C" void bfin_putchar(uint32_t ch);

}  // namespace bcore
