#include "cp_build.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <cctype>

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/FunctionComparator.h>

#include "lift_visitor.h"

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

bool same_ir(const Function& a, const Function& b) {
    // Globals compare by their number in the state, the same for the same
    // global: so only within one module.
    GlobalNumberState numbers;
    return a.getParent() == b.getParent() && FunctionComparator(&a, &b, &numbers).compare() == 0;
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

namespace {

// A function's code in an object: its section (hot) and the splitter's
// .text.split.<name> (cold), if any.
struct Part {
    std::string name;
    bool cold;
};

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
            int kind = -1;
            switch (rel.getType()) {
            case ELF::R_X86_64_32: kind = kAbs32; break;
            case ELF::R_X86_64_32S: kind = kAbs32S; break;
            case ELF::R_X86_64_PC32:
            case ELF::R_X86_64_PLT32: kind = kPcRel32; break;
            case ELF::R_X86_64_64: kind = kAbs64; break;
            }
            const bool code_hole = hole >= int(kHoleCont);
            char why[160] = {};
            if (hole < 0)
                snprintf(why, sizeof why, "relocation against '%s'", name.c_str());
            else if (kind < 0)
                snprintf(why, sizeof why, "relocation type %u against %s", unsigned(rel.getType()), name.c_str());
            else if (code_hole != (kind == kPcRel32))
                snprintf(why, sizeof why, "relocation kind %d against %s", kind, name.c_str());
            else if (hole == kHolePc && kind != kAbs32)
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

bool tail_cont(const Code& c) {
    if (c.bytes.size() < 5 || c.relocs.empty()) return false;
    const Reloc& last = c.relocs.back();
    return last.hole == kHoleCont && last.kind == kPcRel32 && last.offset + 4u == c.bytes.size() &&
           c.bytes[c.bytes.size() - 5] == 0xe9 && last.addend == -4;
}

bool tail_exit(const Code& c) {
    if (c.bytes.size() < 5 || c.relocs.empty()) return false;
    const Reloc& last = c.relocs.back();
    return last.hole == kHoleExit && last.kind == kPcRel32 && last.offset + 4u == c.bytes.size() &&
           c.bytes[c.bytes.size() - 5] == 0xe9 && last.addend == -4;
}

uint16_t code_flags(const Code& c) { return (tail_cont(c) ? kTailCont : 0) | (tail_exit(c) ? kTailExit : 0); }

}  // namespace bcore_cp
