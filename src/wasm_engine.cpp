#include "wasm_engine.h"

#include <cstring>

#include <emscripten.h>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include "helper_functions.h"

namespace {

// Blocks' frames: a spill or two each, one block at a time (a chained block
// replaces its caller's), and a block again only where a helper runs one,
// which none does. Nothing checks for overflow; an exception that left a
// block's frame would leave its stack pointer moved, which is why every
// helper that may throw is a wrapper that catches (host_bridge.h).
constexpr size_t kStackSize = size_t(1) << 16;

void wasm_enter(BbFunc fn, CpuState* cpu, Memory* mem) { fn(cpu, mem); }

}  // namespace

EM_JS_DEPS(bcore_wasm_engine, "$UTF8ToString");

// Module.bcoreWasm: for each engine (by its address) what its blocks'
// modules are instantiated with, the imports an object of Compiler::emit's
// has (its memory, table and the engine's stack pointer, the n helpers
// names[i] at the function pointers fns[i], memset, memcpy and memmove
// among them) with the kind of each; and the table's free slots, which the
// engines share. 1 where the engine has no tail calls (a module with a
// return_call does not validate).
EM_JS(int, bcore_wasm_init, (const void* engine, const char* const* names, void* const* fns, int n,
                             uint32_t stack_top), {
    const probe = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11]);
    if (!WebAssembly.validate(probe)) return 1;
    const env = Object.create(null);
    const kinds = Object.create(null);
    env["__linear_memory"] = wasmMemory;
    kinds["__linear_memory"] = "memory";
    env["__indirect_function_table"] = wasmTable;
    kinds["__indirect_function_table"] = "table";
    env["__stack_pointer"] = new WebAssembly.Global({value: "i32", mutable: true}, stack_top);
    kinds["__stack_pointer"] = "global";
    for (let i = 0; i < n; ++i) {
        const name = UTF8ToString(HEAPU32[(names >> 2) + i]);
        env[name] = wasmTable.get(HEAPU32[(fns >> 2) + i]);
        kinds[name] = "function";
    }
    Module["bcoreWasm"] = Module["bcoreWasm"] || {engines: new Map(), free: []};
    Module["bcoreWasm"].engines.set(engine, {env: env, kinds: kinds});
    return 0;
});

// The object of the block at pc, size bytes at bytes, instantiated for
// engine: the table slot of its function `name`, or -1, said why on stderr.
// The object has functions only: a section of any other kind (data, which
// would be placed nowhere; elements, which would be relocations left
// undone; a start function, memories, tables, globals) or an import that is
// not among init's, of the same kind, is refused.
EM_JS(int, bcore_wasm_load, (const void* engine, const uint8_t* bytes, uint32_t size, const char* name,
                             uint32_t pc), {
    const w = Module["bcoreWasm"];
    const e = w.engines.get(engine);
    const where = "bcore: the block at 0x" + (pc >>> 0).toString(16).padStart(8, "0") + ": ";
    const code = HEAPU8.slice(bytes, bytes + size);
    let at = 8;
    const uleb = () => {
        let value = 0, shift = 0, byte;
        do {
            byte = code[at++];
            value += (byte & 0x7f) * Math.pow(2, shift);
            shift += 7;
        } while (byte & 0x80);
        return value;
    };
    // custom, type, import, function, export, code
    const allowed = [0, 1, 2, 3, 7, 10];
    while (at < code.length) {
        const id = code[at++];
        const length = uleb();
        if (!allowed.includes(id)) {
            err(where + "its object has a section of kind " + id);
            return -1;
        }
        at += length;
    }
    try {
        const module = new WebAssembly.Module(code);
        for (const i of WebAssembly.Module.imports(module)) {
            if (i.module !== "env" || e.kinds[i.name] !== i.kind) {
                err(where + "its object imports " + i.kind + " " + i.module + "." + i.name);
                return -1;
            }
        }
        const fn = new WebAssembly.Instance(module, {env: e.env}).exports[UTF8ToString(name)];
        if (typeof fn !== "function") {
            err(where + "its object exports no " + UTF8ToString(name));
            return -1;
        }
        const slot = w.free.length ? w.free.pop() : wasmTable.grow(1);
        wasmTable.set(slot, fn);
        return slot;
    } catch (error) {
        err(where + error);
        return -1;
    }
});

// The engine's n slots at slots, emptied and free again, and its imports
// forgotten.
EM_JS(void, bcore_wasm_release, (const void* engine, const uint32_t* slots, uint32_t n), {
    const w = Module["bcoreWasm"];
    for (let i = 0; i < n; ++i) {
        const slot = HEAPU32[(slots >> 2) + i];
        wasmTable.set(slot, null);
        w.free.push(slot);
    }
    w.engines.delete(engine);
});

WasmEngine::WasmEngine() = default;

WasmEngine::~WasmEngine() {
    if (stack_) bcore_wasm_release(this, slots_.data(), slots_.size());
}

bool WasmEngine::init() {
    makeTables();
    enter_ = &wasm_enter;
    std::vector<const char*> names;
    std::vector<void*> fns;
    for (const auto& [name, fn] : bcore_abi::kHelperFunctions) {
        names.push_back(name);
        fns.push_back(bcore_abi::bound_helper(name, fn));
    }
    // What the backend calls for an intrinsic (helper_abi.h).
    names.push_back("memset");
    fns.push_back(reinterpret_cast<void*>(&::memset));
    names.push_back("memcpy");
    fns.push_back(reinterpret_cast<void*>(&::memcpy));
    names.push_back("memmove");
    fns.push_back(reinterpret_cast<void*>(&::memmove));
    stack_ = std::make_unique<uint8_t[]>(kStackSize);
    const uint32_t top = (reinterpret_cast<uintptr_t>(stack_.get()) + kStackSize) & ~uintptr_t(15);
    if (bcore_wasm_init(this, names.data(), fns.data(), int(names.size()), top) != 0) {
        llvm::errs() << "bcore: this WebAssembly engine has no tail calls\n";
        return false;
    }
    return true;
}

bool WasmEngine::addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, uint32_t fallthrough_pc,
                           const std::string& name) {
    const int slot = bcore_wasm_load(this, reinterpret_cast<const uint8_t*>(object->getBufferStart()),
                                     uint32_t(object->getBufferSize()), name.c_str(), pc);
    if (slot < 0) return false;
    slots_.push_back(uint32_t(slot));
    ++modules_;
    object_bytes_ += object->getBufferSize();
    addCode(pc, reinterpret_cast<BbFunc>(uintptr_t(slot)), fallthrough_pc);
    return true;
}
