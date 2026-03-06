# cmake/BfinToolchain.cmake
# Locates Blackfin cross-toolchain programs used only in add_custom_command
# calls for assembling and linking test fixtures. NOT a CMake cross-compilation
# toolchain file — bfin-elf-gcc is never CMAKE_CXX_COMPILER.
#
# Cache inputs (user-settable):
#   BFIN_TOOLCHAIN_DIR  — directory searched first (default: empty, falls to PATH)
#
# Cache outputs set by find_program():
#   BFIN_GCC, BFIN_LD, BFIN_OBJDUMP — resolved paths, or NOTFOUND
#
# Output variable set by this module:
#   BFIN_TOOLCHAIN_FOUND — TRUE if all three tools were located, FALSE otherwise
#
# If any tool is missing, BFIN_TOOLCHAIN_FOUND is FALSE and a STATUS message
# is printed. The caller decides whether to skip or fatal-error.
#
# Note: find_program() caches its result. After moving the toolchain, force
# re-detection with:
#   cmake -UBFIN_GCC -UBFIN_LD -UBFIN_OBJDUMP -B <build-dir>

set(BFIN_TOOLCHAIN_DIR "" CACHE PATH
    "Directory containing bfin-elf-* binaries, searched before PATH and common \
locations. Example: -DBFIN_TOOLCHAIN_DIR=\$HOME/toolchains/bfin-elf/bin")

set(_bfin_hints
    "${BFIN_TOOLCHAIN_DIR}"
    "$ENV{HOME}/toolchains/bfin-elf/bin"
    "/opt/bfin-elf/bin"
    "/usr/local/bfin-elf/bin"
)

find_program(BFIN_GCC     NAMES bfin-elf-gcc     HINTS ${_bfin_hints} DOC "bfin-elf-gcc cross-compiler")
find_program(BFIN_LD      NAMES bfin-elf-ld      HINTS ${_bfin_hints} DOC "bfin-elf-ld cross-linker")
find_program(BFIN_OBJDUMP NAMES bfin-elf-objdump HINTS ${_bfin_hints} DOC "bfin-elf-objdump disassembly tool")
unset(_bfin_hints)

if(BFIN_GCC AND BFIN_LD AND BFIN_OBJDUMP)
    set(BFIN_TOOLCHAIN_FOUND TRUE)
    message(STATUS "Found bfin-elf-gcc:     ${BFIN_GCC}")
    message(STATUS "Found bfin-elf-ld:      ${BFIN_LD}")
    message(STATUS "Found bfin-elf-objdump: ${BFIN_OBJDUMP}")
else()
    set(BFIN_TOOLCHAIN_FOUND FALSE)
    set(_missing "")
    foreach(_tool BFIN_GCC BFIN_LD BFIN_OBJDUMP)
        if(NOT ${_tool})
            list(APPEND _missing "${_tool}")
        endif()
    endforeach()
    message(STATUS "Blackfin toolchain not found (missing: ${_missing}). "
        "Test assembly/link targets will be skipped. "
        "Set -DBFIN_TOOLCHAIN_DIR=<path/to/bfin-elf/bin> to enable them.")
    unset(_missing)
endif()
