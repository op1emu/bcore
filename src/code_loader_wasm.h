#pragma once

// The WebAssembly CodeLoader (code_loader.h): each block's object becomes a
// module of its own, instantiated over the process's memory and function
// table (WebAssembly.Module, WebAssembly.Instance), its function placed in
// a slot of that table, which is what a BbFunc is there; C++ calls it as
// any function pointer. The object is checked first (wasm_object.h): it
// runs as LLVM wrote it, linked by nobody.
//
// What a block's module imports, from "env": the memory
// (__linear_memory), the table (__indirect_function_table) and a stack
// pointer (__stack_pointer) of the blocks' own, for a frame that needs
// memory, over a stack of their own; and the helpers (helper_functions.h),
// memset, memcpy and memmove, functions of the main module. A block runs
// another by return_call_indirect (block_abi.h): the engine must have tail
// calls (init checks). No exception leaves a helper there
// (BCORE_HELPERS_CATCH, helper_abi.h), so none unwinds through a block's
// frame, whose stack pointer would stay moved.
//
// Synchronous: the main thread of a browser takes 8 MiB of code at most
// that way (Chrome), a Worker any. A block's module is far smaller.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <llvm/Support/MemoryBuffer.h>

namespace bcore {

class CodeLoader {
public:
    CodeLoader();
    ~CodeLoader();

    bool init();
    BbFunc load(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, const std::string& name);
    // Its slot emptied (the module goes when nothing else holds it) and
    // free for another block's.
    void release(BbFunc fn);
    void enter(BbFunc fn, CpuState* cpu, Memory* mem) const { fn(cpu, mem); }

    // The modules loaded and still in a slot, and the bytes of every object
    // loaded so far.
    size_t modules() const { return slots_.size(); }
    uint64_t objectBytes() const { return object_bytes_; }

private:
    std::unordered_set<uint32_t> slots_;  // this loader's, to empty when it goes
    uint64_t object_bytes_ = 0;
};

}  // namespace bcore
