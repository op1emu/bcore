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
#include <llvm/Support/raw_ostream.h>

#include "compiler.h"
#include "helper_abi.h"
#include "lift_visitor.h"
#include "llvm_compat.h"

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
        g = new GlobalVariable(mod, i8, true, GlobalValue::ExternalLinkage, nullptr, kHoleNames[hole]);
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
    Instruction* inst = ce->getAsInstruction();  // then placed: LLVM 19 takes no position
    inst->insertBefore(at);
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

std::map<std::string, Code> extract(const MemoryBuffer& buffer) {
    std::map<std::string, Code> out;
    auto file = object::ObjectFile::createObjectFile(buffer.getMemBufferRef());
    if (!file) {
        consumeError(file.takeError());
        return out;
    }
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
    mod.setTargetTriple(compiler.targetTriple());
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
