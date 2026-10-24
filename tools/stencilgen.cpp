// bcore-stencilgen: the stencils of the copy-and-patch tier 0 (cp_format.h),
// built from the lifter at build time.
//
//   bcore-stencilgen OUT_DIR [--jobs N]
//
// writes OUT_DIR/cp_stencils.bin (the code), cp_tables.bin (the big tables)
// and cp_stencils.inc (the rest of the tables, which cp_stencils.cpp includes) and cp_stencils.txt (a report: counts, sizes
// and every encoding left without a stencil, with the reason).
//
// Every 16-bit word the decoder takes for a 16-bit instruction gets a
// stencil, and every value of the key fields of each pattern in
// cp_patterns.def (the fields that are not holes). Each is the packet
// BBTranslator::translate_stencil lifts, compiled at O2 for generic x86-64
// (Compiler::init's stencil mode). The checks that make a stencil hold for
// every PC and every value of its holes:
//   - the lift at two PCs is the same IR: nothing in it depends on the PC
//     but the PC hole;
//   - for a 32-bit pattern, lifts with other values of the hole fields
//     (all of them for fields up to 12 bits, 4096 samples beyond) are the
//     same IR, and the values the lifter took for the holes are the ones
//     the pattern's formula gives;
//   - every relocation of the compiled code is one the translator fills
//     in: a value hole as an absolute 32-bit field, the next packet, the
//     exit or a helper as a 32-bit PC-relative one. An encoding whose code
//     has any other gets no stencil (its blocks compile with LLVM).

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "bb_translator.h"
#include "compiler.h"
#include "cp_build.h"
#include "cp_format.h"
#include "cpu_state.h"
#include "mem.h"
#include "tier_table.h"

using namespace llvm;
using namespace bcore_cp;

namespace {

// ---- The decoder's patterns, as decoder.h reads them ----

struct Pattern {
    std::string name;
    uint32_t mask = 0, value = 0;
    std::vector<std::pair<uint32_t, uint32_t>> args;  // (mask, shift), in the decoder's order
};

Pattern parse_pattern(const char* name, const std::string& pattern) {
    Pattern p;
    p.name = name;
    char arg = 0;
    std::vector<std::pair<uint32_t, uint32_t>> args(1);
    for (size_t i = 0, bit = 0; i < pattern.size(); i++) {
        const char c = pattern[pattern.size() - 1 - i];
        if (arg != 0 && arg != c) {
            arg = 0;
            args.emplace_back(0, 0);
        }
        if (c == ' ') continue;
        if (c == '1') p.value |= 1u << bit;
        if (c == '0' || c == '1') {
            p.mask |= 1u << bit;
        } else {
            auto& [mask, shift] = args.back();
            mask |= 1u << bit;
            if (arg != c) shift = uint32_t(bit);
            arg = c;
        }
        bit++;
    }
    if (args.back().first == 0 && args.back().second == 0) args.pop_back();
    std::reverse(args.begin(), args.end());
    p.args = args;
    return p;
}

std::vector<Pattern> patterns32() {
    std::vector<Pattern> out;
#define DEF_INSN(opcode, asm_template, binary_pattern) out.push_back(parse_pattern(#opcode, binary_pattern));
#include "instruction32.def.h"
#undef DEF_INSN
    return out;
}

int first_match(const std::vector<Pattern>& patterns, uint32_t insn) {
    for (size_t i = 0; i < patterns.size(); ++i)
        if ((insn & patterns[i].mask) == patterns[i].value) return int(i);
    return -1;
}

// cp_patterns.def
struct HoleSpec { int arg = -1; bool sign = false; unsigned shift = 0; bool pc = false; };
struct Wanted { const char* name; HoleSpec holes[2]; };
#define CP_NONE HoleSpec{}
#define CP_FIELD(arg, sign, shift, pc) HoleSpec{arg, bool(sign), shift, bool(pc)}
const Wanted kWanted[] = {
#define CP_PATTERN(opcode, hole0, hole1) {#opcode, {hole0, hole1}},
#include "cp_patterns.def"
#undef CP_PATTERN
};
#undef CP_NONE
#undef CP_FIELD

bool is16(uint16_t w) {
    return (w & 0xc000) != 0xc000 || (w & 0xff00) == 0xf800 || (w & 0xff00) == 0xf900 || (w & 0xff00) == 0xff00;
}

bool is_parallel(uint16_t w) { return (w & 0x0800) && (w & 0xe800) != 0xe800; }

// ---- Work ----

// One encoding to make a stencil of.
struct Item {
    enum Kind { kPacket, kBodyPiece, kSlotPiece };
    Kind kind = kPacket;
    int slot = 0;          // a slot piece's: 1 or 2
    uint32_t insn = 0;
    int len = 2;
    int selected = -1;     // index in kWanted for a 32-bit pattern
    uint32_t key = 0;      // its key fields, packed
    bool full = false;     // the pattern's first key: every value of its hole fields checked
    unsigned regs_b[kRegHoles] = {};  // the other registers its register holes are checked with
    bool no_float = false; // compiled without vector code (a constant pool otherwise)
    // Results
    std::string error;
    uint16_t flags = 0;
    uint8_t bytes = 0;
    Code code;
    // A slot piece's variants, v = dis_algn * 2 + did_jump not known clear
    // at its start (slot 1 starts with it known clear: v 1 and 3 none): its
    // writes, flags, and its code, or the variant's whose IR it shares
    // (same_as[v] < v).
    static constexpr int kVariants = 4;
    bool has[kVariants] = {};
    std::vector<LiftVisitor::SlotWrite> writes[kVariants];
    uint16_t slot_flags[kVariants] = {};
    int same_as[kVariants] = {0, 1, 2, 3};
    Code variant_code[kVariants];
};

struct SelectedInfo {
    int pattern = -1;  // in patterns32()
    uint32_t key_mask = 0;
    Formula fields[4] = {};
    // Register holes: data register fields, register hole k's value the
    // field's register number << 2 (infer_regs).
    Formula regs[kRegHoles] = {};
    unsigned reg_count = 0;
    bool pairs = false;  // a hole's register's next is the hole + 4 (register pairs)
    // Its slot 0 pieces lifted as a parallel packet's (writes at the end,
    // registers keyed): it reads a field after writing it.
    bool body_parallel = false;
};

// insn with register field k set to values[k].
uint32_t with_regs(uint32_t insn, const SelectedInfo& info, const unsigned* values) {
    for (unsigned k = 0; k < info.reg_count; ++k) {
        const Formula& f = info.regs[k];
        insn = (insn & ~(7u << f.lsb)) | (values[k] << f.lsb);
    }
    return insn;
}

// Two assignments of registers to the register holes, a and b, which name
// no register twice (with pairs, the next register too) and none the
// instruction's other 3-bit fields name, nor the next of those (a pair's
// other half: the lifter maps registers, not fields, to holes, and reaching
// a mapped one through a constant field fails the check), and differ for
// every hole: false if there are not enough.
bool pick_regs(uint32_t insn, const SelectedInfo& info, const std::vector<std::pair<uint32_t, uint32_t>>& args,
               unsigned* a, unsigned* b) {
    unsigned used = 0;
    for (auto [mask, shift] : args) {
        if (__builtin_popcount(mask) != 3) continue;
        bool hole = false;
        for (unsigned k = 0; k < info.reg_count; ++k) hole |= info.regs[k].lsb == shift;
        // A field may name a pair, its register and the next.
        if (!hole) used |= 3u << ((insn >> shift) & 7);
    }
    used &= 0xff;
    static constexpr unsigned kOrderA[8] = {0, 2, 4, 6, 1, 3, 5, 7}, kOrderB[8] = {5, 3, 1, 7, 6, 4, 2, 0};
    const unsigned span = info.pairs ? 3u : 1u;  // the register, and with pairs its next
    unsigned ta = used, tb = used;
    for (unsigned k = 0; k < info.reg_count; ++k) {
        a[k] = b[k] = 8;
        for (unsigned r : kOrderA)
            if (!(ta & (span << r)) && r + (info.pairs ? 1 : 0) < 8) {
                a[k] = r;
                ta |= span << r;
                break;
            }
        for (unsigned r : kOrderB)
            if (!(tb & (span << r)) && r + (info.pairs ? 1 : 0) < 8 && r != a[k]) {
                b[k] = r;
                tb |= span << r;
                break;
            }
        if (a[k] == 8 || b[k] == 8) return false;
    }
    return true;
}

// A worker: a context, a translator, a compiler.
class Worker {
public:
    Worker() : translator_(&memory_) {
        translator_.set_lazy_state(true);
        compiler_.init(2, 2, /*split=*/true, /*stencil=*/true);
    }

    // Lifts item at pc into mod as `name`; false with item.error set if the
    // lift is not one stencil.
    BBTranslator::StencilLift lift(Module& mod, const Item& item, uint32_t pc, const char* name,
                                   const SelectedInfo* info = nullptr) {
        memory_.set(pc, item.insn, item.len);
        return translator_.translate_stencil(mod, name, pc, holes_for(mod, item, info));
    }
    BBTranslator::StencilLift lift_body(Module& mod, const Item& item, uint32_t pc, const char* name,
                                        const SelectedInfo* info, bool parallel) {
        memory_.set(pc, item.insn, 4);
        return translator_.translate_stencil_body(mod, name, pc, holes_for(mod, item, info), parallel);
    }
    BBTranslator::StencilLift lift_slot(Module& mod, uint16_t word, int slot, int variant, uint32_t pc,
                                        const char* name) {
        uint16_t words[4] = {0xc803, 0x1800, 0, 0};  // MNOP, then the slots
        words[slot + 1] = word;
        memory_.set_words(pc, words, 4);
        return translator_.translate_slot_stencil(mod, name, pc, packet_holes(mod), slot, variant >> 1, variant & 1);
    }
    BBTranslator::StencilHoles holes_for(Module& mod, const Item& item, const SelectedInfo* info) {
        auto holes = packet_holes(mod);
        if (info) {
            auto* i64 = Type::getInt64Ty(mod.getContext());
            for (unsigned k = 0; k < info->reg_count; ++k)
                holes.regs[(item.insn >> info->regs[k].lsb) & 7] = value_hole(mod, Hole(kHoleReg0 + k), 0, 32, i64);
            // A register pair's other half: the next register, at the next
            // offset (dpregs is contiguous; R7's next is P0, as for the
            // lifter), where no field names it.
            for (unsigned k = 0; k < info->reg_count && info->pairs; ++k) {
                const unsigned r = (item.insn >> info->regs[k].lsb) & 7;
                if (r + 1 < 8 && !holes.regs[r + 1])
                    holes.regs[r + 1] = ConstantExpr::getAdd(value_hole(mod, Hole(kHoleReg0 + k), 0, 32, i64),
                                                             ConstantInt::get(i64, 4));
            }
        }
        return holes;
    }

    // The checks of the file comment, then the stencil's IR into batch.
    void prepare(Module& batch, Item& item, const std::vector<Pattern>& patterns,
                 const std::vector<SelectedInfo>& selected, const char* name) {
        if (item.kind == Item::kBodyPiece) {
            prepare_body(batch, item, patterns, selected, name);
            return;
        }
        if (item.kind == Item::kSlotPiece) {
            prepare_slot(batch, item, name);
            return;
        }
        auto& ctx = batch.getContext();
        Module scratch("scratch", ctx);
        const SelectedInfo* regs =
            item.selected >= 0 && selected[item.selected].reg_count ? &selected[item.selected] : nullptr;
        auto a = lift(scratch, item, kPcA, "s", regs);
        const std::string text = ir_text(a.fn);
        a.fn->eraseFromParent();
        // An illegal 32-bit instruction raises its exception as a packet of 2.
        if (a.bytes != item.len && !(a.terminated && item.len == 4)) {
            item.error = "decodes to another length";
            return;
        }
        auto b = lift(scratch, item, kPcB, "s", regs);
        const bool same = ir_text(b.fn) == text;
        b.fn->eraseFromParent();
        if (!same) {
            item.error = "IR depends on the PC";
            return;
        }
        if (regs) {
            // Other registers in the holes: the same IR, and no other way to them.
            if (a.reg_violation) {
                item.error = "a register hole reached another way";
                return;
            }
            Item other = item;
            other.insn = with_regs(item.insn, *regs, item.regs_b);
            auto c = lift(scratch, other, kPcA, "s", regs);
            const bool same_regs = ir_text(c.fn) == text && !c.reg_violation;
            c.fn->eraseFromParent();
            if (!same_regs) {
                item.error = "IR depends on a register field's value";
                return;
            }
        }
        if (item.selected >= 0) {
            const SelectedInfo& info = selected[item.selected];
            if (!check_fields(scratch, item, info, patterns, a, text)) return;
        } else if (a.fields_used) {
            item.error = "a 16-bit instruction with field holes";
            return;
        }
        auto lifted = lift(batch, item, kPcA, name, regs);
        if (item.no_float) lifted.fn->addFnAttr(Attribute::NoImplicitFloat);
        if (verifyFunction(*lifted.fn, &errs())) {
            item.error = "IR does not verify";
            lifted.fn->eraseFromParent();
            return;
        }
        item.bytes = uint8_t(lifted.bytes);
        item.flags = (lifted.terminated ? kTerminates : 0) | (lifted.cycles ? kCycles : 0) |
                     (item.len == 2 && item.insn == 0x0020 ? kIdle : 0);
    }

    // A parallel packet's slot 0 alone (a 32-bit key with the multi-issue
    // bit): the same checks as a packet's, and no field read after its
    // write unless the pattern's pieces are lifted as a parallel packet's.
    // An encoding slot 0 does not take is the packet that raises the
    // undefined instruction's exception, as the decoder makes it.
    void prepare_body(Module& batch, Item& item, const std::vector<Pattern>& patterns,
                      const std::vector<SelectedInfo>& selected, const char* name) {
        auto& ctx = batch.getContext();
        Module scratch("scratch", ctx);
        const SelectedInfo& info = selected[item.selected];
        const SelectedInfo* regs = info.reg_count ? &info : nullptr;
        const bool parallel = info.body_parallel;
        auto a = lift_body(scratch, item, kPcA, "s", regs, parallel);
        const std::string text = ir_text(a.fn);
        a.fn->eraseFromParent();
        if (a.terminated) {
            item.kind = Item::kPacket;
            prepare(batch, item, patterns, selected, name);
            return;
        }
        auto b = lift_body(scratch, item, kPcB, "s", regs, parallel);
        const bool same = ir_text(b.fn) == text;
        b.fn->eraseFromParent();
        if (!same) {
            item.error = "IR depends on the PC";
            return;
        }
        if (a.reads_after_write) {
            item.error = "slot 0 reads a field after writing it";
            return;
        }
        if (regs) {
            if (a.reg_violation) {
                item.error = "a register hole reached another way";
                return;
            }
            Item other = item;
            other.insn = with_regs(item.insn, *regs, item.regs_b);
            auto c = lift_body(scratch, other, kPcA, "s", regs, parallel);
            const bool same_regs = ir_text(c.fn) == text && !c.reg_violation;
            c.fn->eraseFromParent();
            if (!same_regs) {
                item.error = "IR depends on a register field's value";
                return;
            }
        }
        auto lifted = lift_body(batch, item, kPcA, name, regs, parallel);
        if (verifyFunction(*lifted.fn, &errs())) {
            item.error = "IR does not verify";
            lifted.fn->eraseFromParent();
            return;
        }
        item.bytes = 4;
        item.flags = kBody | (lifted.dis_algn ? kSetsDisAlgn : 0) | (lifted.cycles ? kCycles : 0);
    }

    // A 16-bit word in slot 1 or 2, in each variant: legal there, the
    // same IR at another PC, its writes fitting the scratch, no way out but
    // the packet's exits.
    void prepare_slot(Module& batch, Item& item, const char* name) {
        auto& ctx = batch.getContext();
        Module scratch("scratch", ctx);
        std::string texts[Item::kVariants];
        for (int v = 0; v < Item::kVariants; ++v) {
            if (item.slot == 1 && (v & 1)) continue;
            auto a = lift_slot(scratch, uint16_t(item.insn), item.slot, v, kPcA, "s");
            texts[v] = ir_text(a.fn);
            a.fn->eraseFromParent();
            if (!a.legal) {
                item.error = v ? "legal in a slot only without DISALGNEXCPT" : "not legal in the slot";
                return;
            }
            auto b = lift_slot(scratch, uint16_t(item.insn), item.slot, v, kPcB, "s");
            const bool same = ir_text(b.fn) == texts[v];
            b.fn->eraseFromParent();
            if (!same) item.error = "IR depends on the PC";
            else if (a.slot_conflict) item.error = "the slot's writes do not fit the scratch";
            else if (a.terminated) item.error = "the slot ends the packet";
            if (!item.error.empty()) return;
            item.has[v] = true;
            item.writes[v] = a.writes;
            item.slot_flags[v] = (a.checks_cec ? kSlotStore : 0) | (a.cycles ? kCycles : 0) |
                                 (a.did_jump_clear ? 0 : kDjUnknown);
            for (int u = 0; u < v; ++u)
                if (item.has[u] && texts[u] == texts[v] && item.same_as[u] == u) {
                    item.same_as[v] = u;
                    break;
                }
        }
        for (int v = 0; v < Item::kVariants; ++v) {
            if (!item.has[v] || item.same_as[v] != v) continue;
            const std::string n = std::string(name) + "_" + std::to_string(v);
            auto lifted = lift_slot(batch, uint16_t(item.insn), item.slot, v, kPcA, n.c_str());
            if (item.no_float) lifted.fn->addFnAttr(Attribute::NoImplicitFloat);
            if (verifyFunction(*lifted.fn, &errs())) {
                item.error = "IR does not verify";
                lifted.fn->eraseFromParent();
                return;
            }
        }
        item.bytes = 8;
    }

    // Other values of the hole fields: the same IR, and the formula's values.
    bool check_fields(Module& scratch, Item& item, const SelectedInfo& info, const std::vector<Pattern>& patterns,
                      const BBTranslator::StencilLift& ref, const std::string& text) {
        const Pattern& p = patterns[info.pattern];
        auto expect = [&](const BBTranslator::StencilLift& lift, uint32_t insn) {
            for (unsigned i = 0; i < 4; ++i) {
                if (!(lift.fields_used & (1u << i))) continue;
                if (!(info.fields[i].flags & kFormulaUsed)) return false;
                if (field_hole_value(info.fields[i], insn, 0) != lift.field_values[i]) return false;
            }
            return true;
        };
        if (!expect(ref, item.insn)) {
            item.error = "a hole's value is not the formula's";
            return false;
        }
        std::vector<uint32_t> samples;
        build_samples(item, info, p, samples);
        for (uint32_t insn : samples) {
            if (first_match(patterns, insn) != info.pattern) continue;
            Item other = item;
            other.insn = insn;
            auto lifted = lift(scratch, other, kPcA, "s", info.reg_count ? &info : nullptr);
            const bool same = ir_text(lifted.fn) == text;
            const bool values = expect(lifted, insn);
            lifted.fn->eraseFromParent();
            if (!same) {
                item.error = "IR depends on a hole field's value";
                return false;
            }
            if (!values) {
                item.error = "a hole's value is not the formula's";
                return false;
            }
        }
        return true;
    }

    // The encodings with other values of the hole fields: all of them for
    // a key's first stencil when the fields have up to 12 bits, else 4096
    // (edges, single bits, random); 8 for the other keys.
    void build_samples(const Item& item, const SelectedInfo& info, const Pattern& p, std::vector<uint32_t>& out) {
        uint32_t hole_mask = 0;
        for (auto& f : info.fields)
            if (f.flags & kFormulaUsed) hole_mask |= ((f.bits >= 32 ? 0u : 1u << f.bits) - 1u) << f.lsb;
        const uint32_t base = item.insn & ~hole_mask;
        const bool full = item.full;
        const unsigned bits = __builtin_popcount(hole_mask);
        auto spread = [&](uint32_t v) {  // v's bits into hole_mask's
            uint32_t r = 0;
            for (uint32_t m = hole_mask; m; m &= m - 1, v >>= 1)
                if (v & 1) r |= m & -m;
            return base | r;
        };
        if (full && bits <= 12) {
            for (uint32_t v = 0; v < (1u << bits); ++v) out.push_back(spread(v));
            return;
        }
        std::mt19937 rng(item.insn);
        const unsigned count = full ? 4096 : 8;
        for (unsigned b = 0; b < bits && out.size() < count; ++b) out.push_back(spread(1u << b));
        for (uint32_t v : {0u, 1u, 2u, 3u, ~0u, ~0u - 1}) out.push_back(spread(v));
        while (out.size() < count) out.push_back(spread(rng()));
    }

    void compile(Module& mod, std::map<std::string, Code>& out) {
        mod.setDataLayout(compiler_.dataLayout());
        mod.setTargetTriple(compiler_.targetTriple());
        make_local(mod);
        compiler_.optimize(mod);
        auto object = compiler_.emit(mod);
        if (object) out = extract(*object);
    }

    BBTranslator& translator() { return translator_; }

private:
    StencilMemory memory_;
    BBTranslator translator_;
    Compiler compiler_;
};

// ---- Output ----

struct Blob {
    std::vector<uint8_t> code;
    std::vector<bcore_cp::Reloc> relocs;
    std::vector<Stencil> stencils;
    std::map<std::string, uint32_t> dedup;  // bytes + relocs -> stencil

    uint32_t add(const Code& c, uint16_t flags, uint8_t bytes) {
        std::string key(c.bytes.begin(), c.bytes.end());
        key.append(reinterpret_cast<const char*>(c.relocs.data()), c.relocs.size() * sizeof(bcore_cp::Reloc));
        key.push_back('|');
        key.append(c.cold.begin(), c.cold.end());
        key.append(reinterpret_cast<const char*>(c.cold_relocs.data()), c.cold_relocs.size() * sizeof(bcore_cp::Reloc));
        // The parts' lengths, so that no two of them run into each other.
        for (size_t n : {c.bytes.size(), c.relocs.size(), c.cold.size(), c.cold_relocs.size()})
            key.append(reinterpret_cast<const char*>(&n), sizeof n);
        key.append(reinterpret_cast<const char*>(&flags), sizeof flags);
        key.push_back(char(bytes));
        auto it = dedup.find(key);
        if (it != dedup.end()) return it->second;
        while (code.size() % 4) code.push_back(0xcc);  // int3, never run
        Stencil s{};
        s.code = uint32_t(code.size());
        s.size = uint16_t(c.bytes.size());
        s.flags = flags;
        s.flags |= code_flags(c);
        s.relocs = uint32_t(relocs.size());
        s.reloc_count = uint16_t(c.relocs.size());
        s.bytes = bytes;
        code.insert(code.end(), c.bytes.begin(), c.bytes.end());
        relocs.insert(relocs.end(), c.relocs.begin(), c.relocs.end());
        if (!c.cold.empty()) {
            while (code.size() % 4) code.push_back(0xcc);
            s.cold = uint32_t(code.size());
            s.cold_size = uint16_t(c.cold.size());
            code.insert(code.end(), c.cold.begin(), c.cold.end());
        }
        s.cold_relocs = uint32_t(relocs.size());
        s.cold_reloc_count = uint16_t(c.cold_relocs.size());
        relocs.insert(relocs.end(), c.cold_relocs.begin(), c.cold_relocs.end());
        stencils.push_back(s);
        return dedup[key] = uint32_t(stencils.size() - 1);
    }
};

// A pattern's register holes: its 3-bit fields as data registers (with
// pairs if that takes more), the largest set of them for which lifts with
// two other assignments of registers are the same IR and reach the
// registers no other way. None for a pattern with more 3-bit fields than
// holes, or with no encoding the lifter takes.
void infer_regs(Worker& worker, const std::vector<Pattern>& patterns, SelectedInfo& info) {
    const Pattern& p = patterns[info.pattern];
    // An encoding of the pattern the lifter takes as a legal 4-byte
    // instruction, its multi-issue bit clear (a lone instruction).
    uint32_t base = 0;
    bool found = false;
    {
        std::mt19937 rng(p.value);
        LLVMContext ctx;
        Module mod("probe", ctx);
        for (int attempt = 0; attempt < 256 && !found; ++attempt) {
            const uint32_t fill = attempt == 0 ? 0x5a5a5a5au : attempt == 1 ? 0u : uint32_t(rng());
            base = p.value | (fill & ~p.mask & ~0x08000000u);
            if (first_match(patterns, base) != info.pattern || is16(uint16_t(base >> 16)) ||
                is_parallel(uint16_t(base >> 16)))
                continue;
            Item probe;
            probe.len = 4;
            probe.insn = base;
            auto l = worker.lift(mod, probe, kPcA, "s");
            found = l.bytes == 4 && !l.terminated;
            l.fn->eraseFromParent();
        }
    }
    if (!found) return;
    std::vector<unsigned> cand;
    for (unsigned i = 0; i < p.args.size(); ++i)
        if (__builtin_popcount(p.args[i].first) == 3) cand.push_back(i);
    if (cand.empty() || cand.size() > kRegHoles) return;
    SelectedInfo best = info;
    auto trial = [&](unsigned subset, bool pairs) {
        SelectedInfo t = info;
        t.pairs = pairs;
        t.reg_count = 0;
        for (unsigned j = 0; j < cand.size(); ++j)
            if (subset >> j & 1) t.regs[t.reg_count++] = {uint8_t(p.args[cand[j]].second), 3, 2, kFormulaUsed};
        unsigned a[kRegHoles], b[kRegHoles];
        if (!pick_regs(base, t, p.args, a, b)) return false;
        LLVMContext ctx;
        Module mod("infer", ctx);
        Item ia;
        ia.len = 4;
        ia.insn = with_regs(base, t, a);
        Item ib = ia;
        ib.insn = with_regs(base, t, b);
        if (first_match(patterns, ia.insn) != info.pattern || first_match(patterns, ib.insn) != info.pattern)
            return false;
        auto la = worker.lift(mod, ia, kPcA, "s", &t);
        const std::string ta = ir_text(la.fn);
        la.fn->eraseFromParent();
        auto lb = worker.lift(mod, ib, kPcA, "s", &t);
        const bool ok = !la.reg_violation && !lb.reg_violation && la.bytes == 4 && ta == ir_text(lb.fn);
        lb.fn->eraseFromParent();
        if (ok && t.reg_count > best.reg_count) best = t;
        return ok;
    };
    // Largest sets first; without pairs, then with them if that takes more.
    const unsigned all = (1u << cand.size()) - 1;
    for (bool pairs : {false, true}) {
        bool done = false;
        for (int size = int(cand.size()); size > 0 && !done; --size)
            for (unsigned subset = all; subset && !done; --subset)
                if (__builtin_popcount(subset) == size && size > int(best.reg_count)) done = trial(subset, pairs);
    }
    info = best;
}

// Whether a pattern's slot 0 pieces read a field after writing it (with
// its register holes: a data register after another's store), on samples
// of its keys: then they are lifted as a parallel packet's, which make
// their writes at the end (a piece whose lift then reaches a register
// hole, which the packet's shadow cannot key, has no stencil).
void probe_body(Worker& worker, const std::vector<Pattern>& patterns, SelectedInfo& info) {
    const Pattern& p = patterns[info.pattern];
    constexpr uint32_t kM = 0x08000000u;
    std::mt19937 rng(p.value ^ 0x5bd1e995u);
    LLVMContext ctx;
    Module mod("probe", ctx);
    const SelectedInfo* regs = info.reg_count ? &info : nullptr;
    for (int sample = 0; sample < 256; ++sample) {
        uint32_t insn = p.value | kM | (uint32_t(rng()) & ~p.mask);
        if (regs) {
            unsigned a[kRegHoles], b[kRegHoles];
            if (!pick_regs(insn, info, p.args, a, b)) continue;
            insn = with_regs(insn, info, a);
        }
        if (first_match(patterns, insn) != info.pattern) continue;
        Item item;
        item.insn = insn;
        auto l = worker.lift_body(mod, item, kPcA, "s", regs, false);
        const bool raw = !l.terminated && l.reads_after_write;
        l.fn->eraseFromParent();
        if (raw) {
            info.body_parallel = true;
            return;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s OUT_DIR [--jobs N]\n", argv[0]);
        return 2;
    }
    const std::string out_dir = argv[1];
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    long dump = -1;  // --dump WORD: print a 16-bit word's lifts at both PCs, and stop
    for (int i = 2; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--jobs")) jobs = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--dump")) dump = strtol(argv[++i], nullptr, 0);
    }

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    const std::vector<Pattern> patterns = patterns32();
    if (dump >= 0) {
        Worker worker;
        LLVMContext ctx;
        Module mod("dump", ctx);
        Item item;
        item.insn = uint32_t(dump);
        item.len = dump > 0xffff ? 4 : 2;
        for (uint32_t pc : {kPcA, kPcB}) {
            auto lifted = worker.lift(mod, item, pc, pc == kPcA ? "a" : "b");
            printf("%s", ir_text(lifted.fn).c_str());
        }
        return 0;
    }
    // cp_patterns.def's, then every DSP instruction, with register holes.
    std::vector<Wanted> wanted(std::begin(kWanted), std::end(kWanted));
    const size_t first_auto = wanted.size();
    for (const Pattern& p : patterns) {
        bool listed = false;
        for (const Wanted& w : kWanted) listed |= p.name == w.name;
        if (!listed && p.name.rfind("dsp32", 0) == 0) wanted.push_back({p.name.c_str(), {HoleSpec{}, HoleSpec{}}});
    }
    std::vector<SelectedInfo> selected;
    std::vector<std::string> problems;
    Worker inferrer;
    for (const Wanted& w : wanted) {
        SelectedInfo info;
        for (size_t i = 0; i < patterns.size(); ++i)
            if (patterns[i].name == w.name) info.pattern = int(i);
        if (info.pattern < 0) {
            fprintf(stderr, "cp_patterns.def: no pattern %s\n", w.name);
            return 1;
        }
        const Pattern& p = patterns[info.pattern];
        uint32_t holes = 0;
        for (int h = 0; h < 2; ++h) {
            const HoleSpec& spec = w.holes[h];
            if (spec.arg < 0) continue;
            if (spec.arg >= int(p.args.size())) {
                fprintf(stderr, "cp_patterns.def: %s has no argument %d\n", w.name, spec.arg);
                return 1;
            }
            auto [mask, shift] = p.args[spec.arg];
            info.fields[h] = {uint8_t(shift), uint8_t(__builtin_popcount(mask)), uint8_t(spec.shift),
                              uint8_t(kFormulaUsed | (spec.sign ? kFormulaSign : 0) | (spec.pc ? kFormulaPc : 0))};
            holes |= mask;
        }
        if (selected.size() >= first_auto) {
            infer_regs(inferrer, patterns, info);
            probe_body(inferrer, patterns, info);
        }
        for (unsigned k = 0; k < info.reg_count; ++k) holes |= 7u << info.regs[k].lsb;
        info.key_mask = ~p.mask & ~holes;
        selected.push_back(info);
    }

    // The encodings.
    std::vector<Item> items;
    for (uint32_t w = 0; w < 0x10000; ++w)
        if (is16(uint16_t(w))) {
            Item item;
            item.insn = w;
            items.push_back(item);
        }
    const size_t first32 = items.size();
    for (size_t s = 0; s < selected.size(); ++s) {
        const SelectedInfo& info = selected[s];
        const Pattern& p = patterns[info.pattern];
        const unsigned key_bits = __builtin_popcount(info.key_mask);
        if (key_bits > 16) {  // a stencil for each of more keys than that: none
            fprintf(stderr, "stencilgen: %s has %u key bits, no stencils\n", p.name.c_str(), key_bits);
            selected[s].key_mask = 0;
            Item item;
            item.insn = p.value;
            item.len = 4;
            item.selected = int(s);
            item.error = "too many keys";
            items.push_back(item);
            continue;
        }
        // A value of the hole fields that is no special case.
        uint32_t hole_bits = 0;
        for (auto& f : info.fields)
            if (f.flags & kFormulaUsed) hole_bits |= (0x5a5a5a5au & ((f.bits >= 32 ? 0u : 1u << f.bits) - 1u)) << f.lsb;
        bool first = true;
        for (uint32_t key = 0; key < (1u << key_bits); ++key) {
            uint32_t spread = 0, v = key;
            for (uint32_t m = info.key_mask; m; m &= m - 1, v >>= 1)
                if (v & 1) spread |= m & -m;
            uint32_t insn = p.value | spread | hole_bits;
            Item item;
            if (info.reg_count) {
                unsigned a[kRegHoles];
                if (!pick_regs(insn, info, p.args, a, item.regs_b)) item.error = "no registers left for the holes";
                insn = with_regs(insn, info, a);
            }
            item.insn = insn;
            item.len = 4;
            item.selected = int(s);
            item.key = key;
            if (!item.error.empty()) {
            } else if (is_parallel(uint16_t(insn >> 16)) && s >= first_auto &&
                       first_match(patterns, insn) == info.pattern) {
                item.kind = Item::kBodyPiece;  // slot 0 of a parallel packet
            } else if (is16(uint16_t(insn >> 16)) || is_parallel(uint16_t(insn >> 16)))
                item.error = "not a 32-bit encoding";
            else if (first_match(patterns, insn) != info.pattern)
                item.error = "an earlier pattern takes it";
            item.full = first && item.error.empty();
            first = first && !item.full;
            items.push_back(item);
        }
    }

    // Slots 1 and 2 of a parallel packet: every 16-bit word.
    const size_t first_slot = items.size();
    for (int slot = 1; slot <= 2; ++slot)
        for (uint32_t w = 0; w < 0x10000; ++w)
            if (is16(uint16_t(w))) {
                Item item;
                item.kind = Item::kSlotPiece;
                item.slot = slot;
                item.insn = w;
                items.push_back(item);
            }
    // A 32-bit encoding no pattern takes: the undefined instruction, the
    // same IR for every such encoding (checked on samples).
    const size_t undef_item = items.size();
    {
        std::vector<uint32_t> undef;
        std::mt19937 rng(32);
        for (int tries = 0; undef.size() < 4096 && tries < (1 << 22); ++tries) {
            const uint32_t insn = undef.empty() ? 0xc0000000u | 0x07ff0000u : uint32_t(rng());
            if (!is16(uint16_t(insn >> 16)) && !is_parallel(uint16_t(insn >> 16)) && first_match(patterns, insn) < 0)
                undef.push_back(insn);
        }
        Item item;
        item.len = 4;
        if (undef.empty()) {
            item.error = "no undefined 32-bit encoding";
        } else {
            item.insn = undef[0];
            Worker worker;
            LLVMContext ctx;
            Module mod("undef", ctx);
            std::string ref;
            for (uint32_t insn : undef) {
                Item other = item;
                other.insn = insn;
                auto l = worker.lift(mod, other, kPcA, "u");
                const std::string text = ir_text(l.fn);
                l.fn->eraseFromParent();
                if (ref.empty()) ref = text;
                if (text != ref) {
                    item.error = "the undefined instruction's IR depends on its encoding";
                    break;
                }
            }
        }
        items.push_back(item);
    }

    // Lift, check and compile, in batches of 64 per module.
    auto build = [&](const std::vector<size_t>& todo) {
        constexpr size_t kBatch = 64;
        std::atomic<size_t> next{0};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < jobs; ++t)
            threads.emplace_back([&] {
                Worker worker;
                for (;;) {
                    const size_t start = next.fetch_add(kBatch);
                    if (start >= todo.size()) break;
                    const size_t end = std::min(todo.size(), start + kBatch);
                    LLVMContext ctx;
                    Module mod("stencils", ctx);
                    std::vector<std::string> names(end - start);
                    for (size_t j = start; j < end; ++j) {
                        Item& item = items[todo[j]];
                        if (!item.error.empty()) continue;
                        names[j - start] = "st_" + std::to_string(todo[j]);
                        worker.prepare(mod, item, patterns, selected, names[j - start].c_str());
                    }
                    std::map<std::string, Code> code;
                    worker.compile(mod, code);
                    for (size_t j = start; j < end; ++j) {
                        Item& item = items[todo[j]];
                        if (!item.error.empty()) continue;
                        if (item.kind == Item::kSlotPiece) {
                            for (int v = 0; v < Item::kVariants && item.error.empty(); ++v) {
                                if (!item.has[v] || item.same_as[v] != v) continue;
                                auto it = code.find(names[j - start] + "_" + std::to_string(v));
                                if (it == code.end()) item.error = "no code";
                                else if (!it->second.error.empty()) item.error = it->second.error;
                                else item.variant_code[v] = std::move(it->second);
                            }
                            continue;
                        }
                        auto it = code.find(names[j - start]);
                        if (it == code.end()) item.error = "no code";
                        else if (!it->second.error.empty()) item.error = it->second.error;
                        else item.code = std::move(it->second);
                    }
                }
            });
        for (auto& t : threads) t.join();
    };
    {
        std::vector<size_t> all(items.size());
        for (size_t i = 0; i < items.size(); ++i) all[i] = i;
        build(all);
    }
    // Code that loads a constant from a pool (vector code the lifter's
    // stores turned into) is no stencil: again without vector code.
    {
        std::vector<size_t> again;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].error.rfind("relocation against '.LCPI", 0) == 0) {
                items[i].error.clear();
                items[i].no_float = true;
                again.push_back(i);
            }
        if (!again.empty()) build(again);
    }

    // The prologues and exits.
    Blob blob;
    std::vector<uint32_t> prologue(2), exits(5), self_exits(2);
    uint32_t fallthrough = 0, parallel_tail[4] = {};
    std::string special_error;
    {
        Worker worker;
        LLVMContext ctx;
        Module mod("special", ctx);
        auto* i32 = Type::getInt32Ty(ctx);
        auto* cont = code_hole(mod, kContName);
        auto* counter = value_hole(mod, kHoleCounter, 0, TierTable::kCounters, i32);
        auto* offset = value_hole(mod, kHoleCountOffset, 0, 1ull << 31, Type::getInt64Ty(ctx));
        auto* threshold = value_hole(mod, kHoleThreshold, 0, 1ull << 32, i32);
        for (int h = 0; h < 2; ++h)
            worker.translator().emit_stencil_prologue(mod, h ? "prologue_cycles" : "prologue", counter, offset,
                                                      threshold, h, cont);
        // Exits: no chaining; chaining; counting chained blocks; dry; dry counting.
        const char* names[] = {"exit_return", "exit_chain", "exit_chain_count", "exit_dry", "exit_dry_count"};
        for (int e = 0; e < 5; ++e) {
            worker.translator().set_chaining(e > 0, e >= 3);
            worker.translator().set_count_chained(e == 2 || e == 4);
            worker.translator().emit_stencil_exit(mod, names[e]);
        }
        // A block's own exits: chaining, without and with chained blocks counted.
        const char* self_names[] = {"exit_self", "exit_self_count"};
        auto* block_pc = value_hole(mod, kHoleBlockPc, 0, 1ull << 32, i32);
        auto* self = code_hole(mod, kSelfName);
        for (int e = 0; e < 2; ++e) {
            worker.translator().set_chaining(true, false);
            worker.translator().set_count_chained(e == 1);
            worker.translator().emit_stencil_exit(mod, self_names[e], block_pc, self);
        }
        // A block cut at its last packet (BBTranslator's packet cap) leaves
        // with the next PC and the packet's length and index stored, as a
        // block's return does: holes pc, field 0 and packet.
        {
            auto* fn = BBTranslator::declare_block(mod, "fallthrough");
            fn->addFnAttr(Attribute::NoUnwind);
            IRBuilder<> b(BasicBlock::Create(ctx, "entry", fn));
            auto store = [&](Constant* value, size_t offset) {
                b.CreateStore(value, b.CreateGEP(b.getInt8Ty(), fn->getArg(0), b.getInt64(offset)));
            };
            store(value_hole(mod, kHolePc, 0, 1ull << 32, i32), offsetof(CpuState, pc));
            store(value_hole(mod, kHoleField0, 0, 1ull << 32, i32), offsetof(CpuState, insn_len));
            store(value_hole(mod, kHolePacket, 1, 257, i32), offsetof(CpuState, packets));
            auto* call = b.CreateCall(code_hole(mod, kExitName), {fn->getArg(0), fn->getArg(1)});
            call->setCallingConv(CallingConv::GHC);
            call->setTailCallKind(CallInst::TCK_MustTail);
            b.CreateRetVoid();
        }
        // A parallel packet's tail, without and with the CEC check: the same
        // IR at another PC.
        const char* tail_names[] = {"ptail", "ptail_cec", "ptail_dj", "ptail_dj_cec"};
        for (int t = 0; t < 4; ++t) {
            Module scratch("tail", ctx);
            std::string texts[2];
            for (int k = 0; k < 2; ++k) {
                auto l = worker.translator().translate_parallel_tail(scratch, "t", k ? kPcB : kPcA,
                                                                     packet_holes(scratch), t & 1, t >> 1);
                texts[k] = ir_text(l.fn);
                l.fn->eraseFromParent();
            }
            if (texts[0] != texts[1]) special_error += std::string(tail_names[t]) + ": IR depends on the PC\n";
            worker.translator().translate_parallel_tail(mod, tail_names[t], kPcA, packet_holes(mod), t & 1, t >> 1);
        }
        // The exits an arena shares are copied whole (CpTranslator::arena_for).
        for (const char* name : names) mod.getFunction(name)->addFnAttr(Compiler::kNoSplitAttr);
        std::map<std::string, Code> code;
        worker.compile(mod, code);
        auto add = [&](const char* name) -> uint32_t {
            auto it = code.find(name);
            if (it == code.end() || !it->second.error.empty()) {
                special_error += std::string(name) + ": " + (it == code.end() ? "no code" : it->second.error) + "\n";
                return 0;
            }
            return blob.add(it->second, 0, 0);
        };
        prologue[0] = add("prologue");
        prologue[1] = add("prologue_cycles");
        for (int e = 0; e < 5; ++e) exits[e] = add(names[e]);
        for (int e = 0; e < 2; ++e) self_exits[e] = add(self_names[e]);
        fallthrough = add("fallthrough");
        for (int t = 0; t < 4; ++t) parallel_tail[t] = add(tail_names[t]);
    }
    if (!special_error.empty()) {
        fprintf(stderr, "stencilgen: %s", special_error.c_str());
        return 1;
    }

    // The slots' commits: one per slot and list of writes.
    std::map<std::string, uint32_t> commit_of;  // slot and writes -> stencil
    auto commit_key = [](int slot, const std::vector<LiftVisitor::SlotWrite>& writes) {
        std::string k(1, char('0' + slot));
        for (auto& w : writes) k += ":" + std::to_string(w.offset) + "." + std::to_string(w.half);
        return k;
    };
    {
        std::vector<std::pair<int, std::vector<LiftVisitor::SlotWrite>>> commits;
        for (size_t i = first_slot; i < undef_item; ++i) {
            const Item& item = items[i];
            if (!item.error.empty()) continue;
            for (int v = 0; v < Item::kVariants; ++v)
                if (item.has[v] && !item.writes[v].empty() &&
                    commit_of.emplace(commit_key(item.slot, item.writes[v]), 0).second)
                    commits.push_back({item.slot, item.writes[v]});
        }
        Worker worker;
        LLVMContext ctx;
        Module mod("commits", ctx);
        auto* cont = code_hole(mod, kContName);
        for (size_t j = 0; j < commits.size(); ++j)
            BBTranslator::emit_slot_commit(mod, ("commit_" + std::to_string(j)).c_str(), commits[j].first,
                                           commits[j].second, cont);
        std::map<std::string, Code> code;
        worker.compile(mod, code);
        for (size_t j = 0; j < commits.size(); ++j) {
            auto it = code.find("commit_" + std::to_string(j));
            if (it == code.end() || !it->second.error.empty()) {
                fprintf(stderr, "stencilgen: a commit: %s\n", it == code.end() ? "no code" : it->second.error.c_str());
                return 1;
            }
            commit_of[commit_key(commits[j].first, commits[j].second)] = blob.add(it->second, 0, 0);
        }
    }
    std::vector<uint32_t> slots(8 * 0x10000 * 2, 0);  // SlotEntry kSlots[8][0x10000]
    size_t made_slot[2] = {}, legal_slot[2] = {};

    std::vector<uint32_t> index16(0x10000, 0), keys32;
    std::vector<uint32_t> keys_base(selected.size(), 0);
    std::map<std::string, unsigned> reasons;
    size_t made16 = 0, total16 = 0, made32 = 0, total32 = 0;
    uint32_t undef32 = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];
        if (i == undef_item) {
            if (!item.error.empty()) {
                fprintf(stderr, "stencilgen: undefined 32-bit instruction: %s\n", item.error.c_str());
                return 1;
            }
            undef32 = blob.add(item.code, item.flags, item.bytes);
            continue;
        }
        if (item.kind == Item::kSlotPiece) {
            if (item.error == "not legal in the slot") continue;
            ++legal_slot[item.slot - 1];
            if (!item.error.empty()) {
                reasons["slot " + std::to_string(item.slot) + ": " + item.error]++;
                continue;
            }
            ++made_slot[item.slot - 1];
            for (int v = 0; v < Item::kVariants; ++v) {
                if (!item.has[v]) continue;
                const Code& code = item.variant_code[item.same_as[v]];
                const bool nop = item.writes[v].empty() && tail_cont(code) && code.bytes.size() == 5 && code.cold.empty();
                const uint32_t st = blob.add(code, uint16_t(item.slot_flags[v] | (nop ? kNop : 0)), 8);
                const uint32_t cm = item.writes[v].empty() ? 0 : commit_of[commit_key(item.slot, item.writes[v])] + 1;
                const size_t at = (size_t(v * 2 + item.slot - 1) * 0x10000 + item.insn) * 2;
                slots[at] = st + 1;
                slots[at + 1] = cm;
            }
            continue;
        }
        const bool wide = i >= first32;
        if (wide && item.key == 0) keys_base[item.selected] = uint32_t(keys32.size());
        uint32_t entry = 0;
        if (item.error.empty()) entry = blob.add(item.code, item.flags, item.bytes) + 1;
        else reasons[(wide ? wanted[item.selected].name + std::string(item.kind == Item::kBodyPiece ? " slot 0: " : ": ")
                           : std::string("16-bit: ")) + item.error]++;
        if (wide) {
            keys32.push_back(entry);
            if (item.error != "an earlier pattern takes it" && item.error != "not a 32-bit encoding") {
                ++total32;
                made32 += entry != 0;
            }
        } else {
            index16[item.insn] = entry;
            ++total16;
            made16 += entry != 0;
        }
    }

    // cp_stencils.bin
    const std::string bin = out_dir + "/cp_stencils.bin";
    if (FILE* f = fopen(bin.c_str(), "wb")) {
        fwrite(blob.code.data(), 1, blob.code.size(), f);
        fclose(f);
    } else {
        perror(bin.c_str());
        return 1;
    }
    // cp_tables.bin: the big tables as the library lays them out (it is
    // built for the same target, by the same compiler), each at a multiple
    // of 16, which cp_stencils.cpp names with .incbin; text would take the
    // compiler a gigabyte.
    struct Table { const char* name; const void* data; size_t size; size_t at; };
    Table tables[] = {
        {"STENCILS", blob.stencils.data(), blob.stencils.size() * sizeof(Stencil), 0},
        {"RELOCS", blob.relocs.data(), blob.relocs.size() * sizeof(bcore_cp::Reloc), 0},
        {"INDEX16", index16.data(), index16.size() * sizeof(uint32_t), 0},
        {"KEYS32", keys32.data(), keys32.size() * sizeof(uint32_t), 0},
        {"SLOTS", slots.data(), slots.size() * sizeof(uint32_t), 0},
    };
    const std::string tables_bin = out_dir + "/cp_tables.bin";
    if (FILE* t = fopen(tables_bin.c_str(), "wb")) {
        size_t at = 0;
        for (Table& table : tables) {
            static const uint8_t zeros[16] = {};
            fwrite(zeros, 1, (16 - at % 16) % 16, t);
            at += (16 - at % 16) % 16;
            table.at = at;
            fwrite(table.data, 1, table.size, t);
            at += table.size;
        }
        fclose(t);
    } else {
        perror(tables_bin.c_str());
        return 1;
    }
    // cp_stencils.inc
    const std::string inc = out_dir + "/cp_stencils.inc";
    FILE* f = fopen(inc.c_str(), "w");
    if (!f) {
        perror(inc.c_str());
        return 1;
    }
    fprintf(f, "// Generated by bcore-stencilgen (tools/stencilgen.cpp). Do not edit.\n");
    for (const Table& table : tables)
        fprintf(f, "#define CP_TABLE_%s %zu, %zu\n", table.name, table.at, table.size);
    fprintf(f, "namespace bcore_cp {\n");
    fprintf(f, "const uint32_t kCodeSize = %zu;\n", blob.code.size());
    fprintf(f, "const Pattern32 kPatterns32[] = {\n");
    for (size_t i = 0; i < patterns.size(); ++i) {
        int sel = -1;
        for (size_t s = 0; s < selected.size(); ++s)
            if (selected[s].pattern == int(i)) sel = int(s);
        fprintf(f, "    {0x%08x, 0x%08x, %d},  // %s\n", patterns[i].mask, patterns[i].value, sel,
                patterns[i].name.c_str());
    }
    fprintf(f, "};\nconst uint32_t kPatterns32Count = %zu;\n", patterns.size());
    fprintf(f, "const Selected32 kSelected32[] = {\n");
    for (size_t s = 0; s < selected.size(); ++s) {
        const SelectedInfo& info = selected[s];
        fprintf(f, "    {0x%08x, %u, {", info.key_mask, keys_base[s]);
        for (auto& x : info.fields) fprintf(f, "{%u, %u, %u, %u}, ", x.lsb, x.bits, x.shift, x.flags);
        fprintf(f, "}, {");
        for (auto& x : info.regs) fprintf(f, "{%u, %u, %u, %u}, ", x.lsb, x.bits, x.shift, x.flags);
        fprintf(f, "}},  // %s\n", wanted[s].name);
    }
    fprintf(f, "};\n");
    fprintf(f, "const uint32_t kPrologue[2] = {%u, %u};  // host cycles off, on\n", prologue[0], prologue[1]);
    fprintf(f, "const uint32_t kExits[5] = {%u, %u, %u, %u, %u};  // return, chain, count, dry, dry count\n",
            exits[0], exits[1], exits[2], exits[3], exits[4]);
    fprintf(f, "const uint32_t kSelfExits[2] = {%u, %u};  // chain, count\n", self_exits[0], self_exits[1]);
    fprintf(f, "const uint32_t kFallthrough = %u;\n", fallthrough);
    fprintf(f, "const uint32_t kUndef32 = %u;\n", undef32);
    fprintf(f, "const uint32_t kParallelTail[4] = {%u, %u, %u, %u};  // [did_jump unknown * 2 + CEC check]\n",
            parallel_tail[0], parallel_tail[1], parallel_tail[2], parallel_tail[3]);
    fprintf(f, "}  // namespace bcore_cp\n");
    fclose(f);

    // cp_stencils.txt
    const std::string txt = out_dir + "/cp_stencils.txt";
    if (FILE* r = fopen(txt.c_str(), "w")) {
        fprintf(r, "16-bit: %zu of %zu encodings have stencils\n", made16, total16);
        fprintf(r, "32-bit: %zu of %zu keys have stencils\n", made32, total32);
        fprintf(r, "slots: %zu of %zu slot 1 words, %zu of %zu slot 2 words have stencils; %zu commits\n", made_slot[0],
                legal_slot[0], made_slot[1], legal_slot[1], commit_of.size());
        fprintf(r, "stencils: %zu, code %zu bytes, relocations %zu\n", blob.stencils.size(), blob.code.size(),
                blob.relocs.size());
        fprintf(r, "without a stencil:\n");
        for (auto& [why, n] : reasons) fprintf(r, "  %6u  %s\n", n, why.c_str());
        fclose(r);
    }
    printf("stencilgen: 16-bit %zu/%zu, 32-bit %zu/%zu, %zu stencils, %zu bytes\n", made16, total16, made32,
           total32, blob.stencils.size(), blob.code.size());
    return 0;
}
