#pragma once

// The WebAssembly copy-and-patch tier's modules (cp_format.h), as
// bcore-stencilgen makes the stencils for them (cp_build.cpp's wasm_piece)
// and the translator puts them together (cp_emit_wasm.cpp).
//
// A block is a module of its own: a header that is the same for every
// block (its types: the blocks' (cpu, mem) and the helpers'; its imports, as
// the WebAssembly CodeLoader has them in Module.bcoreBlocks: the memory, the
// function table and every helper of cp_format.h's kHelpers, in that
// order), then its one function. That function is its pieces, inside a
// block, and after it the block's exit (one of kExits, or its own of
// kSelfExits).
//
// A stencil is a piece: its code inside a block of its own, which it leaves
// (br) for the next piece where its LLVM code tail-called the next packet,
// and leaves with the pieces' block, for the exit, where it tail-called
// that. Its locals are the function's, laid out the same for every stencil
// (kWasmLocals), and every index it names is the header's: what is left for
// the translator to fill in is its value holes. An exit's code may return
// (to Core::run).

#include <cstdint>
#include <cstring>
#include <vector>

#include "cp_format.h"
#include "helper_abi.h"

namespace bcore {

namespace bcore_cp {

// Function indices: the helpers (kHoleHelper + i is function i), then the
// block.
inline constexpr uint32_t kWasmBlockFunction = kHelperCount;

// The block function's locals after its parameters (cpu, mem): this many of
// i32, i64, f32 and f64, in that order.
inline constexpr uint8_t kWasmValueTypes[4] = {0x7f, 0x7e, 0x7d, 0x7c};
inline constexpr uint32_t kWasmLocals[4] = {20, 6, 0, 0};

// A function type as an id (cp_build.h's wasm_signature): its parameter
// count, its result count (0 or 1), then 3 bits per type (i32, i64, f32,
// f64, v128), parameters first.
inline int32_t wasm_signature_id(const uint8_t* params, unsigned nparams, const uint8_t* results,
                                 unsigned nresults) {
    if (nparams > 8 || nresults > 1 || nparams + nresults > 8) return -1;
    int32_t id = int32_t(nparams) | int32_t(nresults) << 4;
    unsigned shift = 5;
    for (unsigned i = 0; i < nparams + nresults; ++i) {
        const uint8_t t = i < nparams ? params[i] : results[i - nparams];
        int32_t code = -1;
        for (int k = 0; k < 4; ++k)
            if (kWasmValueTypes[k] == t) code = k;
        if (t == 0x7b) code = 4;  // v128
        if (code < 0) return -1;
        id |= code << shift;
        shift += 3;
    }
    return id;
}

struct WasmType {
    std::vector<uint8_t> params, results;
    int32_t id() const { return wasm_signature_id(params.data(), unsigned(params.size()), results.data(),
                                                  unsigned(results.size())); }
};

// Helper i's type: helper_abi.h's signature, every value an i32 (wasm32's
// pointers, and the narrow ones zero-extended); memset, memcpy and memmove
// C's.
inline WasmType wasm_helper_type(unsigned i) {
    const char* name = kHelpers[i];
    const char* sig = nullptr;
    if (const bcore_abi::Helper* h = bcore_abi::find_helper(name)) sig = h->sig;
    else if (!strcmp(name, "memset")) sig = "ppii";
    else sig = "pppi";  // memcpy, memmove
    WasmType t;
    if (sig[0] != 'v') t.results.push_back(0x7f);
    for (const char* c = sig + 1; *c; ++c) t.params.push_back(0x7f);
    return t;
}

// The header's types: the blocks' first, then each helper type once, in the
// order of the helpers.
inline std::vector<WasmType> wasm_types() {
    std::vector<WasmType> types(1);
    types[0].params = {0x7f, 0x7f};
    for (unsigned i = 0; i < kHelperCount; ++i) {
        const WasmType t = wasm_helper_type(i);
        bool known = false;
        for (const WasmType& u : types) known |= u.id() == t.id();
        if (!known) types.push_back(t);
    }
    return types;
}

// The type index of a function type's id, or -1.
inline int32_t wasm_type_index(int32_t id) {
    const std::vector<WasmType> types = wasm_types();
    for (size_t i = 0; i < types.size(); ++i)
        if (types[i].id() == id) return int32_t(i);
    return -1;
}

inline void wasm_leb(std::vector<uint8_t>& out, uint32_t v) {
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

inline void wasm_sleb(std::vector<uint8_t>& out, int32_t v) {
    for (;;) {
        const uint8_t b = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            out.push_back(b);
            return;
        }
        out.push_back(b | 0x80);
    }
}

inline void wasm_name(std::vector<uint8_t>& out, const char* name) {
    const size_t n = strlen(name);
    wasm_leb(out, uint32_t(n));
    out.insert(out.end(), name, name + n);
}

inline void wasm_section(std::vector<uint8_t>& out, uint8_t id, const std::vector<uint8_t>& body) {
    out.push_back(id);
    wasm_leb(out, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

// The header: the magic and version, the types and the imports.
inline std::vector<uint8_t> wasm_header() {
    std::vector<uint8_t> out = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    const std::vector<WasmType> types = wasm_types();
    std::vector<uint8_t> body;
    wasm_leb(body, uint32_t(types.size()));
    for (const WasmType& t : types) {
        body.push_back(0x60);
        wasm_leb(body, uint32_t(t.params.size()));
        body.insert(body.end(), t.params.begin(), t.params.end());
        wasm_leb(body, uint32_t(t.results.size()));
        body.insert(body.end(), t.results.begin(), t.results.end());
    }
    wasm_section(out, 1, body);
    body.clear();
    wasm_leb(body, 2 + kHelperCount);
    wasm_name(body, "env");
    wasm_name(body, "__linear_memory");
    body.insert(body.end(), {0x02, 0x00, 0x00});  // memory, no maximum, at least 0 pages
    wasm_name(body, "env");
    wasm_name(body, "__indirect_function_table");
    body.insert(body.end(), {0x01, 0x70, 0x00, 0x00});  // table of funcref, at least 0
    for (unsigned i = 0; i < kHelperCount; ++i) {
        wasm_name(body, "env");
        wasm_name(body, kHelpers[i]);
        body.push_back(0x00);
        wasm_leb(body, uint32_t(wasm_type_index(wasm_helper_type(i).id())));
    }
    wasm_section(out, 2, body);
    return out;
}

// The block function's locals' declaration (kWasmLocals).
inline std::vector<uint8_t> wasm_locals() {
    std::vector<uint8_t> out;
    wasm_leb(out, 4);
    for (int k = 0; k < 4; ++k) {
        wasm_leb(out, kWasmLocals[k]);
        out.push_back(kWasmValueTypes[k]);
    }
    return out;
}

}  // namespace bcore_cp

}  // namespace bcore
