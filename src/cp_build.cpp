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

// ---- Holes ----

const char* const kHoleNames[kHoleCont] = {
    "bcore_hole_pc", "bcore_hole_packet", "bcore_hole_field0", "bcore_hole_field1", "bcore_hole_field2",
    "bcore_hole_field3", "bcore_hole_counter", "bcore_hole_count_offset", "bcore_hole_threshold",
};
const char* const kContName = "bcore_hole_cont";
const char* const kExitName = "bcore_hole_exit";

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
    for (unsigned i = 0; i < kHelperCount; ++i)
        if (name == kHelpers[i]) return int(kHoleHelper + i);
    return -1;
}

std::map<std::string, Code> extract(const MemoryBuffer& buffer) {
    std::map<std::string, Code> out;
    auto file = object::ObjectFile::createObjectFile(buffer.getMemBufferRef());
    if (!file) {
        consumeError(file.takeError());
        return out;
    }
    auto* elf = dyn_cast<object::ELFObjectFileBase>(file->get());
    if (!elf) return out;
    struct Range { uint64_t start, size; std::string name; };
    std::vector<Range> ranges;
    std::optional<object::SectionRef> text;
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
        ranges.push_back({*addr, sym.getSize(), name->str()});
        text = **section;
    }
    if (!text) return out;
    auto contents = text->getContents();
    if (!contents) {
        consumeError(contents.takeError());
        return out;
    }
    for (auto& r : ranges) {
        Code& code = out[r.name];
        code.bytes.assign(contents->bytes_begin() + r.start, contents->bytes_begin() + r.start + r.size);
    }
    for (const object::SectionRef& section : elf->sections()) {
        auto relocated = section.getRelocatedSection();
        if (!relocated) {
            consumeError(relocated.takeError());
            continue;
        }
        if (*relocated == elf->section_end() || **relocated != *text) continue;
        for (const object::RelocationRef& rel : section.relocations()) {
            const uint64_t offset = rel.getOffset();
            auto it = std::find_if(ranges.begin(), ranges.end(),
                                   [&](const Range& r) { return offset >= r.start && offset < r.start + r.size; });
            if (it == ranges.end()) continue;
            Code& code = out[it->name];
            auto sym = rel.getSymbol();
            std::string name;
            if (sym != elf->symbol_end()) {
                auto n = sym->getName();
                if (n) name = n->str();
                else consumeError(n.takeError());
            }
            const int hole = hole_of(name);
            auto addend = object::ELFRelocationRef(rel).getAddend();
            const int64_t add = addend ? *addend : 0;
            if (!addend) consumeError(addend.takeError());
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
            code.relocs.push_back({uint16_t(offset - it->start), uint8_t(hole), uint8_t(kind), int32_t(add)});
        }
    }
    for (auto& [name, code] : out) {
        if (code.bytes.size() > 0xffff && code.error.empty()) code.error = "too large";
        std::sort(code.relocs.begin(), code.relocs.end(),
                  [](const bcore_cp::Reloc& a, const bcore_cp::Reloc& b) { return a.offset < b.offset; });
    }
    return out;
}

bool tail_cont(const Code& c) {
    if (c.bytes.size() < 5 || c.relocs.empty()) return false;
    const Reloc& last = c.relocs.back();
    return last.hole == kHoleCont && last.kind == kPcRel32 && last.offset + 4u == c.bytes.size() &&
           c.bytes[c.bytes.size() - 5] == 0xe9 && last.addend == -4;
}

}  // namespace bcore_cp
