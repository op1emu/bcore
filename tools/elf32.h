#pragma once

// The 32-bit ELF definitions the tools read a Blackfin program with: the C
// library's <elf.h> where there is one, the same definitions where there
// is none (macOS).

#if __has_include(<elf.h>)
#include <elf.h>
#else
#include <cstdint>

using Elf32_Half = uint16_t;
using Elf32_Word = uint32_t;
using Elf32_Addr = uint32_t;
using Elf32_Off = uint32_t;

constexpr int EI_NIDENT = 16;
constexpr int EI_MAG0 = 0;
constexpr int EI_MAG1 = 1;
constexpr int EI_MAG2 = 2;
constexpr int EI_MAG3 = 3;
constexpr int EI_CLASS = 4;
constexpr int EI_DATA = 5;
constexpr unsigned char ELFMAG0 = 0x7f;
constexpr unsigned char ELFMAG1 = 'E';
constexpr unsigned char ELFMAG2 = 'L';
constexpr unsigned char ELFMAG3 = 'F';
constexpr unsigned char ELFCLASS32 = 1;
constexpr unsigned char ELFDATA2LSB = 1;
constexpr Elf32_Word PT_LOAD = 1;
constexpr Elf32_Word SHT_NOBITS = 8;
constexpr Elf32_Word SHF_ALLOC = 0x2;
constexpr Elf32_Word SHF_EXECINSTR = 0x4;

struct Elf32_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
};

struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

struct Elf32_Shdr {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
};

static_assert(sizeof(Elf32_Ehdr) == 52 && sizeof(Elf32_Phdr) == 32 && sizeof(Elf32_Shdr) == 40,
              "the ELF32 layouts");
#endif
