#!/bin/bash
# The WebAssembly build's tools, in $BCORE_WASM_TOOLS (~/.cache/bcore-wasm
# by default), none of them in the tree: emsdk 3.1.73 (Emscripten and its
# Node), and emscripten-forge's LLVM 19.1.7 for that Emscripten
# (emscripten-abi 3.1.73), its libraries built for wasm32, which bcore links.
# Fetches what is missing; prints where each is.
set -euo pipefail

EMSDK_VERSION=3.1.73
LLVM_PACKAGE=llvm-19.1.7-h91a5cda_0.tar.bz2
LLVM_SHA256=909fa5e5d69becd95f959a89573072fe3da1b56455353c21f2938ca7e7de371a
LLVM_CHANNEL=https://repo.prefix.dev/emscripten-forge-3x/emscripten-wasm32

root=${BCORE_WASM_TOOLS:-$HOME/.cache/bcore-wasm}
mkdir -p "$root"

# Each made in a directory of its own and moved into place when complete:
# an interrupted run leaves nothing that a later one takes for done.
if [ ! -f "$root/emsdk/.emscripten" ]; then
    rm -rf "$root/emsdk" "$root/emsdk.new"
    git clone -q --depth 1 --branch "$EMSDK_VERSION" https://github.com/emscripten-core/emsdk.git "$root/emsdk.new"
    "$root/emsdk.new/emsdk" install "$EMSDK_VERSION"
    "$root/emsdk.new/emsdk" activate "$EMSDK_VERSION" > /dev/null
    rm -rf "$root/emsdk.new/downloads"  # the archives, unpacked
    mv "$root/emsdk.new" "$root/emsdk"
fi

if [ ! -f "$root/llvm/lib/cmake/llvm/LLVMConfig.cmake" ]; then
    new="$root/llvm.new"
    rm -rf "$root/llvm" "$new" "$root/$LLVM_PACKAGE"
    curl -fsSL --retry 3 -o "$root/$LLVM_PACKAGE" "$LLVM_CHANNEL/$LLVM_PACKAGE"
    echo "$LLVM_SHA256  $root/$LLVM_PACKAGE" | sha256sum -c --quiet -
    mkdir -p "$new"
    tar -xjf "$root/$LLVM_PACKAGE" -C "$new"
    rm "$root/$LLVM_PACKAGE"
    # LLVM's libraries, headers and CMake package only, nor clang's and
    # lld's; of its tools (as WebAssembly) those its CMake package names,
    # which checks they are there.
    rm -rf "$new/lib/cmake/clang" "$new/lib/cmake/lld" "$new/include/clang"* "$new/include/lld" "$new/lib/clang"
    rm -f "$new/lib/libclang"* "$new/lib/liblld"*
    keep=$(grep -oh '_IMPORT_PREFIX}/bin/[^" ]*' "$new/lib/cmake/llvm/LLVMExports-"*.cmake | sed 's|.*/bin/||' | sort -u)
    [ -n "$keep" ] || { echo "setup.sh: LLVM's CMake package names no tools" >&2; exit 1; }
    for f in "$new/bin/"*; do
        grep -qxF "$(basename "$f")" <<< "$keep" || rm -rf "$f"
    done
    mv "$new" "$root/llvm"
fi

echo "EMSDK=$root/emsdk"
echo "BCORE_WASM_LLVM_DIR=$root/llvm/lib/cmake/llvm"
