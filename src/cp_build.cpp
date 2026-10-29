#include "cp_build.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <tuple>
#include <cctype>
#include <cstring>

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/Wasm.h>
#include <llvm/Support/raw_ostream.h>

#include "compiler.h"
#include "cp_wasm.h"
#include "helper_abi.h"
#include "lift_visitor.h"
#include "llvm_compat.h"

namespace bcore {

using namespace llvm;

namespace bcore_cp {

std::string ir_text(Function* fn) {
    std::string s;
    raw_string_ostream os(s);
    fn->print(os);
    os.flush();
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        out.push_back(s[i]);
        if (s[i] == '#' && i > 0 && s[i - 1] == ' ')
            while (i + 1 < s.size() && isdigit(static_cast<unsigned char>(s[i + 1]))) ++i;
    }
    return out;
}

// ---- Holes ----

const char* const kHoleNames[kHoleCont] = {
    "bcore_hole_pc", "bcore_hole_packet", "bcore_hole_field0", "bcore_hole_field1", "bcore_hole_field2",
    "bcore_hole_field3", "bcore_hole_counter", "bcore_hole_count_offset", "bcore_hole_threshold",
    "bcore_hole_block_pc", "bcore_hole_reg0", "bcore_hole_reg1", "bcore_hole_reg2", "bcore_hole_reg3",
};
const char* const kContName = "bcore_hole_cont";
const char* const kExitName = "bcore_hole_exit";
const char* const kSelfName = "bcore_hole_self";

Constant* value_hole(Module& mod, Hole hole, uint64_t lo, uint64_t hi, Type* ty) {
    auto& ctx = mod.getContext();
    auto* i8 = Type::getInt8Ty(ctx);
    auto* g = mod.getNamedGlobal(kHoleNames[hole]);
    if (!g) {
        // On WebAssembly (whose stencils are LLVM 23's) weak: two holes may
        // hold the same value, as two globals that are not weak never do
        // (LLVM 23 folds the PC hole's compare with a loop bottom a field
        // hole made to false, found by cp_stencil_test).
#if defined(__EMSCRIPTEN__)
        const auto linkage = GlobalValue::ExternalWeakLinkage;
#else
        const auto linkage = GlobalValue::ExternalLinkage;
#endif
        g = new GlobalVariable(mod, i8, true, linkage, nullptr, kHoleNames[hole]);
        g->setDSOLocal(true);
        auto* i64 = Type::getInt64Ty(ctx);
        g->setMetadata(LLVMContext::MD_absolute_symbol,
                       MDNode::get(ctx, {ConstantAsMetadata::get(ConstantInt::get(i64, lo)),
                                         ConstantAsMetadata::get(ConstantInt::get(i64, hi))}));
    }
    return ConstantExpr::getPtrToInt(g, ty);
}

Function* code_hole(Module& mod, const char* name) {
    if (auto* fn = mod.getFunction(name)) return fn;
    auto* fn = BBTranslator::declare_block(mod, name);
    fn->setDSOLocal(true);
    return fn;
}

BBTranslator::StencilHoles packet_holes(Module& mod) {
    auto* i32 = Type::getInt32Ty(mod.getContext());
    BBTranslator::StencilHoles holes;
    holes.pc = value_hole(mod, kHolePc, 0, 1ull << 32, i32);
    holes.packet = value_hole(mod, kHolePacket, 1, 257, i32);
    for (unsigned i = 0; i < LiftVisitor::kFieldHoles; ++i)
        holes.fields[i] = value_hole(mod, Hole(kHoleField0 + i), 0, 1ull << 32, i32);
    holes.cont = code_hole(mod, kContName);
    holes.exit = code_hole(mod, kExitName);
    return holes;
}

void make_local(Module& mod) {
    for (auto& fn : mod)
        if (fn.isDeclaration()) fn.setDSOLocal(true);
}

int hole_of(StringRef name) {
    for (int i = 0; i < int(kHoleCont); ++i)
        if (name == kHoleNames[i]) return i;
    if (name == kContName) return kHoleCont;
    if (name == kExitName) return kHoleExit;
    if (name == kSelfName) return kHoleSelf;
    for (unsigned i = 0; i < kHelperCount; ++i)
        if (name == kHelpers[i]) return int(kHoleHelper + i);
    return -1;
}

// ---- AArch64 value holes ----

namespace {

// A constant that is a value hole's address plus a constant: the hole and
// the constant.
struct HoleUse {
    GlobalVariable* hole = nullptr;
    int64_t addend = 0;
};

bool is_value_hole(const GlobalValue* g) {
    if (!g) return false;
    for (const char* name : kHoleNames)
        if (g->getName() == name) return true;
    return false;
}

// c as a hole + addend: the hole, a GEP of i8 off it, its ptrtoint, and
// adds or subtracts of constants (what the IR pipeline folds the lifter's
// PC arithmetic into).
bool as_hole_use(Constant* c, HoleUse& use) {
    if (auto* g = dyn_cast<GlobalVariable>(c)) {
        if (!is_value_hole(g)) return false;
        use = {g, 0};
        return true;
    }
    auto* ce = dyn_cast<ConstantExpr>(c);
    if (!ce) return false;
    switch (ce->getOpcode()) {
    case Instruction::PtrToInt:
    case Instruction::BitCast:
        return as_hole_use(ce->getOperand(0), use);
    case Instruction::GetElementPtr: {
        auto* gep = cast<GEPOperator>(ce);
        auto* index = gep->getNumIndices() == 1 ? dyn_cast<ConstantInt>(gep->getOperand(1)) : nullptr;
        if (!index || !gep->getSourceElementType()->isIntegerTy(8)) return false;
        if (!as_hole_use(ce->getOperand(0), use)) return false;
        use.addend += index->getSExtValue();
        return true;
    }
    case Instruction::Add:
    case Instruction::Sub: {
        auto* k = dyn_cast<ConstantInt>(ce->getOperand(1));
        if (k && as_hole_use(ce->getOperand(0), use)) {
            use.addend += ce->getOpcode() == Instruction::Add ? k->getSExtValue() : -k->getSExtValue();
            return true;
        }
        k = dyn_cast<ConstantInt>(ce->getOperand(0));
        if (k && ce->getOpcode() == Instruction::Add && as_hole_use(ce->getOperand(1), use)) {
            use.addend += k->getSExtValue();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool mentions_hole(const Constant* c) {
    if (auto* g = dyn_cast<GlobalVariable>(c)) return is_value_hole(g);
    for (const Use& op : c->operands())
        if (auto* k = dyn_cast<Constant>(op.get()); k && mentions_hole(k)) return true;
    return false;
}

// The values made so far in a function, by hole, addend and type: one that
// dominates a use serves it too (as the backend would keep a constant in a
// register: the loop bottoms' compares each take the PC).
struct Made {
    DominatorTree tree;
    std::map<std::tuple<GlobalVariable*, int64_t, Type*>, std::vector<Instruction*>> values;
    explicit Made(Function& fn) : tree(fn) {}
};

// The value hole + addend as an integer of `ty` (i32 or i64), made at `at`:
// MOVZ/MOVK of its 16-bit parts, as few as its range [lo, hi) + addend
// leaves set. An i32 takes the value modulo 2^32 (unchecked parts).
Value* movw(const HoleUse& use, IntegerType* ty, Instruction* at, Made& made) {
    auto& same = made.values[{use.hole, use.addend, ty}];
    for (Instruction* v : same)
        if (made.tree.dominates(v, at)) return v;
    // Without a range (or one past 2^62), any value: all of the type's parts.
    int64_t first = -1, last = INT64_MAX;
    if (MDNode* range = use.hole->getMetadata(LLVMContext::MD_absolute_symbol)) {
        const uint64_t lo = mdconst::extract<ConstantInt>(range->getOperand(0))->getZExtValue();
        const uint64_t hi = mdconst::extract<ConstantInt>(range->getOperand(1))->getZExtValue();
        if (hi > lo && hi <= (uint64_t(1) << 62)) {
            first = int64_t(lo) + use.addend;
            last = int64_t(hi) - 1 + use.addend;
        }
    }
    const bool w = ty->getBitWidth() == 32;
    char sym[96];
    if (use.addend) snprintf(sym, sizeof sym, "%s%+lld", use.hole->getName().str().c_str(), (long long)use.addend);
    else snprintf(sym, sizeof sym, "%s", use.hole->getName().str().c_str());
    const char* reg = w ? "${0:w}" : "$0";
    std::string text;
    auto part = [&](const char* op, const char* g) {
        if (!text.empty()) text += "\n";
        text += std::string(op) + " " + reg + ", #:" + g + ":" + sym;
    };
    if (first >= 0 && last < 0x10000) {
        part("movz", "abs_g0");
    } else if (w) {
        part("movz", "abs_g1_nc");
        part("movk", "abs_g0_nc");
    } else if (first >= 0 && last <= int64_t(UINT32_MAX)) {
        part("movz", "abs_g1");
        part("movk", "abs_g0_nc");
    } else {
        part("movz", "abs_g3");
        part("movk", "abs_g2_nc");
        part("movk", "abs_g1_nc");
        part("movk", "abs_g0_nc");
    }
    auto* asm_fn = InlineAsm::get(FunctionType::get(ty, false), text, "=r", /*hasSideEffects=*/false);
    auto* call = CallInst::Create(FunctionType::get(ty, false), asm_fn, {}, use.hole->getName() + ".value", at);
    call->setDoesNotAccessMemory();
    call->setDoesNotThrow();
    same.push_back(call);
    return call;
}

// c, which mentions a value hole, as instructions at `at`: the hole's value
// from movw, the rest of c as instructions on it.
Value* rebuild(Constant* c, Instruction* at, Made& made, std::string& error) {
    HoleUse use;
    if (as_hole_use(c, use)) {
        Type* ty = c->getType();
        if (ty->isPointerTy()) {
            Value* v = movw(use, Type::getInt64Ty(c->getContext()), at, made);
            return new IntToPtrInst(v, ty, "", at);
        }
        auto* ity = dyn_cast<IntegerType>(ty);
        if (!ity) {
            error = "a value hole of a type neither integer nor pointer";
            return nullptr;
        }
        if (ity->getBitWidth() == 32 || ity->getBitWidth() == 64) return movw(use, ity, at, made);
        Value* v = movw(use, Type::getInt32Ty(c->getContext()), at, made);
        return CastInst::CreateIntegerCast(v, ity, false, "", at);
    }
    auto* ce = dyn_cast<ConstantExpr>(c);
    if (!ce) {
        error = "a value hole in a constant aggregate";
        return nullptr;
    }
    Instruction* inst = as_instruction(ce, at);
    for (Use& op : inst->operands()) {
        auto* k = dyn_cast<Constant>(op.get());
        if (!k || !mentions_hole(k)) continue;
        Value* v = rebuild(k, inst, made, error);
        if (!v) return nullptr;
        op.set(v);
    }
    return inst;
}

}  // namespace

bool materialize_holes(Function& fn, std::string& error) {
    Made made(fn);
    std::vector<Instruction*> users;
    for (BasicBlock& bb : fn)
        for (Instruction& inst : bb)
            for (const Use& op : inst.operands())
                if (auto* k = dyn_cast<Constant>(op.get()); k && !isa<Function>(k) && mentions_hole(k)) {
                    users.push_back(&inst);
                    break;
                }
    for (Instruction* inst : users) {
        // A phi's operand on the edge it comes along; the same value for
        // each of its entries from one block (a switch's cases).
        std::map<std::pair<BasicBlock*, Constant*>, Value*> edges;
        for (Use& op : inst->operands()) {
            auto* k = dyn_cast<Constant>(op.get());
            if (!k || isa<Function>(k) || !mentions_hole(k)) continue;
            Instruction* at = inst;
            Value** edge = nullptr;
            if (auto* phi = dyn_cast<PHINode>(inst)) {
                BasicBlock* from = phi->getIncomingBlock(op);
                at = from->getTerminator();
                edge = &edges[{from, k}];
                if (*edge) {
                    op.set(*edge);
                    continue;
                }
            }
            Value* v = rebuild(k, at, made, error);
            if (!v) return false;
            if (edge) *edge = v;
            op.set(v);
        }
    }
    return true;
}

namespace {

// A function's code in an object: its section (hot) and the splitter's
// .text.split.<name> (cold), if any.
struct Part {
    std::string name;
    bool cold;
};

// A relocation's RelocKind, -1 for one the translator does not fill in.
int x86_kind(uint32_t type) {
    switch (type) {
    case ELF::R_X86_64_32: return kAbs32;
    case ELF::R_X86_64_32S: return kAbs32S;
    case ELF::R_X86_64_PC32:
    case ELF::R_X86_64_PLT32: return kPcRel32;
    case ELF::R_X86_64_64: return kAbs64;
    }
    return -1;
}

int a64_kind(uint32_t type) {
    switch (type) {
    case ELF::R_AARCH64_MOVW_UABS_G0: return kMovw0;
    case ELF::R_AARCH64_MOVW_UABS_G0_NC: return kMovw0Nc;
    case ELF::R_AARCH64_MOVW_UABS_G1: return kMovw1;
    case ELF::R_AARCH64_MOVW_UABS_G1_NC: return kMovw1Nc;
    case ELF::R_AARCH64_MOVW_UABS_G2_NC: return kMovw2Nc;
    case ELF::R_AARCH64_MOVW_UABS_G3: return kMovw3;
    case ELF::R_AARCH64_JUMP26:
    case ELF::R_AARCH64_CALL26: return kBranch26;
    case ELF::R_AARCH64_CONDBR19: return kCondBr19;
    case ELF::R_AARCH64_TSTBR14: return kTestBr14;
    }
    return -1;
}

}  // namespace

int32_t wasm_signature(const wasm::WasmSignature& sig) {
    std::vector<uint8_t> params, results;
    for (wasm::ValType t : sig.Params) params.push_back(uint8_t(t));
    for (wasm::ValType t : sig.Returns) results.push_back(uint8_t(t));
    return wasm_signature_id(params.data(), unsigned(params.size()), results.data(), unsigned(results.size()));
}

namespace {

// A WebAssembly object's functions (wasm_object.h's kind: llc's, unlinked):
// each one's body, its locals' declarations and its code, and the
// relocations in it, as Code.
std::map<std::string, Code> extract_wasm(const object::WasmObjectFile& wasm) {
    std::map<std::string, Code> out;
    const object::WasmSection* code_section = nullptr;
    for (const object::SectionRef& section : wasm.sections()) {
        const object::WasmSection& s = wasm.getWasmSection(section);
        if (s.Type == wasm::WASM_SEC_CODE) code_section = &s;
        // Data the code would reach (a switch's lookup table, a constant).
        else if (s.Type == wasm::WASM_SEC_DATA || s.Type == wasm::WASM_SEC_ELEM)
            for (const wasm::WasmFunction& fn : wasm.functions())
                out[fn.SymbolName.str()].error = "a data section";
    }
    if (!code_section) return out;
    const ArrayRef<uint8_t> payload = code_section->Content;
    std::vector<const object::WasmSymbol*> symbols;  // by index, as relocations name them
    for (const object::SymbolRef& sym : wasm.symbols()) symbols.push_back(&wasm.getWasmSymbol(sym));
    struct Range { uint64_t start, end; std::string name; };
    std::vector<Range> ranges;
    for (const wasm::WasmFunction& fn : wasm.functions()) {
        // The body: after its size, its locals' declarations and its code.
        uint64_t at = fn.CodeSectionOffset;
        uint64_t size = 0;
        unsigned shift = 0;
        uint8_t byte = 0;
        do {
            byte = payload[at++];
            size |= uint64_t(byte & 0x7f) << shift;
            shift += 7;
        } while (byte & 0x80);
        const std::string name = fn.SymbolName.str();
        Code& code = out[name];
        code.bytes.assign(payload.begin() + at, payload.begin() + at + size);
        ranges.push_back({at, at + size, name});
    }
    for (const wasm::WasmRelocation& r : code_section->Relocations) {
        const Range* range = nullptr;
        for (const Range& candidate : ranges)
            if (r.Offset >= candidate.start && r.Offset < candidate.end) range = &candidate;
        if (!range) continue;
        Code& code = out[range->name];
        std::string name;
        int hole = -1;
        int kind = -1;
        int64_t add = r.Addend;
        if (r.Type == wasm::R_WASM_TYPE_INDEX_LEB) {
            hole = int(kHoleWasmType);
            kind = kWasmIndex;
            add = wasm_signature(wasm.types()[r.Index]);
            name = "a type";
            if (add < 0) {
                if (code.error.empty()) code.error = "a function type of more than the stencils' value types";
                continue;
            }
        } else {
            name = symbols[r.Index]->Info.Name.str();
            hole = hole_of(name);
            if (name == "__stack_pointer") hole = int(kHoleWasmStack);
            if (name == "__indirect_function_table") hole = int(kHoleWasmTable);
            switch (r.Type) {
            case wasm::R_WASM_MEMORY_ADDR_SLEB: kind = kWasmSleb; break;
            case wasm::R_WASM_MEMORY_ADDR_LEB: kind = kWasmLeb; break;
            case wasm::R_WASM_FUNCTION_INDEX_LEB:
            case wasm::R_WASM_GLOBAL_INDEX_LEB:
            case wasm::R_WASM_TABLE_NUMBER_LEB: kind = kWasmIndex; break;
            }
        }
        const bool value = hole >= 0 && hole < int(kHoleCont);
        char why[160] = {};
        if (hole < 0)
            snprintf(why, sizeof why, "relocation against '%s'", name.c_str());
        else if (kind < 0)
            snprintf(why, sizeof why, "relocation type %u against %s", unsigned(r.Type), name.c_str());
        else if (value != (kind != kWasmIndex))
            snprintf(why, sizeof why, "relocation kind %d against %s", kind, name.c_str());
        else if (add < INT32_MIN || add > INT32_MAX)
            snprintf(why, sizeof why, "addend %lld", (long long)add);
        if (why[0]) {
            if (code.error.empty()) code.error = why;
            continue;
        }
        code.relocs.push_back({uint16_t(r.Offset - range->start), uint8_t(hole), uint8_t(kind), int32_t(add)});
    }
    for (auto& [name, code] : out) {
        if (code.bytes.size() > 0xffff && code.error.empty()) code.error = "too large";
        std::sort(code.relocs.begin(), code.relocs.end(),
                  [](const Reloc& a, const Reloc& b) { return a.offset < b.offset; });
    }
    return out;
}

}  // namespace

std::map<std::string, Code> extract(const MemoryBuffer& buffer) {
    std::map<std::string, Code> out;
    auto file = object::ObjectFile::createObjectFile(buffer.getMemBufferRef());
    if (!file) {
        consumeError(file.takeError());
        return out;
    }
    if (auto* wasm = dyn_cast<object::WasmObjectFile>(file->get())) return extract_wasm(*wasm);
    auto* elf = dyn_cast<object::ELFObjectFileBase>(file->get());
    if (!elf) return out;
    const bool a64 = elf->getEMachine() == ELF::EM_AARCH64;
    // Each function: its code's section and offset there. A section can hold
    // several (one .text without function sections), or one each.
    struct Range { uint64_t start, size; std::string name; uint64_t section; };
    std::vector<Range> ranges;
    for (const object::ELFSymbolRef sym : elf->symbols()) {
        auto type = sym.getType();
        if (!type || *type != object::SymbolRef::ST_Function) {
            if (!type) consumeError(type.takeError());
            continue;
        }
        auto name = sym.getName();
        auto addr = sym.getAddress();
        auto section = sym.getSection();
        if (!name || !addr || !section) {
            if (!name) consumeError(name.takeError());
            if (!addr) consumeError(addr.takeError());
            if (!section) consumeError(section.takeError());
            continue;
        }
        if (*section == elf->section_end()) continue;
        ranges.push_back({*addr, sym.getSize(), name->str(), (*section)->getIndex()});
    }
    auto section_name = [&](const object::SectionRef& section) -> std::string {
        auto name = section.getName();
        if (name) return name->str();
        consumeError(name.takeError());
        return {};
    };
    // Where an offset in a section lies: a function's code, hot or cold, and
    // the offset there.
    std::map<uint64_t, std::string> cold_of;  // section index -> function
    for (const object::SectionRef& section : elf->sections()) {
        const std::string name = section_name(section);
        static constexpr const char kSplit[] = ".text.split.";
        if (name.rfind(kSplit, 0) == 0) cold_of[section.getIndex()] = name.substr(sizeof kSplit - 1);
    }
    auto locate = [&](uint64_t section, uint64_t offset, Part& part, uint64_t& at) {
        if (auto it = cold_of.find(section); it != cold_of.end()) {
            part = {it->second, true};
            at = offset;
            return true;
        }
        for (auto& r : ranges)
            if (r.section == section && offset >= r.start && offset < r.start + r.size) {
                part = {r.name, false};
                at = offset - r.start;
                return true;
            }
        // The end of a function: where a branch may go (an emptied block).
        for (auto& r : ranges)
            if (r.section == section && offset == r.start + r.size) {
                part = {r.name, false};
                at = offset - r.start;
                return true;
            }
        return false;
    };
    for (const object::SectionRef& section : elf->sections()) {
        auto contents = section.getContents();
        if (!contents) {
            consumeError(contents.takeError());
            continue;
        }
        if (auto it = cold_of.find(section.getIndex()); it != cold_of.end()) {
            out[it->second].cold.assign(contents->bytes_begin(), contents->bytes_end());
            continue;
        }
        for (auto& r : ranges)
            if (r.section == section.getIndex())
                out[r.name].bytes.assign(contents->bytes_begin() + r.start, contents->bytes_begin() + r.start + r.size);
    }
    for (const object::SectionRef& rela : elf->sections()) {
        auto relocated = rela.getRelocatedSection();
        if (!relocated) {
            consumeError(relocated.takeError());
            continue;
        }
        if (*relocated == elf->section_end()) continue;
        const object::SectionRef target = **relocated;
        for (const object::RelocationRef& rel : rela.relocations()) {
            Part from;
            uint64_t offset = 0;
            if (!locate(target.getIndex(), rel.getOffset(), from, offset)) continue;
            Code& code = out[from.name];
            auto sym = rel.getSymbol();
            std::string name;
            int hole = -1;
            auto addend = object::ELFRelocationRef(rel).getAddend();
            int64_t add = addend ? *addend : 0;
            if (!addend) consumeError(addend.takeError());
            if (sym != elf->symbol_end()) {
                auto n = sym->getName();
                if (n) name = n->str();
                else consumeError(n.takeError());
                hole = hole_of(name);
                // A branch between the function's hot and cold code: to a
                // symbol or a section, the place in it in the addend.
                auto section = sym->getSection();
                auto addr = sym->getAddress();
                if (hole < 0 && section && addr && *section != elf->section_end()) {
                    Part to;
                    uint64_t at = 0;
                    if (locate((*section)->getIndex(), *addr, to, at) && to.name == from.name) {
                        hole = to.cold ? kHoleCold : kHoleHot;
                        add += int64_t(at);
                    }
                } else if (!section || !addr) {
                    if (!section) consumeError(section.takeError());
                    if (!addr) consumeError(addr.takeError());
                }
                if (name.empty()) name = "a section";
            }
            const int kind = a64 ? a64_kind(rel.getType()) : x86_kind(rel.getType());
            const bool code_hole = hole >= int(kHoleCont);
            const bool conditional = kind == kCondBr19 || kind == kTestBr14;
            const bool branch = kind == kPcRel32 || kind == kBranch26 || conditional;
            char why[160] = {};
            if (hole < 0)
                snprintf(why, sizeof why, "relocation against '%s'", name.c_str());
            else if (kind < 0)
                snprintf(why, sizeof why, "relocation type %u against %s", unsigned(rel.getType()), name.c_str());
            else if (code_hole != branch || (conditional && hole != kHoleHot && hole != kHoleCold))
                snprintf(why, sizeof why, "relocation kind %d against %s", kind, name.c_str());
            else if (hole == kHolePc && !a64 && kind != kAbs32)
                snprintf(why, sizeof why, "the PC as a sign-extended or 64-bit value");
            else if (add < INT32_MIN || add > INT32_MAX)
                snprintf(why, sizeof why, "addend %lld", (long long)add);
            if (why[0]) {
                if (code.error.empty()) code.error = why;
                continue;
            }
            (from.cold ? code.cold_relocs : code.relocs)
                .push_back({uint16_t(offset), uint8_t(hole), uint8_t(kind), int32_t(add)});
        }
    }
    auto by_offset = [](const bcore_cp::Reloc& a, const bcore_cp::Reloc& b) { return a.offset < b.offset; };
    for (auto& [name, code] : out) {
        if ((code.bytes.size() > 0xffff || code.cold.size() > 0xffff) && code.error.empty()) code.error = "too large";
        std::sort(code.relocs.begin(), code.relocs.end(), by_offset);
        std::sort(code.cold_relocs.begin(), code.cold_relocs.end(), by_offset);
    }
    return out;
}

// ---- WebAssembly pieces (cp_wasm.h) ----

namespace {

bool read_leb(const std::vector<uint8_t>& b, size_t& p, uint64_t& v) {
    v = 0;
    for (unsigned shift = 0; p < b.size() && shift < 64; shift += 7) {
        const uint8_t byte = b[p++];
        v |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

void write_padded(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    for (int i = 0; i < 5; ++i) {
        out[at + i] = uint8_t(v & 0x7f) | (i < 4 ? 0x80 : 0);
        v >>= 7;
    }
}

// Where the immediates of the instruction at op end, from p (after the
// opcode), for the instructions LLVM emits without SIMD; false for another.
bool skip_immediates(const std::vector<uint8_t>& b, size_t& p, uint8_t op) {
    uint64_t v = 0;
    switch (op) {
    case 0x00: case 0x01: case 0x05: case 0x0b: case 0x0f: case 0x1a: case 0x1b: return true;
    case 0x02: case 0x03: case 0x04:  // a block type: empty, a value type, or a type index
        if (p >= b.size()) return false;
        if (b[p] == 0x40 || (b[p] >= 0x7b && b[p] <= 0x7f)) {
            ++p;
            return true;
        }
        return read_leb(b, p, v);
    case 0x0c: case 0x0d: case 0x10: case 0x12:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
    case 0x41: case 0x42:
        return read_leb(b, p, v);
    case 0x0e: {
        uint64_t n = 0;
        if (!read_leb(b, p, n)) return false;
        for (uint64_t i = 0; i <= n; ++i)
            if (!read_leb(b, p, v)) return false;
        return true;
    }
    case 0x11: case 0x13: return read_leb(b, p, v) && read_leb(b, p, v);
    case 0x3f: case 0x40: return read_leb(b, p, v);
    case 0x43: p += 4; return p <= b.size();
    case 0x44: p += 8; return p <= b.size();
    case 0xfc: {
        uint64_t sub = 0;
        if (!read_leb(b, p, sub)) return false;
        if (sub <= 7) return true;                                       // trunc_sat
        if (sub == 8) return read_leb(b, p, v) && read_leb(b, p, v);     // memory.init
        if (sub == 9) return read_leb(b, p, v);                          // data.drop
        if (sub == 10) return read_leb(b, p, v) && read_leb(b, p, v);    // memory.copy
        if (sub == 11) return read_leb(b, p, v);                         // memory.fill
        return false;
    }
    }
    if (op >= 0x28 && op <= 0x3e) return read_leb(b, p, v) && read_leb(b, p, v);  // a memarg
    return op >= 0x45 && op <= 0xc4;  // numeric, no immediates
}

// wasm_piece (cp_build.h); exit_code: an exit's, which may return.
bool wasm_rewrite(Code& c, bool exit_code) {
    const std::vector<uint8_t>& b = c.bytes;
    auto fail = [&](const char* why) {
        if (c.error.empty()) c.error = why;
        return false;
    };
    // The locals: their types, by index from 2 (after cpu and mem).
    size_t p = 0;
    uint64_t groups = 0;
    if (!read_leb(b, p, groups)) return fail("a malformed body");
    std::vector<uint32_t> map = {0, 1};  // a local's index in the block's function
    uint32_t used[4] = {};
    for (uint64_t g = 0; g < groups; ++g) {
        uint64_t n = 0;
        if (!read_leb(b, p, n) || p >= b.size()) return fail("a malformed body");
        const uint8_t type = b[p++];
        int k = -1;
        for (int i = 0; i < 4; ++i)
            if (kWasmValueTypes[i] == type) k = i;
        if (k < 0) return fail("a local of a type stencils do not take");
        for (uint64_t i = 0; i < n; ++i) {
            uint32_t at = 2 + used[k]++;
            for (int j = 0; j < k; ++j) at += kWasmLocals[j];
            map.push_back(at);
        }
    }
    for (int k = 0; k < 4; ++k)
            if (used[k] > kWasmLocals[k]) {
                char why[96];
                snprintf(why, sizeof why, "too many locals (%u i32, %u i64, %u f32, %u f64)", used[0], used[1],
                         used[2], used[3]);
                return fail(why);
            }
    std::vector<uint8_t> out = {0x02, 0x40};  // the piece's own block
    std::vector<Reloc> relocs;
    size_t next_reloc = 0;
    // The relocation at offset at, if any.
    auto reloc_at = [&](size_t at) -> const Reloc* {
        while (next_reloc < c.relocs.size() && c.relocs[next_reloc].offset < at) ++next_reloc;
        return next_reloc < c.relocs.size() && c.relocs[next_reloc].offset == at ? &c.relocs[next_reloc] : nullptr;
    };
    uint32_t depth = 0;
    size_t prev = 0, prev2 = 0;  // where the last two instructions start in out
    while (p < b.size()) {
        const size_t start = p, out_start = out.size();
        const uint8_t op = b[p++];
        size_t imm = p;
        if (!skip_immediates(b, p, op)) return fail("an instruction stencils do not take");
        if (op == 0x0f && !exit_code) return fail("a return");
        if (op == 0x20 || op == 0x21 || op == 0x22) {
            uint64_t index = 0;
            read_leb(b, imm, index);
            if (index >= map.size()) return fail("a local out of range");
            out.push_back(op);
            wasm_leb(out, map[index]);
        } else if (op == 0x12 && reloc_at(imm) &&
                   (reloc_at(imm)->hole == kHoleCont || reloc_at(imm)->hole == kHoleExit)) {
            // The next packet: out of the piece's block; the block's exit:
            // out of the block around the pieces, after which the exit's
            // code follows (cp_wasm.h). Without the arguments (local.get 0,
            // local.get 1) the tail call took.
            const bool exit = reloc_at(imm)->hole == kHoleExit;
            static const uint8_t kArgs[] = {0x20, 0x00, 0x20, 0x01};
            const bool args = out_start >= 4 && prev2 == out_start - 4 && prev == out_start - 2 &&
                              std::equal(out.end() - 4, out.end(), kArgs);
            if (args) out.resize(out_start - 4);
            // At the piece's end it runs on into the next one.
            const bool last = !exit && depth == 0 && p + 1 == b.size() && b[p] == 0x0b;
            if (!last || !args) {
                out.push_back(0x0c);
                wasm_leb(out, depth + (exit ? 1 : 0));
            }
            prev = prev2 = SIZE_MAX;
            continue;
        } else {
            out.insert(out.end(), b.begin() + long(start), b.begin() + long(p));
            // Its relocations: the indices the header fixes made, the value
            // holes and the exit moved.
            for (size_t at = imm; at < p; ++at) {
                const Reloc* r = reloc_at(at);
                if (!r) continue;
                const size_t to = out_start + (at - start);
                if (r->kind != kWasmIndex) {
                    relocs.push_back({uint16_t(to), r->hole, r->kind, r->addend});
                    continue;
                }
                int64_t value = -1;
                if (r->hole >= kHoleHelper && r->hole < kHoleHelper + kHelperCount) value = r->hole - kHoleHelper;
                else if (r->hole == kHoleWasmTable) value = 0;
                else if (r->hole == kHoleWasmType) value = wasm_type_index(r->addend);
                else if (r->hole == kHoleSelf) value = kWasmBlockFunction;
                if (value < 0)
                    return fail(r->hole == kHoleWasmStack ? "the stack pointer"
                                : r->hole == kHoleExit    ? "the exit, other than in a tail call"
                                                          : "an index the header has not");
                write_padded(out, to, uint32_t(value));
            }
        }
        if (op == 0x02 || op == 0x03 || op == 0x04) ++depth;
        if (op == 0x0b) {
            if (depth == 0 && p != b.size()) return fail("code after the function's end");
            if (depth > 0) --depth;
        }
        if (out.size() != out_start) {
            prev2 = prev;
            prev = out_start;
        }
    }
    if (out.size() > 0xffff) return fail("too large");
    c.bytes = std::move(out);
    c.relocs = std::move(relocs);
    return true;
}

}  // namespace

bool wasm_piece(Code& c, bool exit) { return c.error.empty() && wasm_rewrite(c, exit); }

size_t tail_jump(const Code& c, Hole hole) {
    if (c.relocs.empty()) return 0;
    const Reloc& last = c.relocs.back();
    if (last.hole != hole || last.offset + 4u != c.bytes.size()) return 0;
    if (last.kind == kPcRel32 && c.bytes.size() >= 5 && c.bytes[c.bytes.size() - 5] == 0xe9 && last.addend == -4)
        return 5;  // jmp rel32
    if (last.kind == kBranch26 && last.addend == 0) {
        uint32_t insn = 0;
        memcpy(&insn, c.bytes.data() + last.offset, 4);
        if ((insn & 0xfc000000u) == 0x14000000u) return 4;  // B, not BL
    }
    return 0;
}

std::map<std::string, Code> compile_stencils(Compiler& compiler, Module& mod) {
    std::map<std::string, Code> out;
    mod.setDataLayout(compiler.dataLayout());
    set_target_triple(mod, compiler.targetTriple());
    const Triple triple(compiler.targetTriple());
    make_local(mod);
    bcore_abi::set_helper_cc(mod, bcore_abi::helper_cc(triple));  // lifted in this host's
    compiler.optimize(mod);
    // A function whose holes cannot be made loses its body (only its own
    // stencil is lost) and keeps its error.
    std::map<std::string, std::string> failed;
    if (triple.isAArch64()) {
        for (Function& fn : mod) {
            if (fn.isDeclaration()) continue;
            // A jump table would be data the code reaches PC-relatively.
            fn.addFnAttr("no-jump-tables", "true");
            std::string error;
            if (!materialize_holes(fn, error) || verifyFunction(fn, &errs())) {
                failed[fn.getName().str()] = error.empty() ? "the value holes' code does not verify" : error;
                fn.deleteBody();
            }
        }
    }
    if (auto object = compiler.emit(mod)) out = extract(*object);
    for (auto& [name, error] : failed) out[name].error = error;
    return out;
}

bool tail_cont(const Code& c) { return tail_jump(c, kHoleCont) != 0; }

bool tail_exit(const Code& c) { return tail_jump(c, kHoleExit) != 0; }

uint16_t code_flags(const Code& c) { return (tail_cont(c) ? kTailCont : 0) | (tail_exit(c) ? kTailExit : 0); }

}  // namespace bcore_cp

}  // namespace bcore
