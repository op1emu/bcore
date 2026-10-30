// The copy-and-patch tier's WebAssembly blocks (cp_wasm.h): a scanned
// block's pieces (CpTranslator::scan) as one function of a module of its own
// or of a batch's, their value holes and exits filled in, which the
// CodeLoader instantiates; or a cold block's pieces (cp_cold_wasm.h).

#include "cp_translator.h"

#include <string>
#include <vector>

#include "cp_cold_wasm.h"
#include "cp_wasm.h"

namespace bcore {

using namespace bcore_cp;

namespace {

// value + addend, in a LEB of as few bytes as it takes at out's end: signed
// (kWasmSleb, an i32.const) or unsigned (kWasmLeb, an offset), as 32 bits;
// false if it does not fit. The stencil's own is 5 bytes, whatever its value;
// a piece's branches go by depth, so a shorter one moves nothing.
bool put_leb(std::vector<uint8_t>& out, const Reloc& r, uint64_t value) {
    const int64_t v = int64_t(value) + r.addend;
    if (r.kind == kWasmSleb) {
        if (v < INT32_MIN || v > int64_t(UINT32_MAX)) return false;
        wasm_sleb(out, int32_t(uint32_t(v)));
        return true;
    }
    if (v < 0 || v > int64_t(UINT32_MAX)) return false;
    wasm_leb(out, uint32_t(v));
    return true;
}

}  // namespace

CpTranslator::Block CpTranslator::emit_wasm(uint32_t pc, uint32_t counter, const Config& config, Block out,
                                            Batch* batch) {
    static const std::vector<uint8_t> header = wasm_header();
    static const std::vector<uint8_t> locals = wasm_locals();
    const int exit = !config.chaining ? 0 : config.dry ? (config.count_chained ? 4 : 3) : (config.count_chained ? 2 : 1);
    const uint64_t last_len = last_len_;
    // A piece's code, its value holes filled in, at body_'s end: false if a
    // value does not fit.
    auto place = [&](const Piece& p) {
        size_t from = 0;  // the code up to here is placed
        for (uint32_t i = 0; i < p.stencil->reloc_count; ++i) {
            const Reloc& r = p.relocs[i];
            uint64_t value = 0;
            if (r.offset < from || !is_value_hole(r.hole) || !value_of(p, r, p.packet, last_len, counter, value))
                return false;
            body_.insert(body_.end(), p.code + from, p.code + r.offset);
            if (!put_leb(body_, r, value)) return false;
            from = r.offset + 5u;
        }
        body_.insert(body_.end(), p.code + from, p.code + p.stencil->size);
        return true;
    };
    // The function: the locals, the pieces inside a block, then the exit:
    // the block's own, or the one its chaining takes. A block that runs
    // again past its guard (kSelfExitsLoop) has the guard first, then a loop
    // around the rest, which its exit branches to; one of a batch that runs
    // again (loop_all_), a loop around all of it.
    body_ = locals;
    bool placed = true;
    size_t first = 0;
    if (past_guard_) {
        placed = place(pieces_[0]);
        first = 1;
    }
    const bool loop = past_guard_ || loop_all_;
    if (loop) body_.insert(body_.end(), {0x03, 0x40});
    body_.insert(body_.end(), {0x02, 0x40});
    const size_t pieces = exit_piece_ >= 0 ? size_t(exit_piece_) : pieces_.size();
    for (size_t i = first; i < pieces && placed; ++i) placed = place(pieces_[i]);
    body_.push_back(0x0b);
    placed = placed && place(exit_piece_ >= 0 ? pieces_[size_t(exit_piece_)]
                                              : CpTranslator::piece(kStencils[kExits[exit]], pc, 0, -1));
    if (loop) body_.push_back(0x0b);
    body_.push_back(0x0b);
    if (!placed) {
        out.why = "relocation";
        return out;
    }
    if (batch) {
        out.batched = int(batch->bodies.size());
        batch->bodies.push_back(body_);
        out.size = body_.size();
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

bool CpTranslator::load(Batch& batch, uint32_t pc, std::vector<BbFunc>& fns) {
    static const std::vector<uint8_t> header = wasm_header();
    const uint32_t n = uint32_t(batch.bodies.size());
    module_ = header;
    std::vector<uint8_t> section;
    wasm_leb(section, n);
    section.insert(section.end(), n, 0x00);  // of the blocks' type
    wasm_section(module_, 3, section);
    section.clear();
    wasm_leb(section, n);
    for (uint32_t i = 0; i < n; ++i) {
        wasm_name(section, std::to_string(i).c_str());
        section.push_back(0x00);
        wasm_leb(section, kWasmBlockFunction + i);
    }
    wasm_section(module_, 7, section);
    section.clear();
    wasm_leb(section, n);
    for (const std::vector<uint8_t>& body : batch.bodies) {
        wasm_leb(section, uint32_t(body.size()));
        section.insert(section.end(), body.begin(), body.end());
    }
    wasm_section(module_, 10, section);
    fns.assign(n, nullptr);
    return jit_.loader().load_functions(pc, module_.data(), module_.size(), n, fns.data());
}

bool CpTranslator::translate_cold(uint32_t pc, Memory& code, uint32_t counter, const Config& config, Cold& out) {
    out = Cold{};
    Block block;
    if (config.chaining || config.hint || config.self_loop || !config.careful) {
        out.why = "not a cold block's config";
        return false;
    }
    if (!scan(pc, code, config, block)) {
        out.why = block.why;
        return false;
    }
    threshold_ = out.threshold = tier_threshold(config.threshold, config.size_ref, block.packets);
    out.fallthrough_pc = block.fallthrough_pc;
    out.packets = block.packets;
    // Each piece's hole values, as emit_wasm places them.
    std::vector<uint32_t> ids;
    for (const Piece& p : pieces_) {
        ids.push_back(uint32_t(p.stencil - kStencils));
        out.pieces.push_back({0, uint32_t(out.values.size())});
        for (uint32_t i = 0; i < p.stencil->reloc_count; ++i) {
            const Reloc& r = p.relocs[i];
            uint64_t value = 0;
            if (!is_value_hole(r.hole) || !value_of(p, r, p.packet, last_len_, counter, value)) {
                out.why = "relocation";
                return false;
            }
            const int64_t v = int64_t(value) + r.addend;
            if (v < (r.kind == kWasmSleb ? int64_t(INT32_MIN) : 0) || v > int64_t(UINT32_MAX)) {
                out.why = "relocation";
                return false;
            }
            out.values.push_back(uint32_t(v));
        }
    }
    std::vector<uint32_t> slots(ids.size());
    if (!piece_slots(ids.data(), ids.size(), slots.data())) {
        out.why = "no piece function";
        return false;
    }
    for (size_t i = 0; i < slots.size(); ++i) out.pieces[i].slot = slots[i];
    return true;
}

}  // namespace bcore
