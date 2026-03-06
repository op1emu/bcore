#pragma once

#include "cpu_state.h"
#include "mem.h"

extern "C" void bfin_syscall(CpuState* cpu, Memory* mem);
extern "C" void bfin_putchar(uint32_t ch);
