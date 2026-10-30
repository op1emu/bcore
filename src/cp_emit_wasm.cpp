// The copy-and-patch tier's WebAssembly blocks (cp_wasm.h): a scanned
// block's pieces (CpTranslator::scan) as one function of a module of its own,
// their value holes and exits filled in, which the CodeLoader instantiates.

#include "cp_translator.h"

#include <cstring>

#include "cp_wasm.h"

namespace bcore {

using namespace bcore_cp;

namespace {

// value + addend in a 5-byte LEB at at: signed (kWasmSleb, an i32.const)
// or unsigned (kWasmLeb, an offset), as 32 bits; false if it does not fit.
bool patch_leb(uint8_t* at, const Reloc& r, uint64_t value) {
    const int64_t v = int64_t(value) + r.addend;
    if (r.kind == kWasmSleb) {
        if (v < INT32_MIN || v > int64_t(UINT32_MAX)) return false;
        int32_t s = int32_t(uint32_t(v));
        for (int i = 0; i < 5; ++i) {
            at[i] = uint8_t(s & 0x7f) | (i < 4 ? 0x80 : 0);
            s >>= 7;
        }
        return true;
    }
    if (v < 0 || v > int64_t(UINT32_MAX)) return false;
    uint32_t u = uint32_t(v);
    for (int i = 0; i < 5; ++i) {
        at[i] = uint8_t(u & 0x7f) | (i < 4 ? 0x80 : 0);
        u >>= 7;
    }
    return true;
}

}  // namespace

CpTranslator::Block CpTranslator::emit_wasm(uint32_t pc, uint32_t counter, const Config& config, Block out) {
    static const std::vector<uint8_t> header = wasm_header();
    static const std::vector<uint8_t> locals = wasm_locals();
    const int exit = !config.chaining ? 0 : config.dry ? (config.count_chained ? 4 : 3) : (config.count_chained ? 2 : 1);
    const uint64_t last_len = last_len_;
    // A piece's code, its value holes filled in, at body_'s end: false if a
    // value does not fit.
    auto place = [&](const Piece& p) {
        const size_t at = body_.size();
        body_.insert(body_.end(), p.code, p.code + p.stencil->size);
        for (uint32_t i = 0; i < p.stencil->reloc_count; ++i) {
            const Reloc& r = p.relocs[i];
            uint64_t value = 0;
            if (!is_value_hole(r.hole) || !value_of(p, r, p.packet, last_len, counter, value) ||
                !patch_leb(body_.data() + at + r.offset, r, value))
                return false;
        }
        return true;
    };
    // The function: the locals, the pieces inside a block, then the exit:
    // the block's own, or the one its chaining takes.
    body_ = locals;
    body_.insert(body_.end(), {0x02, 0x40});
    const size_t pieces = exit_piece_ >= 0 ? size_t(exit_piece_) : pieces_.size();
    bool placed = true;
    for (size_t i = 0; i < pieces && placed; ++i) placed = place(pieces_[i]);
    body_.push_back(0x0b);
    placed = placed && place(exit_piece_ >= 0 ? pieces_[size_t(exit_piece_)]
                                              : CpTranslator::piece(kStencils[kExits[exit]], pc, 0, -1));
    body_.push_back(0x0b);
    if (!placed) {
        out.why = "relocation";
        return out;
    }
    module_ = header;
    wasm_section(module_, 3, {1, 0x00});  // one function, of the blocks' type
    std::vector<uint8_t> section;
    wasm_leb(section, 1);
    wasm_name(section, "f");
    section.push_back(0x00);
    wasm_leb(section, kWasmBlockFunction);
    wasm_section(module_, 7, section);
    section.clear();
    wasm_leb(section, 1);
    wasm_leb(section, uint32_t(body_.size()));
    section.insert(section.end(), body_.begin(), body_.end());
    wasm_section(module_, 10, section);
    out.fn = jit_.loader().load_module(pc, module_.data(), module_.size(), "f");
    if (!out.fn) {
        out.why = "no module";
        return out;
    }
    out.size = module_.size();
    return out;
}

}  // namespace bcore
