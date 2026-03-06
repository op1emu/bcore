#include <elf.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "core.h"
#include "flat_memory.h"

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto len = f.tellg();
    if (len <= 0) return false;
    out.resize(static_cast<size_t>(len));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), len);
    return f.good();
}

int main(int argc, char** argv) {
    bool trace = false;
    bool dump_ir = false;
    bool fastmem = false;
    uint64_t max_steps = 0; // 0 = unlimited
    int opt_level = 2;      // 0=None, 1=Less, 2=Default, 3=Aggressive
    const char* elf_path = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--dump") == 0) {
            dump_ir = true;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], nullptr, 0);
        } else if ((strcmp(argv[i], "--opt-level") == 0 || strcmp(argv[i], "-O") == 0) && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
            if (opt_level < 0 || opt_level > 3) {
                fprintf(stderr, "error: --opt-level/-O must be 0-3\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--fastmem") == 0) {
            fastmem = true;
        } else {
            elf_path = argv[i];
        }
    }

    if (!elf_path) {
        fprintf(stderr, "usage: emu [--trace] [--dump] [--fastmem] [--max-steps N] [--opt-level/-O N] <elf-file>\n");
        return 1;
    }

    std::vector<uint8_t> buf;
    if (!read_file(elf_path, buf)) {
        fprintf(stderr, "failed to read file: %s\n", elf_path);
        return 1;
    }

    if (buf.size() < sizeof(Elf32_Ehdr)) {
        fprintf(stderr, "file too small\n");
        return 1;
    }

    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(buf.data());
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "unsupported ELF\n");
        return 1;
    }

    // Find loadable sections and determine memory range
    const uint32_t shoff = eh->e_shoff;
    const uint16_t shentsize = eh->e_shentsize;
    const uint16_t shnum = eh->e_shnum;
    const uint16_t phnum = eh->e_phnum;
    const uint32_t phoff = eh->e_phoff;
    const uint16_t phentsize = eh->e_phentsize;

    // Determine memory range from program headers
    uint32_t mem_lo = 0xFFFFFFFF;
    uint32_t mem_hi = 0;

    if (phnum > 0) {
        for (uint16_t i = 0; i < phnum; i++) {
            auto* ph = reinterpret_cast<const Elf32_Phdr*>(buf.data() + phoff + i * phentsize);
            if (ph->p_type == PT_LOAD) {
                uint32_t lo = ph->p_vaddr;
                uint32_t hi = ph->p_vaddr + ph->p_memsz;
                if (lo < mem_lo) mem_lo = lo;
                if (hi > mem_hi) mem_hi = hi;
            }
        }
    } else {
        // Fall back to section headers
        for (uint16_t i = 0; i < shnum; i++) {
            auto* sh = reinterpret_cast<const Elf32_Shdr*>(buf.data() + shoff + i * shentsize);
            if (sh->sh_flags & SHF_ALLOC) {
                uint32_t lo = sh->sh_addr;
                uint32_t hi = sh->sh_addr + sh->sh_size;
                if (lo < mem_lo) mem_lo = lo;
                if (hi > mem_hi) mem_hi = hi;
            }
        }
    }

    if (mem_lo >= mem_hi) {
        fprintf(stderr, "no loadable segments\n");
        return 1;
    }

    // Allocate memory with extra room for stack
    // Stack grows down from 0x08000000
    uint32_t stack_top = 0x08000000;
    uint32_t stack_size = 0x00100000; // 1MB stack
    uint32_t stack_base = stack_top - stack_size;

    uint32_t total_lo = (mem_lo < stack_base) ? mem_lo : stack_base;
    uint32_t total_hi = (mem_hi > stack_top) ? mem_hi : stack_top;
    uint32_t total_size = total_hi - total_lo;

    // Initialize CPU state
    CpuState cpu;
    cpu_state_init(&cpu);
    FlatMemory memory(total_lo, total_size, fastmem, &cpu);

    // Load sections into memory
    if (phnum > 0) {
        for (uint16_t i = 0; i < phnum; i++) {
            auto* ph = reinterpret_cast<const Elf32_Phdr*>(buf.data() + phoff + i * phentsize);
            if (ph->p_type == PT_LOAD && ph->p_filesz > 0) {
                memory.load(ph->p_vaddr, buf.data() + ph->p_offset, ph->p_filesz);
            }
        }
    } else {
        for (uint16_t i = 0; i < shnum; i++) {
            auto* sh = reinterpret_cast<const Elf32_Shdr*>(buf.data() + shoff + i * shentsize);
            if ((sh->sh_flags & SHF_ALLOC) && sh->sh_type != SHT_NOBITS && sh->sh_size > 0) {
                memory.load(sh->sh_addr, buf.data() + sh->sh_offset, sh->sh_size);
            }
        }
    }

    // Find .text section for code boundaries
    uint32_t text_base = mem_lo;
    uint32_t text_size = mem_hi - mem_lo;

    cpu.pc = eh->e_entry;
    cpu.dpregs[14] = stack_top; // SP
    cpu.ksp = stack_top;        // KSP for user→supervisor stack swap
    cec_set_mem(&memory);       // register memory for libgloss syscall fallback
    // Set step limit: 0 = unlimited, N = execute up to N instructions
    cpu.steps_remaining = (max_steps == 0 || max_steps > 0xFFFFFFFFULL)
                          ? 0u
                          : static_cast<uint32_t>(max_steps);

    // Initialize Core (JIT + translator)
    Core core(&cpu, &memory);
    if (dump_ir) core.set_dump_ir(true);
    if (!core.init(opt_level)) {
        fprintf(stderr, "failed to init JIT\n");
        return 1;
    }

    // Wire JIT cache invalidation: writes to the code region evict overlapping BBs.
    memory.set_write_notify(
        [&core](uint32_t addr, uint32_t size) { core.invalidate(addr, size); },
        text_base, text_base + text_size);

    if (trace) {
        auto [text, next_pc] = core.disassemble(cpu.pc);
        fprintf(stderr, "  %08x:  %s\n", cpu.pc, text.c_str());
        fprintf(stderr, "Entry: 0x%08x, SP: 0x%08x\n", cpu.pc, cpu.dpregs[14]);
    }

    // Main execution loop
    while (!cpu.halted) {
        cpu.steps_remaining = static_cast<uint32_t>(max_steps);
        if (!core.run(cpu.pc)) {
            fprintf(stderr, "failed to run BB at 0x%08x\n", cpu.pc);
            return 1;
        }
        if (trace) {
            auto [text, next_pc] = core.disassemble(cpu.pc);
            fprintf(stderr, "  %08x:  %s\n", cpu.pc, text.c_str());
            fprintf(stderr, "PC=0x%08x R0=0x%08x R1=0x%08x R2=0x%08x P0=0x%08x SP=0x%08x CC=%u\n",
                    cpu.pc, cpu.dpregs[0], cpu.dpregs[1], cpu.dpregs[2],
                    cpu.dpregs[8], cpu.dpregs[14], cpu.cc);
        }
    }

    return cpu.exit_code;
}
