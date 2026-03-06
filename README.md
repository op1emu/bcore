# bcore

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Blackfin DSP JIT recompiler and emulator — built by vibe coding with [Claude Code](https://claude.ai/code).

---

## Overview

**bcore** lifts [Analog Devices Blackfin](https://en.wikipedia.org/wiki/Blackfin) ELF32 executables to LLVM IR and executes them natively via LLVM LLJIT. It also includes a standalone disassembler that matches `bfin-elf-objdump` output.

- Decodes both 16-bit and 32-bit Blackfin instructions using a template-based binary pattern matcher
- JIT-compiles one basic block at a time; newly translated blocks are cached and reused
- Emulates the Core Event Controller (CEC): interrupts, exceptions, supervisor/user mode transitions
- Emulates libgloss `EXCPT 0` syscalls (exit, write) for running bare-metal test binaries

---

## Architecture

```
ELF binary
   │
   ▼
FlatMemory ──────────────► CEC / EVT (MMR dispatch)
   │
   ▼
BBTranslator
   │
   ├── decoder.h  (template binary-pattern matcher)
   │       │
   │       ▼
   │   LiftVisitor  ──► llvm::IRBuilder emits IR per instruction
   │
   ▼
JitEngine (LLVM LLJIT)
   │
   ▼
Native code  ◄──► extern "C" helpers (mem_read/write, CEC, hwloop)
```

### Key Components

| File | Description |
|---|---|
| `src/decoder.h` | Template-based binary pattern matching engine; calls `decode_*` visitor methods |
| `src/instruction16/32.def.h` | All Blackfin instruction patterns as `DEF_INSN` macro tables |
| `src/lift_visitor.cpp` | ~7800 lines; emits LLVM IR for each instruction |
| `src/disasm_visitor.cpp` | ~2800 lines; produces `objdump`-compatible disassembly text |
| `src/bb_translator.cpp` | Drives instruction-by-instruction decode until a terminator; one LLVM function per BB |
| `src/jit_engine.cpp` | LLVM LLJIT wrapper; explicit `absoluteSymbols` registration; JIT cache with write-triggered invalidation |
| `src/cec.cpp` | Core Event Controller (interrupts, exceptions, RTI/RTX/RTN/RTE, CLI/STI, supervisor mode) |
| `src/evt.cpp` | Exception Vector Table device (EVT0–EVT15) |
| `include/core.h` | Public facade: `Core::init()`, `run(pc)`, `invalidate()`, `disassemble()` |
| `tools/flat_memory.h` | Concrete `Memory`: vector-backed or `mmap`-at-0 (fastmem); write-notify for JIT invalidation |

**Parallel instruction support:** bcore implements the Blackfin parallel-issue slot rules (Group A/B/C constraints and cross-slot conflict detection) and raises `VEC_ILGAL_I` on violations, matching hardware behavior.

---

## Prerequisites

| Dependency | Notes |
|---|---|
| CMake ≥ 3.10 | |
| C++17 compiler | GCC or Clang |
| LLVM 15 | `llvm-15` + `llvm-dev` packages, or built from source |
| `bfin-elf-gcc` / `bfin-elf-ld` | Optional — only required to assemble and run the test suite |

**Installing LLVM 15 on Ubuntu/Debian:**
```bash
wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 15
```

**Blackfin cross-toolchain** is searched in this order:
1. CMake variable `BFIN_TOOLCHAIN_DIR`
2. `$HOME/toolchains/bfin-elf/bin`
3. `/opt/bfin-elf/bin`
4. System `PATH`

If not found, the test assembly and link targets are silently skipped (the disassembler and emulator still build).

---

## Quick Start

```bash
# Clone and build
git clone <repo-url> bcore && cd bcore
cmake -B build && cmake --build build

# With explicit LLVM path (if cmake can't find it automatically):
cmake -B build -DLLVM_DIR=/usr/lib/llvm-15/lib/cmake/llvm

# With explicit cross-toolchain location:
cmake -B build -DBFIN_TOOLCHAIN_DIR=$HOME/toolchains/bfin-elf/bin
```

---

## Usage

### Disassembler

```bash
./build/disasm <elf-file>
```

Reads an ELF32 Blackfin binary and prints disassembly for all executable sections, matching `bfin-elf-objdump -d` output format.

### Emulator

```bash
./build/emu [options] <elf-file>
```

| Option | Description |
|---|---|
| `--trace` | Print PC + disassembly before each basic-block execution |
| `--dump` | Print generated LLVM IR per basic block (post-optimization) |
| `--max-steps N` | Stop after N basic-block steps (0 = unlimited, default) |
| `--opt-level N` / `-O N` | LLVM optimization level 0–3 (default: 2) |
| `--fastmem` | `mmap` memory at address 0 for zero-overhead JIT loads/stores |

Example — run with trace output:
```bash
./build/emu --trace build/test_linked/hello.S.elf
```

---

## Testing

Tests are assembled from the [op1emu/bfin_sim](https://github.com/op1emu/bfin_sim) testsuite, fetched automatically at CMake configure time.

```bash
# Emulator tests — runs all 827 test ELFs
python3 tests/run_emu_test.py --no-fail-fast

# Run a subset matching a glob
python3 tests/run_emu_test.py --filter "cec-*" -v

# Disassembler comparison against bfin-elf-objdump
python3 tests/run_comparison_test.py
```

Current pass rate: **790 / 827** emulator tests, **827 / 827** disassembler comparison tests.

---

## Reference

Instruction semantics and test fixtures are derived from [op1emu/bfin_sim](https://github.com/op1emu/bfin_sim):

- `sim/bfin-sim.c` — authoritative Blackfin simulator (~45 K lines)
- `sim/bfin-dis.c` — authoritative Blackfin disassembler (~33 K lines)
- `testsuite/` — assembly test cases

---

## License

MIT — see [LICENSE](LICENSE).
