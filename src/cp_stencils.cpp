// The copy-and-patch stencils (cp_format.h) bcore-stencilgen built: its
// code blob as bcore_cp::kCode, its big tables (kStencils, kRelocs,
// kIndex16, kKeys32, kSlots) from cp_tables.bin, the rest from cp_stencils.inc.

#include <cstdint>

#include "cp_format.h"
#include "cp_stencils.inc"

#if CP_STENCILS_AARCH64 != defined(__aarch64__)
#error "the stencils were made for another architecture (bcore-stencilgen --target)"
#endif

#if defined(__APPLE__)
#define CP_SYMBOL(name) "__ZN8bcore_cp" name "E"
#define CP_SECTION ".section __TEXT,__const"
#else
#define CP_SYMBOL(name) "_ZN8bcore_cp" name "E"
#define CP_SECTION ".section .rodata"
#endif
#define CP_STR2(...) #__VA_ARGS__
#define CP_STR(...) CP_STR2(__VA_ARGS__)
// A table: the symbol, then `count` bytes of the file from `skip`.
#define CP_TABLE(name, file, table) \
    ".p2align 4\n.globl " CP_SYMBOL(name) "\n" CP_SYMBOL(name) ":\n.incbin \"" file "\", " CP_STR(table) "\n"
__asm__(CP_SECTION "\n"
        ".p2align 4\n"
        ".globl " CP_SYMBOL("5kCode") "\n"
        CP_SYMBOL("5kCode") ":\n"
        ".incbin \"" CP_STENCILS_BIN "\"\n"
        ".byte 0\n"
        CP_TABLE("9kStencils", CP_TABLES_BIN, CP_TABLE_STENCILS)
        CP_TABLE("7kRelocs", CP_TABLES_BIN, CP_TABLE_RELOCS)
        CP_TABLE("8kIndex16", CP_TABLES_BIN, CP_TABLE_INDEX16)
        CP_TABLE("7kKeys32", CP_TABLES_BIN, CP_TABLE_KEYS32)
        CP_TABLE("6kSlots", CP_TABLES_BIN, CP_TABLE_SLOTS)
        ".text\n");
