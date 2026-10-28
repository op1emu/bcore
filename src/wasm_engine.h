#pragma once

// The WebAssembly engine, in a WebAssembly build (Emscripten): each block's
// object file (Compiler::emit for wasm32) is a module of its own, which the
// engine checks, instantiates with bcore's memory, table and helpers, and
// whose block function it puts in bcore's function table. A function
// pointer there is an index into that table: BbFunc and ChainSlot::fn are
// such indices, which generated code calls with call_indirect (a chained
// block with return_call_indirect) and C++ as it calls any function
// pointer. The engine needs WebAssembly's tail calls (init fails without).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "exec_backend.h"

class WasmEngine : public ExecBackend {
public:
    WasmEngine();
    ~WasmEngine() override;

    bool init() override;
    bool addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, uint32_t fallthrough_pc,
                   const std::string& name) override;

    // The modules instantiated, and the bytes of their objects.
    uint64_t modules() const { return modules_; }
    uint64_t object_bytes() const { return object_bytes_; }

private:
    // The stack blocks' frames are on (their __stack_pointer), apart from
    // bcore's: a block runs one frame at a time, its next block in place.
    std::unique_ptr<uint8_t[]> stack_;
    // This engine's functions in the table, given back when it goes.
    std::vector<uint32_t> slots_;
    uint64_t modules_ = 0, object_bytes_ = 0;
};
