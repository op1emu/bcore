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

// A piece of a cold block (cp_translator.h's CpTranslator::Cold): the slot
// of a function all blocks share (load_shared_functions), and where its
// hole values start in the block's.
struct ColdPiece {
    uint32_t slot;
    uint32_t values;
};

// Functions every loader's blocks use (the copy-and-patch tier's pieces,
// cp_cold_wasm.h): a module of n functions, exported as "0" to n - 1, each
// in a slot of the process's that nothing empties; slots[i] the i-th's.
// False, and none in a slot, if that fails (said why).
bool load_shared_functions(const uint8_t* bytes, size_t size, uint32_t n, uint32_t* slots);
// Where CodeLoader::run_pieces leaves a pointer to a piece's hole values,
// for the piece to read them: an address in the process's memory.
uintptr_t piece_values_address();

class CodeLoader {
public:
    CodeLoader();
    ~CodeLoader();

    bool init();
    BbFunc load(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, const std::string& name);
    // A module the copy-and-patch translator put together (cp_wasm.h): as
    // load, but for the object's checks, which a module made of the build's
    // stencils needs none of; its function `name` in a slot.
    BbFunc load_module(uint32_t pc, const uint8_t* bytes, size_t size, const char* name);
    // As load_module, for a module of n blocks (its functions exported as
    // "0" to n - 1), each in a slot of its own: out[i] the i-th's. False,
    // and none in a slot, if that fails.
    bool load_functions(uint32_t pc, const uint8_t* bytes, size_t size, uint32_t n, BbFunc* out);
    // Its slot emptied (the module goes when nothing else holds it: none of
    // its functions in a slot) and free for another block's.
    void release(BbFunc fn);
    void enter(BbFunc fn, CpuState* cpu, Memory* mem) const { fn(cpu, mem); }
    // A cold block: its pieces (functions (cpu, mem) -> i32), one after the
    // other, each with its hole values at values + piece.values, until one
    // returns nonzero: it left the block, to the host.
    void run_pieces(const ColdPiece* pieces, size_t n, const uint32_t* values, CpuState* cpu, Memory* mem) const;

    // The blocks loaded and still in a slot, and the bytes of every object
    // loaded so far.
    size_t modules() const { return slots_.size(); }
    uint64_t objectBytes() const { return object_bytes_; }

private:
    std::unordered_set<uint32_t> slots_;  // this loader's, to empty when it goes
    uint64_t object_bytes_ = 0;
};

}  // namespace bcore
