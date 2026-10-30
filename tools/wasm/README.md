# bcore in WebAssembly

The core built with Emscripten runs blocks as WebAssembly: each block's
object, from LLVM's WebAssembly backend in the same process, becomes a
module of its own, instantiated over the process's memory and function
table (`src/code_loader_wasm.h`), after a check that it runs as LLVM wrote
it, linked by nobody (`src/wasm_object.h`). Blocks run the next one by
`return_call_indirect`: the engine needs tail calls (Chrome 112, Firefox
121, Safari 18.2, Node 20). There is no profiling, and no promotion worker
(no threads: `Core::kHasThreads`).

## Copy-and-patch

The copy-and-patch tier 0 (`--copy-patch`) has stencils of its own,
WebAssembly code (`src/cp_wasm.h`): the build's bcore-stencilgen, itself
WebAssembly, lifts them with this build's CpuState and compiles them with
its LLVM, at O2, in Node. It has no threads, so `tools/wasm/stencilgen.mjs`
runs it in a process per CPU (`--shard`) and merges what they built
(`--merge`), which is what one run makes. About 2 minutes on 16 CPUs;
`-DBCORE_STENCIL_CACHE_DIR` keeps them for a generator that has not changed,
as on native builds.

A stencil is a piece of a block's one function, put together by
`src/cp_emit_wasm.cpp`: its code in a block of its own, which it leaves
for the next piece where its LLVM code tail-called the next packet, and
leaves with the pieces' block for the exit's code, which follows them. Its
locals are the function's, the same for every stencil, and every index it
names is the header's that all blocks share: what the translator fills in
are its value holes, 5-byte LEB128 immediates in the stencil (the
relocations LLVM's object leaves there), as long as their values in the
block (its modules 9% smaller over a boot of the OP-1's firmware). A block
of 8 packets made of a function per packet took three times as long to
make and run once in V8 (118 + 69 µs, against 40 + 20 µs): V8 compiles each
function when it is first called. A block that is never promoted (a
threshold of UINT32_MAX) counts no starts (`kPrologueUncounted`).

## The cold tier and batches

A module costs V8 about 40 µs to make and 5 to 15 to instantiate whatever
its size, its function 20 µs or more to compile when first called, and 27
KiB of memory; most of a boot's blocks start a few times (the OP-1's: 74%
of them at most 100 times, 0.14% of its packets). With
`Core::set_cold_threshold(N)` (emu `--cp-cold N`) a block's first N starts
make no module: its pieces run one at a time (`CodeLoader::run_pieces`),
careful and unchained, as functions of their stencils that every block
shares (`src/cp_cold_wasm.h`), made the first time a block needs them, a
module of those it needs that none did before (5,100 over that boot, a
third of a second all told). Its holes' values are read from memory. Then
it is compiled as copy-and-patch would have at its first start, with the
state of its next start as its hint, and its count goes on towards
`set_tiering`'s threshold. With `Core::set_cold_batch(K, age)` (emu
`--cp-batch K`) the blocks that leave the cold tier are compiled K to a
module, or fewer once the first has waited `age` packets (Chrome 133: 4.6 +
1.5 µs a block at 32, against 38.7 + 11.9 alone). A batch's block that runs
itself again does so by a branch to a loop around its pieces
(`kSelfExitsLoop`), as its function's index is not the one its module alone
would have. The same work either way (`run_diff_test.py --matrix
copy-patch`: tier0-cold and the rows after it; a cold block chains to none,
so more runs return to the host).

LLVM is the WebAssembly build's largest part, and with every instruction
on stencils (a block with IDLE too, `kIdleWait`, where the host waits) a
host may make it only when a block needs it: `Core::set_llvm_on_demand`.

The stencils are data in memory, 43 MiB of it: the executables start with
128 MiB (`-sINITIAL_MEMORY`). A block that runs a hardware loop in a few
packets runs slower than LLVM's tier 0 there (`bench.s` 4.1 s against
3.2 s; natively it runs faster): every packet compares the loop bottoms
and steps the loop through `bfin_hwloop_step`, which V8's optimizing tier,
which recompiles both, cannot take out of a stencil.

Every block's module imports the one function table, and the engine must
not copy it into each: V8 before 13.3 did, so that memory grew as the
square of the blocks (Node 20 ran out of it in the test suite). V8 13.3
on keeps one dispatch table a Table: Node 24, Chrome 133 or newer.
Firefox 157 ran the test programs (`web/`); Safari is untried.

## Tools

```bash
tools/wasm/setup.sh        # emsdk 4.0.9 and emscripten-forge's LLVM 23.1.2, in ~/.cache/bcore-wasm
source ~/.cache/bcore-wasm/emsdk-4.0.9/emsdk_env.sh
```

LLVM 23.1.2 comes built for wasm32 from emscripten-forge (channel
`emscripten-forge-4x`, for Emscripten 4): Release, without assertions,
RTTI or threads. Native builds use LLVM 15, or 23 (`src/llvm_compat.h`).

## Build and test

```bash
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=$HOME/.cache/bcore-wasm/llvm-23.1.2/lib/cmake/llvm \
  -DCMAKE_CROSSCOMPILING_EMULATOR=$(command -v node) -DBCORE_BUILD_FAULT_TESTS=ON
cmake --build build-wasm
ctest --test-dir build-wasm
```

The executables (`emu.js`, the tests) run in Node, which must be 24 or
newer for the table (above; CMake checks). `build-wasm/emu-node` runs
`emu.js` as an executable for the test scripts, against a native build's
emu and test programs:

```bash
python3 tests/run_diff_test.py --emu build-wasm/emu-node --base-emu build/emu \
  --matrix standard --count-packets -t 120 --ignore-list .github/emu-ignore.txt
```

The link is at -O1 whatever the build type: wasm-opt (-O2 and up) took
75 s for each executable, and its larger functions started three times
slower in V8; they run as fast either way.
