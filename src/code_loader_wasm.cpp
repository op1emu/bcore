#include "code_loader.h"
#include "helper_functions.h"
#include "wasm_object.h"

#include <cstring>
#include <unordered_set>

#include <emscripten.h>

#if defined(__EMSCRIPTEN_PTHREADS__)
#error "the WebAssembly loader's slots, imports and stack are one thread's"
#endif

#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

namespace bcore {

namespace {

// The blocks' stack, for a frame that needs memory (a spill, an alloca a
// helper is given): one block's frame at a time, as a block that runs the
// next one has left its own (return_call), and no helper runs a block. Of
// the process: every loader's blocks run on it, one Core::run at a time.
// Nothing checks for overflow.
constexpr size_t kStackSize = size_t(1) << 16;
alignas(16) uint8_t g_stack[kStackSize];

// The table grows by this many slots at a time.
constexpr uint32_t kSlotChunk = 1024;

// What a block's module may import (wasm_object.h), and whether the imports
// are made (Module.bcoreBlocks, once in the process: the same for every
// loader). Table slots free for any loader's blocks.
std::vector<WasmImport> g_imports;
bool g_ready = false;
std::vector<uint32_t> g_free;

}  // namespace

}  // namespace bcore

EM_JS_DEPS(bcore_code_loader, "$UTF8ToString,$wasmTable");

// Whether the engine has tail calls: (func (return_call 0)) validates.
EM_JS(int, bcore_wasm_has_tail_calls, (), {
    const probe = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11]);
    return WebAssembly.validate(probe) ? 1 : 0;
});

// Module.bcoreBlocks: the imports of every block's module (env), by name,
// with no other name a module could find there (no prototype): the memory,
// the table, a stack pointer at stack_top, and the n functions names[i] at
// the function pointers fns[i]. 0, or -1 (said why). (A pointer is
// unsigned: >>> 0, as memory may grow past 2 GiB.)
EM_JS(int, bcore_wasm_make_imports, (const char* const* names, void* const* fns, int n, uint32_t stack_top), {
    try {
        const env = Object.create(null);
        env["__linear_memory"] = wasmMemory;
        env["__indirect_function_table"] = wasmTable;
        env["__stack_pointer"] = new WebAssembly.Global({value: "i32", mutable: true}, stack_top);
        for (let i = 0; i < n; ++i) {
            const name = UTF8ToString(HEAPU32[(names >>> 2) + i]);
            env[name] = wasmTable.get(HEAPU32[(fns >>> 2) + i]);
        }
        Module["bcoreBlocks"] = env;
        return 0;
    } catch (error) {
        err("bcore: the blocks' imports: " + error);
        return -1;
    }
});

// The first of n new slots at the table's end, or -1 (said why).
EM_JS(int, bcore_wasm_grow, (uint32_t n), {
    try {
        return wasmTable.grow(n);
    } catch (error) {
        err("bcore: the table grows no more: " + error);
        return -1;
    }
});

// The module of the size bytes at bytes (copied first: a view of shared
// memory is no module's source in every engine) instantiated, and its
// function `name` in the table's slot. 0, or -1 (said why, for the block at
// pc): no exception leaves.
EM_JS(int, bcore_wasm_instantiate, (const uint8_t* bytes, uint32_t size, const char* name, uint32_t slot,
                                    uint32_t pc), {
    try {
        const module = new WebAssembly.Module(HEAPU8.slice(bytes >>> 0, (bytes >>> 0) + (size >>> 0)));
        const fn = new WebAssembly.Instance(module, {env: Module["bcoreBlocks"]}).exports[UTF8ToString(name)];
        wasmTable.set(slot, fn);
        return 0;
    } catch (error) {
        err("bcore: the block at 0x" + (pc >>> 0).toString(16).padStart(8, "0") + ": " + error);
        return -1;
    }
});

EM_JS(void, bcore_wasm_clear, (uint32_t slot), {
    wasmTable.set(slot, null);
});

namespace bcore {

CodeLoader::CodeLoader() = default;

CodeLoader::~CodeLoader() {
    for (uint32_t slot : slots_) {
        bcore_wasm_clear(slot);
        g_free.push_back(slot);
    }
}

bool CodeLoader::init() {
    if (g_ready) return true;
    if (!bcore_wasm_has_tail_calls()) {
        llvm::errs() << "bcore: this WebAssembly engine has no tail calls\n";
        return false;
    }
    std::vector<const char*> names;
    std::vector<void*> fns;
    for (const auto& [name, fn] : bcore_abi::helper_functions()) {
        names.push_back(name);
        fns.push_back(fn);
    }
    // What the backend calls for an intrinsic (helper_abi.h).
    names.push_back("memset");
    fns.push_back(reinterpret_cast<void*>(&::memset));
    names.push_back("memcpy");
    fns.push_back(reinterpret_cast<void*>(&::memcpy));
    names.push_back("memmove");
    fns.push_back(reinterpret_cast<void*>(&::memmove));
    const uint32_t stack_top = uint32_t(reinterpret_cast<uintptr_t>(g_stack + kStackSize));
    if (bcore_wasm_make_imports(names.data(), fns.data(), int(names.size()), stack_top) != 0) return false;
    g_imports = {{"__linear_memory", WasmImportKind::Memory},
                 {"__indirect_function_table", WasmImportKind::Table},
                 {"__stack_pointer", WasmImportKind::Global}};
    for (const char* name : names) g_imports.push_back({name, WasmImportKind::Function});
    g_ready = true;
    return true;
}

BbFunc CodeLoader::load(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, const std::string& name) {
    const std::string why = check_wasm_object(object->getMemBufferRef(), g_imports.data(), g_imports.size(), name);
    if (!why.empty()) {
        llvm::errs() << "bcore: the block at 0x" << llvm::format_hex_no_prefix(pc, 8) << ": its object has " << why
                     << "\n";
        return nullptr;
    }
    if (g_free.empty()) {
        const int first = bcore_wasm_grow(kSlotChunk);
        if (first < 0) return nullptr;
        for (uint32_t i = kSlotChunk; i-- > 0;) g_free.push_back(uint32_t(first) + i);
    }
    const uint32_t slot = g_free.back();
    if (bcore_wasm_instantiate(reinterpret_cast<const uint8_t*>(object->getBufferStart()),
                               uint32_t(object->getBufferSize()), name.c_str(), slot, pc) != 0)
        return nullptr;
    g_free.pop_back();
    slots_.insert(slot);
    object_bytes_ += object->getBufferSize();
    return reinterpret_cast<BbFunc>(uintptr_t(slot));
}

void CodeLoader::release(BbFunc fn) {
    const uint32_t slot = uint32_t(reinterpret_cast<uintptr_t>(fn));
    if (!slots_.erase(slot)) return;
    bcore_wasm_clear(slot);
    g_free.push_back(slot);
}

}  // namespace bcore
