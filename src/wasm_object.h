#pragma once

// Whether a block's object file for WebAssembly (Compiler::emit) is a
// module that runs as it is, instantiated over the process's memory and
// function table (code_loader_wasm.cpp), with no linker. LLVM's object is
// relocatable (tool-conventions' Linking.md): its code section holds, at
// each relocation, a value the linker would replace. A function's, a
// type's, a global's and a table's index are already the module's own (an
// object's index spaces are a module's), and an engine reads the code as
// it is. An address in memory or a function's place in the table is not:
// an object with one, or with a section other than its types, imports,
// functions, exports, code and custom sections (data, which nothing would
// place; elements, tables, memories, globals, a start function), is
// refused, and so is an import the loader does not give.

#include <string>

#include <llvm/Support/MemoryBuffer.h>

namespace bcore {

// What a block's module may import from "env", and as what.
enum class WasmImportKind { Function, Table, Memory, Global };
struct WasmImport {
    const char* name;
    WasmImportKind kind;
};

// Why object, a module whose function `name` the loader runs, cannot run as
// it is (imports: what the loader gives it), or empty if it can.
std::string check_wasm_object(llvm::MemoryBufferRef object, const WasmImport* imports, size_t n,
                              const std::string& name);

}  // namespace bcore
