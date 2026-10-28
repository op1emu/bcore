# bcore in WebAssembly

The core built with Emscripten runs blocks as WebAssembly: each block's
object, from LLVM's WebAssembly backend in the same process, becomes a
module of its own, instantiated over the process's memory and function
table (`src/code_loader_wasm.h`), after a check that it runs as LLVM wrote
it, linked by nobody (`src/wasm_object.h`). Blocks run the next one by
`return_call_indirect`: the engine needs tail calls (Chrome 112, Firefox
121, Safari 18.2, Node 20). There is no copy-and-patch tier and no
profiling, and no promotion worker (no threads: `Core::kHasThreads`).

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
