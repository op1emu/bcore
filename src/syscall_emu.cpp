#include "syscall_emu.h"

#include <cstdio>
#include <unistd.h>

// Libgloss syscall convention:
//   P0 = syscall number
//   R0 = pointer to params array (4 x uint32_t)
//
// Syscall 1: exit(status)
//   params[0] = exit code
//
// Syscall 5: write(fd, buf, count)
//   params[0] = fd
//   params[1] = buf pointer
//   params[2] = count

extern "C" void bfin_putchar(uint32_t ch) {
    putchar((int)ch);
}

extern "C" void bfin_syscall(CpuState* cpu, Memory* mem) {
    uint32_t syscall_nr = cpu->dpregs[8]; // P0
    uint32_t params_ptr = cpu->dpregs[0]; // R0

    switch (syscall_nr) {
    case 1: { // exit
        uint32_t status = mem->read32(params_ptr);
        cpu->halted = true;
        cpu->exit_code = static_cast<int>(status);
        break;
    }
    case 5: { // write
        uint32_t fd    = mem->read32(params_ptr);
        uint32_t buf   = mem->read32(params_ptr + 4);
        uint32_t count = mem->read32(params_ptr + 8);

        // Batch write: compute offset into raw memory and write all at once
        uint32_t offset = buf - mem->base();
        if (offset + count > mem->size()) {
            fprintf(stderr, "bfin_syscall write: out of bounds buf=0x%08x count=%u\n", buf, count);
            cpu->halted = true;
            cpu->exit_code = 1;
            break;
        }
        ssize_t written = ::write(static_cast<int>(fd), mem->raw() + offset, count);
        // Return actual bytes written (or -1 on error) in R0
        cpu->dpregs[0] = (written < 0) ? static_cast<uint32_t>(-1) : static_cast<uint32_t>(written);
        break;
    }
    default:
        fprintf(stderr, "bfin_syscall: unimplemented syscall %u at PC=0x%08x\n",
                syscall_nr, cpu->pc);
        cpu->halted = true;
        cpu->exit_code = 1;
        break;
    }
}
