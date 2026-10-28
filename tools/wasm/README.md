# bcore in WebAssembly

bcore built with Emscripten compiles its blocks with LLVM in the same
WebAssembly program and runs them on the engine (`src/wasm_engine.h`), for a
browser (in a Worker) or Node. The executables of this build (emu, the tests)
are Node programs, which read the machine's files (`-sNODERAWFS`); a page
links bcore's libraries into a program of its own. This directory has what the
build needs beside the tree.

## Tools

`tools/wasm/setup.sh` fetches them into `$BCORE_WASM_TOOLS` (`~/.cache/bcore-wasm`
by default), nothing into the tree:

- emsdk 3.1.73: Emscripten and its Node;
- emscripten-forge's LLVM 19.1.7 for that Emscripten (channel
  `emscripten-forge-3x`, `emscripten-abi` 3.1.73), its libraries built for
  wasm32 (MinSizeRel, with assertions: compile times measured with it say so).

## Build

```bash
tools/wasm/setup.sh
source ~/.cache/bcore-wasm/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=$HOME/.cache/bcore-wasm/llvm/lib/cmake/llvm
cmake --build build-wasm --target emu
node build-wasm/emu.js --page-table --chain -O 1 program.elf
```

In this build there is no copy-and-patch tier (stencils are host code), no
profiling and no promotion worker (no threads: promotions compile in the run).
The engine needs WebAssembly's tail calls (Chrome 112, Firefox 121, Safari 18.2,
Node 20), and blocks use SIMD and bulk memory; exceptions are WebAssembly's. Each block is a module of its own, and
importing the function table must not copy it: emsdk's Node 20 (V8 11.3) gives
every instance a table of its own as large as the shared one, so that memory
grows with the square of the blocks (tier_test ran out of its 4 GB); Node 24
shares it (tier_test peaks at 0.5 GB).

## Tests

- `ctest --test-dir build-wasm` runs the C++ tests that do not need the native
  engine under Node (with `-DBCORE_BUILD_FAULT_TESTS=ON -DBCORE_BUILD_PROFILE_TESTS=ON`,
  and `-DCMAKE_CROSSCOMPILING_EMULATOR=$(command -v node)` for a Node newer
  than emsdk's).
- `tests/run_diff_test.py --emu build-wasm/emu.js --base-emu build/emu
  --matrix standard --count-packets` compares it with a native emu: every test's
  exit code, output and packets the same. The `--max-steps 3` configuration
  compiles a block every few packets and wants a longer timeout (`-t`) there.
  An `emu.js` runs under Node (`$NODE`, `node` by default).
