// The copy-and-patch stencils (cp_format.h) bcore-stencilgen built: its
// tables, and its code blob as bcore_cp::kCode.

#include <cstdint>

#include "cp_format.h"

#if defined(__APPLE__)
#define CP_SYMBOL "__ZN8bcore_cp5kCodeE"
#define CP_SECTION ".section __TEXT,__const"
#else
#define CP_SYMBOL "_ZN8bcore_cp5kCodeE"
#define CP_SECTION ".section .rodata"
#endif
__asm__(CP_SECTION "\n"
        ".p2align 4\n"
        ".globl " CP_SYMBOL "\n"
        CP_SYMBOL ":\n"
        ".incbin \"" CP_STENCILS_BIN "\"\n"
        ".byte 0\n"
        ".text\n");

#include "cp_stencils.inc"
