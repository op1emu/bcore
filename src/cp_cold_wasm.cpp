// WebAssembly's cold blocks' piece functions (cp_cold_wasm.h).

#include "cp_cold_wasm.h"

#include <string>
#include <unordered_map>
#include <utility>

#include "code_loader.h"
#include "cp_format.h"
#include "cp_wasm.h"

namespace bcore::bcore_cp {

namespace {

// The function's locals after the block function's (kWasmLocals: 2 + 20 +
// 6 of them, from cpu and mem): the values' pointer, an i32 and an i64.
constexpr uint8_t kValues = 28, kHeld32 = 29, kHeld64 = 30;

bool is_store32(uint8_t op) { return op == 0x36 || op == 0x3a || op == 0x3b; }
bool is_store64(uint8_t op) { return op == 0x37 || op == 0x3c || op == 0x3d || op == 0x3e; }
bool is_load(uint8_t op) { return op >= 0x28 && op <= 0x35; }

// The piece type: (cpu, mem) -> i32, after the header's (wasm_types).
WasmType piece_type() {
    WasmType t;
    t.params = {0x7f, 0x7f};
    t.results = {0x7f};
    return t;
}

// The k-th value: local.get kValues, i32.load offset=4k.
void value(std::vector<uint8_t>& out, uint32_t k) {
    out.insert(out.end(), {0x20, kValues, 0x28, 0x02});
    wasm_leb(out, 4 * k);
}

std::unordered_map<uint32_t, uint32_t> g_slots;  // stencil -> its piece function's slot

}  // namespace

bool piece_function(uint32_t id, std::vector<uint8_t>& body) {
    const Stencil& s = kStencils[id];
    const uint8_t* code = kCode + s.code;
    // No return and no call of the block: none but an exit's or a guard's
    // code has one (wasm_rewrite takes a return in no other).
    for (size_t p = 0; p < s.size;) {
        const uint8_t op = code[p++];
        if (op == 0x0f || op == 0x12 || op == 0x13) return false;
        if (!wasm_skip_immediates(code, s.size, p, op)) return false;
    }
    body.clear();
    wasm_leb(body, 4);
    for (const auto& [n, type] : {std::pair<uint32_t, uint8_t>{kWasmLocals[0], 0x7f}, {kWasmLocals[1], 0x7e},
                                  {2, 0x7f}, {1, 0x7e}}) {
        wasm_leb(body, n);
        body.push_back(type);
    }
    static_assert(kWasmLocals[2] == 0 && kWasmLocals[3] == 0, "the piece functions' locals follow i32 and i64 only");
    body.push_back(0x41);  // i32.const, i32.load, local.set kValues
    wasm_sleb(body, int32_t(uint32_t(piece_values_address())));
    body.insert(body.end(), {0x28, 0x02, 0x00, 0x21, kValues, 0x02, 0x40});
    size_t from = 0;
    for (uint32_t k = 0; k < s.reloc_count; ++k) {
        const Reloc& r = kRelocs[s.relocs + k];
        // An i32.const's value, or an offset after its opcode and alignment.
        const size_t before = r.kind == kWasmSleb ? 1 : 2;
        if (r.offset < from + before || size_t(r.offset) + 5 > s.size || !is_value_hole(r.hole)) return false;
        if (r.kind == kWasmSleb) {
            if (code[r.offset - 1] != 0x41) return false;
            body.insert(body.end(), code + from, code + r.offset - 1);
            value(body, k);
        } else if (r.kind == kWasmLeb) {
            const uint8_t op = code[r.offset - 2], align = code[r.offset - 1];
            if (align & 0x80) return false;
            body.insert(body.end(), code + from, code + r.offset - 2);
            const uint8_t held = is_store32(op) ? kHeld32 : is_store64(op) ? kHeld64 : 0;
            if (!held && !is_load(op)) return false;
            if (held) body.insert(body.end(), {0x21, held});  // the value aside
            value(body, k);
            body.push_back(0x6a);  // i32.add: the offset in the address
            if (held) body.insert(body.end(), {0x20, held});
            body.insert(body.end(), {op, align, 0x00});
        } else {
            return false;
        }
        from = r.offset + 5u;
    }
    body.insert(body.end(), code + from, code + s.size);
    // On to the next piece: 0; left for the exit: 1.
    body.insert(body.end(), {0x41, 0x00, 0x0f, 0x0b, 0x41, 0x01, 0x0b});
    return true;
}

bool piece_slots(const uint32_t* ids, size_t n, uint32_t* slots) {
    std::vector<uint32_t> missing;
    for (size_t i = 0; i < n; ++i)
        if (!g_slots.count(ids[i])) {
            bool seen = false;
            for (uint32_t m : missing) seen |= m == ids[i];
            if (!seen) missing.push_back(ids[i]);
        }
    if (!missing.empty()) {
        static const WasmType type = piece_type();
        static const std::vector<uint8_t> header = wasm_header(&type);
        const uint32_t type_index = uint32_t(wasm_types().size());
        std::vector<uint8_t> module = header, section, code, body;
        wasm_leb(code, uint32_t(missing.size()));
        for (uint32_t id : missing) {
            if (!piece_function(id, body)) return false;
            wasm_leb(code, uint32_t(body.size()));
            code.insert(code.end(), body.begin(), body.end());
        }
        wasm_leb(section, uint32_t(missing.size()));
        for (size_t i = 0; i < missing.size(); ++i) wasm_leb(section, type_index);
        wasm_section(module, 3, section);
        section.clear();
        wasm_leb(section, uint32_t(missing.size()));
        for (size_t i = 0; i < missing.size(); ++i) {
            wasm_name(section, std::to_string(i).c_str());
            section.push_back(0x00);
            wasm_leb(section, kWasmBlockFunction + uint32_t(i));
        }
        wasm_section(module, 7, section);
        wasm_section(module, 10, code);
        std::vector<uint32_t> made(missing.size());
        if (!load_shared_functions(module.data(), module.size(), uint32_t(missing.size()), made.data())) return false;
        for (size_t i = 0; i < missing.size(); ++i) g_slots[missing[i]] = made[i];
    }
    for (size_t i = 0; i < n; ++i) slots[i] = g_slots[ids[i]];
    return true;
}

}  // namespace bcore::bcore_cp
