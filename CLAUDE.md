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
- `src/cpu_state.h/cpp`: CpuState struct (dpregs, accumulators, ASTAT flags, loops), ASTAT compose/decompose, the helpers JIT'd code calls for them
- `src/memory.cpp`, `src/mem_helpers.h`: the read/write helpers JIT'd code calls (`mem_read8`, ...), through `Memory`
- `src/lift_visitor.h/cpp`: LiftVisitor — plugs into decoder, emits LLVM IR per instruction (~24 implemented, rest are stubs)
- `src/compiler.h/cpp`: `Compiler` — module to object file: the IR pipeline and the backend, with the TargetMachine, pass builder, analysis managers and legacy codegen pass manager built once (per tier: Core keeps one for tier 0), and `kLightPipeline` for promotions (Core::set_light_tier1)
- `src/block_abi.h`: the blocks' and stencils' calling convention (`swifttailcc`, CpuState `swiftself`, Memory `swiftasync`, and on x86-64 `no_callee_saved_registers`) and the musttail call by which one runs the next; on WebAssembly (by the module's triple, `bcore_abi::is_wasm`) C's convention, each block exported under its name, musttail as `return_call`
- `src/host_bridge.h/cpp`: the helpers that reach host code, wrapped to catch a host exception where none may unwind through generated code (stencils; macOS), and `RunScope`, which Core::run throws it again with
- `src/helper_abi.h`: the helpers' names, signatures and the convention generated code calls them in (C on x86-64; preserve_most through the native CodeLoader's trampolines on AArch64)
- `src/jit_engine.h/cpp`: `JitEngine` — which block each PC has (`addObject`, `addCode`, `lookup`) and the chain table with the tier table after it (`include/tier_table.h`, also `tier_threshold`: a tier-0 block's threshold scaled by its size, Core::set_tiering's size_ref), over a `CodeLoader` (`src/code_loader.h`: one per build, for its host) that makes object files functions
- `src/code_loader_native.h/cpp`: the native `CodeLoader` — LLJIT, which loads the Compiler's object files into the code arenas, with explicit `absoluteSymbols` registration for all helpers (by address, under their names in `kHelpers`, from `src/helper_functions.cpp`), `bcore_enter`, and the AArch64 trampolines
- `src/helper_functions.h/cpp`: the C function (or catching wrapper) each helper is, as a loader registers it
- `src/code_loader_wasm.h/cpp`: the WebAssembly `CodeLoader` (an Emscripten build's) — each block's object instantiated as a module of its own over the process's memory and function table (EM_JS), its function in a table slot (that is a `BbFunc` there), slots freed when a block is replaced; blocks chain by `return_call_indirect`, so the engine must have tail calls
- `src/wasm_object.h/cpp`: whether a block's WebAssembly object runs as LLVM wrote it, unlinked: only types, imports, functions, exports, code and custom sections, only index relocations, only the loader's imports (`tests/wasm_object_test.cpp` compiles blocks for wasm32 on any host whose LLVM has the target)
- `tools/wasm/`: the WebAssembly build (`README.md`): `setup.sh` fetches emsdk 4.0.9 and emscripten-forge's LLVM 23.1.2; `web/` runs test programs in a browser's Worker (`emu-web`)
- `src/compile_worker.h/cpp`: `CompileWorker` — a thread with a Compiler of its own that runs the IR pipeline and backend of promotions (Core::set_tier_background); the dispatching thread lifts, loads and publishes (none in a build without threads, `Core::kHasThreads`: `set_tier_background(true)` returns false)
- `src/tier_memory.h`: `RecordingMemory`/`ReplayMemory` (namespace `bcore_tier`): a tier-0 compilation logs the instructions it reads, its promotion reads only those
- `src/cp_translator.h/cpp`: `CpTranslator` — the copy-and-patch tier 0 (Core::set_copy_patch): a block copied together from stencils, holes filled in; helpers wrapped so host exceptions cross no stencil code, reached through 16-byte jumps placed in the arena (on AArch64 a set within every 120 MiB, B/BL's reach, to preserve_most trampolines); on Apple silicon it writes inside a JitWriteScope, and it invalidates the instruction cache before a block is published
- `src/cp_format.h`, `src/cp_patterns.def`, `src/cp_stencils.cpp`: the stencils' format, the 32-bit patterns whose immediates are field holes (every DSP pattern takes register holes, inferred by stencilgen), and the generated tables in the library (`cp_tables.bin`, `.incbin`'d)
- `src/cp_build.h/cpp`: what building a stencil takes: the holes, the packet's memory (`StencilMemory`), the compile (`compile_stencils`: on AArch64 each value hole's use becomes MOVZ/MOVK, `materialize_holes`), the object's code, cold code (the machine function splitter's, x86-64 only) and relocations (`extract()`: x86-64's and AArch64's) and the checks' PCs
- `src/cp_patch.h`: how the translator fills a relocation in (`patch_reloc`), for both architectures
- `src/bb_stencil.cpp`: BBTranslator's stencils (one packet in LiftVisitor's stencil mode; a parallel packet's pieces: slot 0's instruction alone, a slot with its writes to `CpuState::slot_scratch`, a slot's commit, the packet's tail; a block's prologue, its exit)
- `tools/stencilgen.cpp`: `bcore-stencilgen`, run by the build: lifts, checks and compiles every stencil into `cp_gen/` (report: `cp_gen/cp_stencils.txt`; `--dump WORD` prints a lift); it links `bcore-lift` (the lifter, its passes and Compiler, over `bcore-runtime`). With `-DBCORE_STENCIL_CACHE_DIR=DIR` the build keeps the stencils of the last generator binary there (`cmake/StencilCache.cmake`, keyed by its SHA-256) and copies them when it links the same binary again, as CI does with ccache
- `src/code_arena.h/cpp`: `CodeArena` (one reservation: hot code, cold parts split off by the machine function splitter, data), `CodeArenaPool` (the native CodeLoader's arenas: an object goes whole into one with room, else a new one, else its own `SectionMemoryManager`, so a full arena never stops the process) and the per-object `ArenaMemoryManager` that the native CodeLoader's `RTDyldObjectLinkingLayer` uses
- `src/bb_translator.h/cpp`: Basic-block translator — decodes instructions until terminator, produces one LLVM function per BB
- `src/syscall_emu.h/cpp`: Libgloss syscall emulation (exit, write)
- `src/guest_memory.h/cpp`: a loop block's guest memory said abstractly (`bcore.guest.ptr`, `bcore.guest.ptr.in` for an address known to lie in a circular buffer, `bcore.guest.aligned`), `GuestLoopVersioning` (SCEV ranges proven at loop entry, a copy with direct RAM access) and `lower_guest_memory`
- `src/guest_speculation.h/cpp`: `SatReductionSpeculation`, a speculative unclamped copy of W32 saturating MAC loops for LoopVectorize, checked by |a0| + m·max|x| ≤ INT32_MAX at its end
- `src/steady_skip.h/cpp`: `SteadyLoopSkip` (Core::set_steady_skip), a latch test that the iteration just run changed nothing but the loop's affine counters and made no read the host could not promise would repeat (`CpuState::unstable_reads`); the loop then goes to its last counted iteration (`evaluateAtIteration`)
- `tools/emu.cpp`: ELF loader, JIT execution loop, hardware loop support, CLI options (`--trace`, `--dump`, `--max-steps`, `--count-packets`, `--chain`, `--opt-level`)

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
./build/emu [--trace] [--dump] [--max-steps N] [--count-packets] [--chain] [--opt-level/-O N] <elf-file>
```

The emulator:
1. Loads an ELF32 Blackfin executable into flat memory
2. Translates basic blocks to LLVM IR on first encounter (lazy JIT)
3. Executes JIT'd native code, handling branches and hardware loops in the host loop
4. Emulates libgloss syscalls (exit, write) via EXCPT 0

**Options:**
- `--trace`: Print PC and disassembly of each instruction before each BB execution
- `--dump`: Print generated LLVM IR per basic block (after optimization)
- `--max-steps N`: Leave each block after at most N packets (0 = unlimited, default; 1 single-steps)
- `--count-packets`: Print the packets and block runs executed (`packets=N runs=M`) to stderr at exit
- `--chain`: Blocks run the next block themselves until a CEC event or a halt (CpuState::chain_budget); results match a run without it
- `--no-loop-blocks`: Blocks that run themselves again compile like any other (Core::set_loop_blocks); results match
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
- **Prefer LLVM intrinsics over helpers.**

## Testing

- `tests/run_comparison_test.py`: Python script to compare disassembly output against `bfin-elf-objdump`
  run with `python3 tests/run_comparison_test.py`
- Emulator tests: assembly files from https://github.com/op1emu/bfin_sim/tree/main/testsuite/ are fetched at configure time, assembled, linked to `.elf`, and run via `./build/emu`
- `tests/run_packet_count_test.py`: checks `CpuState::packets` (packets the last `Core::run` entered) against single-stepping for every test ELF
- `tests/run_diff_test.py`: every test ELF run by two emulators (`--base-emu`) or with two sets of flags (`--base-flags`, or a `--matrix`: `standard`, `tiers`, `copy-patch`) and compared: exit code, output, packets (`--count-packets`), or the IR (`--dump`: emu reports its host base and the constants near it are compared as offsets from it, every other constant as it is). Its own tests: `python3 -m unittest tests/test_run_diff.py`. The gate for a change that must not change what the guest does: build the commit before it and pass its emu as `--base-emu`

## Profiling and Observability

`include/bcore_profile.h` is the contract; read it first. `README.md`
("Observability (host API)") covers host-visible behavior. bcore is a library:
no env vars, no I/O, no printing on success. The host owns all sinks.

Code: `src/profile.cpp` + `src/profile_state.h` (code-map `JITEventListener`,
jitdump attach), translation-path instrumentation in `src/core.cpp` behind
`#if BCORE_ENABLE_PROFILE`, `Core` API in `include/core.h`, test in
`tests/profile_test.cpp`.

### Build and test

| Option | Default | Effect |
|---|---|---|
| `BCORE_ENABLE_PROFILE` | ON | code map, event sink, counters (links LLVM `object`); OFF compiles them out of `Core::run` |
| `BCORE_PERF_JIT_EVENTS` | OFF | links LLVM `perfjitevents` (requires `BCORE_ENABLE_PROFILE`); still needs a runtime `set_perf_jitdump(true)` |
| `BCORE_BUILD_PROFILE_TESTS` | OFF | builds the `bcore-profile-test` target |

```bash
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_DIR=/usr/lib/llvm-15/cmake -DBCORE_ENABLE_PROFILE=ON \
  -DBCORE_PERF_JIT_EVENTS=ON -DBCORE_BUILD_PROFILE_TESTS=ON
cmake --build build-profile --target bcore-profile-test
ctest --test-dir build-profile -R bcore-profile-test --output-on-failure
```

Verify profiling changes with `BCORE_ENABLE_PROFILE` both ON and OFF; the test
is written to pass in both.

### Capabilities

- **Code map**: `forEachCompiledBlock` yields `{host_addr, host_size, guest_pc, variant, loaded_ns, unloaded_ns}`; sizes are exact (`computeSymbolSizes` in the load listener), `0` = unavailable. `profileHasBlockSizes()` reports listener attach. `setProfileHistory(n)` retains bounded retired ranges so pre-`invalidate()` samples still resolve; `profileHistoryDropped()` reports truncation.
- **Event sink**: `setEventSink()` (non-owning; detach with `nullptr` first). Translation path only: `onTranslateBegin` / `onTranslateEnd` / `onTranslateFailure` and the `lift`, `ir-optimize` (when `opt_level != 0`) and `materialize` stages, reported after the translation finishes so sink time is in no measured interval. Never sum stages into their parent. Nothing fires on a cache hit.
- **Counters**: `stats()` POD snapshot, one owner-thread increment per event.

Call control/query APIs from the `Core::run()` thread or while it is stopped;
code-map callbacks run under the map mutex and must not re-enter these APIs.
Event-sink callbacks run inside `Core::run()` and must neither throw nor call
back into that `Core` (an `invalidate()` from a callback would free the block
about to run). A translation that throws still reports `onTranslateFailure`.

### `perf inject --jit`

Build with `BCORE_PERF_JIT_EVENTS=ON`; the host calls `set_perf_jitdump(true)`
any time after `init()` (attaches immediately, re-attaches across
`invalidate()`; `false` detaches from the current engine at once); `perf record -k mono`, then `perf inject --jit` resolves blocks
by `bb_0x<pc>` names. LLVM writes the dump under `$JITDUMPDIR/.debug/jit/`
(or `$HOME/.debug/jit/`). Shut down cleanly: the listener flushes from its
destructor, so a default-handled signal truncates the jitdump.

### Invariants (each was a real bug in the research line)

- The perf listener is a **function-local static, non-owning**: never `unique_ptr`/`delete` it (was: `free(): invalid pointer` on every profiling shutdown).
- Re-register listeners after `invalidate()` (it replaces the engine). Attach the perf listener via its own `attach_perf_listener` path: re-running `attach_listeners()` would replace `map_listener` while `RTDyldObjectLinkingLayer` still holds the old raw pointer. Repeated enable must stay idempotent per engine.
- Never size blocks by "nearest preceding block, capped at N bytes": samples past the cap get misattributed. Exact sizes and range containment, or `0`.
- Never reattribute dropped history to reused JIT addresses; report the drop.
- Enabled builds keep a null check and counter increments per dispatch even with no sink: say "near zero cost", not zero overhead.
- Sampling a JIT thread: no `backtrace()` in the handler (see EXPERIENCES.md).

## Experiences
- Store experiences in `EXPERIENCES.md` to track insights, challenges, and solutions encountered during development. (Optional but recommended for knowledge sharing)

## Important Notes

- Build with CMake: `cmake -B build && cmake --build build`. The core is three libraries and Core: `bcore-runtime` (no LLVM: CPU state, CEC, events, the helpers' C functions, syscalls, disassembler), `bcore-lift` (lifter, IR passes, Compiler), `bcore-engine` (JitEngine, the host's CodeLoader, code arenas, the promotion worker, profiling), and `bfin-core` (Core) over them
- WebAssembly: built with Emscripten and LLVM 23 (`tools/wasm/README.md`); executables run in Node 24 or newer (`build-wasm/emu-node` for the test scripts); no copy-and-patch, no profiling, no promotion worker; helpers catch host exceptions there (`BCORE_HELPERS_CATCH`)
- Requires LLVM 15, or 23 without copy-and-patch (set `LLVM_DIR`; without it CMake looks for 15). Where the two differ, `src/llvm_compat.h` (and `src/scev_expander.h`) has the one spelling both take; a change that would make LLVM 15's IR differ goes behind `LLVM_VERSION_MAJOR`, and `run_diff_test.py --dump` against the commit before decides whether it would. Builds on x86-64 Linux and on AArch64 (macOS on Apple silicon, Linux), copy-and-patch on both architectures (stencils for the host's, `bcore-stencilgen --target` makes the other's to look at)
- Blocks and stencils are made and called only through `src/block_abi.h` (`make_block`, `call_block`, `tail_call_block`); never call one from C++, only through `JitEngine::enter` (`CodeLoader::enter`). A helper is declared only through `bcore_abi::declare_helper` (`src/helper_abi.h`), and a new helper goes into its `kHelpers` table as well as `helper_functions.cpp`'s
- On Apple silicon the code arena is `MAP_JIT`: anything that writes code into it (linking, copying) does so inside a `JitWriteScope`, and nothing runs code inside one
- Requires `bfin-elf-gcc` and `bfin-elf-ld` at `$HOME/toolchains/bfin-elf/bin/` for test assembly/linking
- The helpers JIT'd code calls are registered explicitly via `absoluteSymbols` in `code_loader_native.cpp` — do NOT rely on `-rdynamic` / `DynamicLibrarySearchGenerator`; they are C++ functions in `namespace bcore`, with no C name a host could clash with
