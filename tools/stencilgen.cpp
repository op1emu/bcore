// bcore-stencilgen: the stencils of the copy-and-patch tier 0 (cp_format.h),
// built from the lifter at build time.
//
//   bcore-stencilgen OUT_DIR [--jobs N]
//
// writes OUT_DIR/cp_stencils.bin (the code), cp_stencils.inc (the tables
// cp_translator.cpp includes) and cp_stencils.txt (a report: counts, sizes
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
    uint32_t insn = 0;
    int len = 2;
    int selected = -1;     // index in kWanted for a 32-bit pattern
    uint32_t key = 0;      // its key fields, packed
    bool full = false;     // the pattern's first key: every value of its hole fields checked
    // Results
    std::string error;
    uint16_t flags = 0;
    uint8_t bytes = 0;
    Code code;
};

struct SelectedInfo {
    int pattern = -1;  // in patterns32()
    uint32_t key_mask = 0;
    Formula fields[4] = {};
};

// A worker: a context, a translator, a compiler.
class Worker {
public:
    Worker() : translator_(&memory_) {
        translator_.set_lazy_state(true);
        compiler_.init(2, 2, /*split=*/false, /*stencil=*/true);
    }

    // Lifts item at pc into mod as `name`; false with item.error set if the
    // lift is not one stencil.
    BBTranslator::StencilLift lift(Module& mod, const Item& item, uint32_t pc, const char* name) {
        memory_.set(pc, item.insn, item.len);
        auto holes = packet_holes(mod);
        return translator_.translate_stencil(mod, name, pc, holes);
    }

    // The checks of the file comment, then the stencil's IR into batch.
    void prepare(Module& batch, Item& item, const std::vector<Pattern>& patterns,
                 const std::vector<SelectedInfo>& selected, const char* name) {
        auto& ctx = batch.getContext();
        Module scratch("scratch", ctx);
        auto a = lift(scratch, item, kPcA, "s");
        const std::string text = ir_text(a.fn);
        a.fn->eraseFromParent();
        // An illegal 32-bit instruction raises its exception as a packet of 2.
        if (a.bytes != item.len && !(a.terminated && item.len == 4)) {
            item.error = "decodes to another length";
            return;
        }
        auto b = lift(scratch, item, kPcB, "s");
        const bool same = ir_text(b.fn) == text;
        b.fn->eraseFromParent();
        if (!same) {
            item.error = "IR depends on the PC";
            return;
        }
        if (item.selected >= 0) {
            const SelectedInfo& info = selected[item.selected];
            if (!check_fields(scratch, item, info, patterns, a, text)) return;
        } else if (a.fields_used) {
            item.error = "a 16-bit instruction with field holes";
            return;
        }
        auto lifted = lift(batch, item, kPcA, name);
        if (verifyFunction(*lifted.fn, &errs())) {
            item.error = "IR does not verify";
            lifted.fn->eraseFromParent();
            return;
        }
        item.bytes = uint8_t(lifted.bytes);
        item.flags = (lifted.terminated ? kTerminates : 0) | (lifted.cycles ? kCycles : 0) |
                     (item.len == 2 && item.insn == 0x0020 ? kIdle : 0);
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
            auto lifted = lift(scratch, other, kPcA, "s");
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
        stencils.push_back(s);
        return dedup[key] = uint32_t(stencils.size() - 1);
    }
};

void write_array_u32(FILE* f, const char* decl, const std::vector<uint32_t>& v) {
    fprintf(f, "%s = {", decl);
    for (size_t i = 0; i < v.size(); ++i) fprintf(f, "%s%u,", i % 16 ? "" : "\n    ", v[i]);
    fprintf(f, "\n};\n");
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
    std::vector<SelectedInfo> selected;
    std::vector<std::string> problems;
    for (const Wanted& w : kWanted) {
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
        // A value of the hole fields that is no special case.
        uint32_t hole_bits = 0;
        for (auto& f : info.fields)
            if (f.flags & kFormulaUsed) hole_bits |= (0x5a5a5a5au & ((f.bits >= 32 ? 0u : 1u << f.bits) - 1u)) << f.lsb;
        bool first = true;
        for (uint32_t key = 0; key < (1u << key_bits); ++key) {
            uint32_t spread = 0, v = key;
            for (uint32_t m = info.key_mask; m; m &= m - 1, v >>= 1)
                if (v & 1) spread |= m & -m;
            const uint32_t insn = p.value | spread | hole_bits;
            Item item;
            item.insn = insn;
            item.len = 4;
            item.selected = int(s);
            item.key = key;
            if (is16(uint16_t(insn >> 16)) || is_parallel(uint16_t(insn >> 16)))
                item.error = "not a 32-bit encoding";
            else if (first_match(patterns, insn) != info.pattern)
                item.error = "an earlier pattern takes it";
            item.full = first && item.error.empty();
            first = first && !item.full;
            items.push_back(item);
        }
    }

    // Lift, check and compile, in batches of 64 per module.
    constexpr size_t kBatch = 64;
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobs; ++t)
        threads.emplace_back([&] {
            Worker worker;
            for (;;) {
                const size_t start = next.fetch_add(kBatch);
                if (start >= items.size()) break;
                const size_t end = std::min(items.size(), start + kBatch);
                LLVMContext ctx;
                Module mod("stencils", ctx);
                std::vector<std::string> names(end - start);
                for (size_t i = start; i < end; ++i) {
                    if (!items[i].error.empty()) continue;
                    names[i - start] = "st_" + std::to_string(i);
                    worker.prepare(mod, items[i], patterns, selected, names[i - start].c_str());
                }
                std::map<std::string, Code> code;
                worker.compile(mod, code);
                for (size_t i = start; i < end; ++i) {
                    if (!items[i].error.empty()) continue;
                    auto it = code.find(names[i - start]);
                    if (it == code.end()) items[i].error = "no code";
                    else if (!it->second.error.empty()) items[i].error = it->second.error;
                    else items[i].code = std::move(it->second);
                }
            }
        });
    for (auto& t : threads) t.join();

    // The prologues and exits.
    Blob blob;
    std::vector<uint32_t> prologue(2), exits(5), self_exits(2);
    uint32_t fallthrough = 0;
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
    }
    if (!special_error.empty()) {
        fprintf(stderr, "stencilgen: %s", special_error.c_str());
        return 1;
    }

    std::vector<uint32_t> index16(0x10000, 0), keys32;
    std::vector<uint32_t> keys_base(selected.size(), 0);
    std::map<std::string, unsigned> reasons;
    size_t made16 = 0, total16 = 0, made32 = 0, total32 = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];
        const bool wide = i >= first32;
        if (wide && item.key == 0) keys_base[item.selected] = uint32_t(keys32.size());
        uint32_t entry = 0;
        if (item.error.empty()) entry = blob.add(item.code, item.flags, item.bytes) + 1;
        else reasons[(wide ? kWanted[item.selected].name + std::string(": ") : std::string("16-bit: ")) + item.error]++;
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
    // cp_stencils.inc
    const std::string inc = out_dir + "/cp_stencils.inc";
    FILE* f = fopen(inc.c_str(), "w");
    if (!f) {
        perror(inc.c_str());
        return 1;
    }
    fprintf(f, "// Generated by bcore-stencilgen (tools/stencilgen.cpp). Do not edit.\n");
    fprintf(f, "namespace bcore_cp {\n");
    fprintf(f, "const uint32_t kCodeSize = %zu;\n", blob.code.size());
    fprintf(f, "const Stencil kStencils[] = {\n");
    for (const Stencil& s : blob.stencils)
        fprintf(f, "    {%u, %u, %u, %u, %u, %u, 0},\n", s.code, s.size, s.flags, s.relocs, s.reloc_count, s.bytes);
    fprintf(f, "};\nconst Reloc kRelocs[] = {\n");
    for (const bcore_cp::Reloc& r : blob.relocs) fprintf(f, "    {%u, %u, %u, %d},\n", r.offset, r.hole, r.kind, r.addend);
    if (blob.relocs.empty()) fprintf(f, "    {0, 0, 0, 0},\n");
    fprintf(f, "};\n");
    write_array_u32(f, "const uint32_t kIndex16[0x10000]", index16);
    write_array_u32(f, "const uint32_t kKeys32[]", keys32);
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
        fprintf(f, "}},  // %s\n", kWanted[s].name);
    }
    fprintf(f, "};\n");
    fprintf(f, "const uint32_t kPrologue[2] = {%u, %u};  // host cycles off, on\n", prologue[0], prologue[1]);
    fprintf(f, "const uint32_t kExits[5] = {%u, %u, %u, %u, %u};  // return, chain, count, dry, dry count\n",
            exits[0], exits[1], exits[2], exits[3], exits[4]);
    fprintf(f, "const uint32_t kSelfExits[2] = {%u, %u};  // chain, count\n", self_exits[0], self_exits[1]);
    fprintf(f, "const uint32_t kFallthrough = %u;\n", fallthrough);
    fprintf(f, "}  // namespace bcore_cp\n");
    fclose(f);

    // cp_stencils.txt
    const std::string txt = out_dir + "/cp_stencils.txt";
    if (FILE* r = fopen(txt.c_str(), "w")) {
        fprintf(r, "16-bit: %zu of %zu encodings have stencils\n", made16, total16);
        fprintf(r, "32-bit: %zu of %zu keys have stencils\n", made32, total32);
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
