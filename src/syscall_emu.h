#pragma once

#include "cpu_state.h"
#include "mem.h"

namespace bcore {

void bfin_syscall(CpuState* cpu, Memory* mem);
void bfin_putchar(uint32_t ch);

}  // namespace bcore
