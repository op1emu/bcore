#!/bin/bash
# The WebAssembly build's tools, in $BCORE_WASM_TOOLS (~/.cache/bcore-wasm
# by default), none of them in the tree: emsdk 4.0.9 (Emscripten), and
# emscripten-forge's LLVM 23.1.2 for it (channel emscripten-forge-4x,
# emscripten-abi 4), its libraries built for wasm32 (Release, without
# assertions, RTTI or threads), which bcore links. Fetches what is missing;
# prints where each is.
set -euo pipefail

EMSDK_VERSION=4.0.9
LLVM_VERSION=23.1.2
LLVM_PACKAGE=llvm-$LLVM_VERSION-hd68d874_0.tar.bz2
LLVM_SHA256=d66ecaed21878e5caecbcca61e65cddf813a421270b961958056a3c3dc23d2f2
LLVM_CHANNEL=https://repo.prefix.dev/emscripten-forge-4x/emscripten-wasm32

root=${BCORE_WASM_TOOLS:-$HOME/.cache/bcore-wasm}
mkdir -p "$root"
emsdk=$root/emsdk-$EMSDK_VERSION
llvm=$root/llvm-$LLVM_VERSION

# Each made in a directory of its own and moved into place when complete:
# an interrupted run leaves nothing that a later one takes for done.
if [ ! -f "$emsdk/.emscripten" ]; then
    rm -rf "$emsdk" "$emsdk.new"
    git clone -q --depth 1 --branch "$EMSDK_VERSION" https://github.com/emscripten-core/emsdk.git "$emsdk.new"
    "$emsdk.new/emsdk" install "$EMSDK_VERSION"
    "$emsdk.new/emsdk" activate "$EMSDK_VERSION" > /dev/null
    rm -rf "$emsdk.new/downloads"  # the archives, unpacked
    mv "$emsdk.new" "$emsdk"
fi

if [ ! -f "$llvm/lib/cmake/llvm/LLVMConfig.cmake" ]; then
    rm -rf "$llvm" "$llvm.new" "$root/$LLVM_PACKAGE"
    curl -fsSL --retry 3 -o "$root/$LLVM_PACKAGE" "$LLVM_CHANNEL/$LLVM_PACKAGE"
    echo "$LLVM_SHA256  $root/$LLVM_PACKAGE" | sha256sum -c --quiet -
    mkdir -p "$llvm.new"
    tar -xjf "$root/$LLVM_PACKAGE" -C "$llvm.new"
    rm "$root/$LLVM_PACKAGE"
    # LLVM's libraries, headers and CMake package only, not clang's or
    # lld's (LLVM's CMake package checks that its own libraries are there).
    rm -rf "$llvm.new/lib/clang" "$llvm.new/lib/cmake/clang" "$llvm.new/lib/cmake/lld" \
           "$llvm.new/include/clang" "$llvm.new/include/clang-c" "$llvm.new/include/lld"
    rm -f "$llvm.new/lib/libclang"*.a "$llvm.new/lib/liblld"*.a
    mv "$llvm.new" "$llvm"
fi

echo "EMSDK=$emsdk"
echo "BCORE_WASM_LLVM_DIR=$llvm/lib/cmake/llvm"
