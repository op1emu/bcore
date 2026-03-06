# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Blackfin DSP (Digital Signal Processor) recompiler and disassembler project. It lifts Blackfin instructions to LLVM IR with JIT execution.

- **Target Architecture**: Analog Devices Blackfin processor
- **Implementation**: `src/` — C++ implementation (in progress)

## Ground-Truth Reference

Instruction semantics and test fixtures come from the [op1emu/bfin_sim](https://github.com/op1emu/bfin_sim) repository, which contains:

- `sim/bfin-sim.c` — complete simulator (~45K lines), authoritative for instruction semantics
- `sim/bfin-dis.c` — complete disassembler (~33K lines)
- `testsuite/` — assembly test cases (fetched at CMake configure time into `build/bfin_testsuite_src/`)

When implementing or verifying instruction semantics, consult `bfin-sim.c` in that repo.

## Architecture

### Instruction Decoding System

The C++ implementation uses a sophisticated template-based pattern matching system:

1. **Template-Based Instruction Matching**: `src/decoder.h` implements `InstructionMatcher` class that uses C++ templates to match binary instruction patterns at compile time.

2. **Visitor Pattern**: The decoder uses the visitor pattern - visitors implement `decode_<OPCODE>` methods that get called when matching instructions are found.

3. **Binary Pattern Matching**: Instructions are defined using binary string patterns (e.g., `"010000 0000 ddd ddd"` where `d` represents register fields).

4. **Dual Instruction Set**:
   - 16-bit instructions: Standard operations (`src/instruction16.def.h`)
   - 32-bit instructions: DSP operations (`src/instruction32.def.h`)

### Key Components

- `src/decoder.h`: Core template-based instruction decoder
- `src/instruction16.def.h`: 16-bit instruction pattern definitions
- `src/instruction32.def.h`: 32-bit instruction pattern definitions
- `src/disasm_visitor.h`: Visitor interface for instruction disassembly
- `src/disasm_visitor.cpp`: Implementation of disassembly visitor methods
- `src/cpu_state.h/cpp`: CpuState struct (dpregs, accumulators, ASTAT flags, loops), ASTAT compose/decompose, extern "C" wrappers for JIT
- `src/memory.cpp`: Flat memory model (little-endian), extern "C" read/write wrappers for JIT
- `src/lift_visitor.h/cpp`: LiftVisitor — plugs into decoder, emits LLVM IR per instruction (~24 implemented, rest are stubs)
- `src/jit_engine.h/cpp`: LLJIT wrapper with explicit `absoluteSymbols` registration for all extern "C" helpers
- `src/bb_translator.h/cpp`: Basic-block translator — decodes instructions until terminator, produces one LLVM function per BB
- `src/syscall_emu.h/cpp`: Libgloss syscall emulation (exit, write)
- `tools/emu.cpp`: ELF loader, JIT execution loop, hardware loop support, CLI options (`--trace`, `--dump`, `--max-steps`, `--opt-level`)

## Common Development Tasks

### Adding New Instructions

1. Determine if it's a 16-bit or 32-bit instruction
2. Add pattern to appropriate `.def.h` file using `DEF_INSN` macro:
   ```cpp
   DEF_INSN(Category_OpcodeName, "assembly syntax", "binary pattern")
   ```
3. Implement visitor method in your visitor class:
   ```cpp
   bool decode_Category_OpcodeName(ArgType arg1, ArgType arg2) { ... }
   ```

### Running the Disassembler

```bash
./build/disasm <elf-file>
```

The disassembler:
1. Reads an ELF file
2. Finds the `.text` section
3. Decodes instructions using the visitor pattern
4. Prints disassembly output

### Running the Emulator

```bash
./build/emu [--trace] [--dump] [--max-steps N] [--opt-level/-O N] <elf-file>
```

The emulator:
1. Loads an ELF32 Blackfin executable into flat memory
2. Translates basic blocks to LLVM IR on first encounter (lazy JIT)
3. Executes JIT'd native code, handling branches and hardware loops in the host loop
4. Emulates libgloss syscalls (exit, write) via EXCPT 0

**Options:**
- `--trace`: Print PC and disassembly of each instruction before each BB execution
- `--dump`: Print generated LLVM IR per basic block (after optimization)
- `--max-steps N`: Stop after N basic-block steps (0 = unlimited, default)
- `--opt-level N` / `-O N`: LLVM optimization level 0–3 (default: 2)

### Adding Emulator Support for New Instructions

1. Find the stub in `src/lift_visitor.cpp` (search for `STUB_` macros)
2. Replace the stub with an implementation that emits LLVM IR via `builder_`
3. Use helpers (see `src/lift_visitor.h` private section for full list):
   - **Register access**: `load_dreg/preg`, `store_dreg/preg`, `load_cpu_u32/store_cpu_u32`
   - **Flag epilogs**: `emit_flags_logic(result)` — AZ/AN + clear AC0/V (logical ops); `emit_flags_az_an(result)` — AZ/AN only; `emit_flags_arith(result, v, ac0)` — AZ/AN/V/VS/AC0 (arithmetic)
   - **Memory access**: `emit_mem_read("mem_read32", builder_.getInt32Ty(), addr)` / `emit_mem_write("mem_write32", addr, val)` — also mem_read8/16, mem_write8/16
   - **Control flow**: `emit_jump`, `emit_jump_imm`, `call_extern`
   - **Accumulators**: `build_acc_i64(ax, aw)`, `emit_acc_abs(src, dst)`
4. Reference `bfin-sim.c` in https://github.com/op1emu/bfin_sim for ground-truth semantics
5. Test: `./build/emu build/test_linked/<test>.S.elf`


## Code Style

- **C++ Standard**: C++17 (uses template metaprogramming heavily)
- **Pattern**: Visitor pattern for instruction decoding
- **Templates**: Extensive use of C++ templates for compile-time pattern matching
- **Binary Patterns**: Instructions defined using binary string patterns with field markers
- **Prefer LLVM intrinsics over extern "C" helpers.**

## Testing

- `tests/run_comparison_test.py`: Python script to compare disassembly output against `bfin-elf-objdump`
  run with `python3 tests/run_comparison_test.py`
- Emulator tests: assembly files from https://github.com/op1emu/bfin_sim/tree/main/testsuite/ are fetched at configure time, assembled, linked to `.elf`, and run via `./build/emu`

## Experiences
- Store experiences in `EXPERIENCES.md` to track insights, challenges, and solutions encountered during development. (Optional but recommended for knowledge sharing)

## Important Notes

- Build with CMake: `cmake -B build && cmake --build build`
- Requires LLVM 15 (set `LLVM_DIR` in CMakeLists.txt if not at default path)
- Requires `bfin-elf-gcc` and `bfin-elf-ld` at `$HOME/toolchains/bfin-elf/bin/` for test assembly/linking
- JIT extern "C" symbols are registered explicitly via `absoluteSymbols` in `jit_engine.cpp` — do NOT rely on `-rdynamic` / `DynamicLibrarySearchGenerator`
