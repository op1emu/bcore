#pragma once

// Building a copy-and-patch stencil (cp_format.h) from the lifter: what
// tools/stencilgen.cpp does at build time and cp_runtime.h at run time for
// the packets the build left without one. A stencil is the packet
// BBTranslator::translate_stencil lifts with the holes made here, compiled
// by a Compiler in stencil mode; extract() takes each function's code and
// relocations from the object and says why one cannot be a stencil.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/MemoryBuffer.h>

#include "bb_translator.h"
#include "cp_format.h"
#include "mem.h"

namespace llvm {
class Constant;
class Function;
class Module;
class Type;
}  // namespace llvm

namespace bcore_cp {

// The instruction being lifted, and a page table that is there (the lifter
// inlines RAM accesses through it, as the translator requires,
// cp_translator.h); nothing else is ever read.
class StencilMemory : public Memory {
public:
    // A 16-bit (len 2) or 32-bit (len 4) instruction at pc.
    void set(uint32_t pc, uint32_t insn, int len) {
        if (len == 2) {
            const uint16_t w = uint16_t(insn);
            set_words(pc, &w, 1);
        } else {
            const uint16_t w[2] = {uint16_t(insn >> 16), uint16_t(insn)};
            set_words(pc, w, 2);
        }
    }
    // A packet of `count` halfwords (up to 4) at pc.
    void set_words(uint32_t pc, const uint16_t* words, int count) {
        pc_ = pc;
        count_ = count;
        for (int i = 0; i < 4; ++i) words_[i] = i < count ? words[i] : 0;
        outside_ = false;
    }
    // Whether the lifter read any halfword but the packet's since set:
    // then the stencil would depend on what follows the packet.
    bool read_outside() const { return outside_; }
    uint32_t base() const override { return 0; }
    uint32_t size() const override { return 0; }
    uint8_t read8(uint32_t) const override { return 0; }
    uint16_t read16(uint32_t addr) const override {
        const uint32_t i = (addr - pc_) / 2;
        if (addr >= pc_ && !((addr - pc_) & 1) && i < uint32_t(count_)) return words_[i];
        outside_ = true;
        return 0;
    }
    uint32_t read32(uint32_t) const override { return 0; }
    void write8(uint32_t, uint8_t) override {}
    void write16(uint32_t, uint16_t) override {}
    void write32(uint32_t, uint32_t) override {}
    const uint8_t* raw() const override { return nullptr; }
    uintptr_t fast_base() const override { return 0; }
    uint32_t rawmem_limit() const override { return 0; }
    uint8_t* const* page_table() const override { return &dummy_; }

private:
    uint32_t pc_ = 0;
    uint16_t words_[4] = {};
    int count_ = 0;
    mutable bool outside_ = false;
    uint8_t* dummy_ = nullptr;
};

// The PC a stencil is lifted at, and another to lift it at again: the two
// lifts must be the same IR (the PC holes aside), or the stencil depends on
// its PC.
inline constexpr uint32_t kPcA = 0x00001000;
inline constexpr uint32_t kPcB = 0xffa01234;
// A function's IR, but for its attribute groups' numbers (a module numbers
// them as functions come, and the checks lift one function after another).
std::string ir_text(llvm::Function* fn);

// ---- Holes ----

extern const char* const kHoleNames[kHoleCont];
extern const char* const kContName;
extern const char* const kExitName;

// A value hole: a global whose address is the value, in [lo, hi) (so that
// the backend can take it for an immediate of that width), as a constant
// of type ty.
llvm::Constant* value_hole(llvm::Module& mod, Hole hole, uint64_t lo, uint64_t hi, llvm::Type* ty);
// A code hole: a block function the code jumps to (the next packet, the exit).
llvm::Function* code_hole(llvm::Module& mod, const char* name);
// A packet's holes (BBTranslator::translate_stencil).
BBTranslator::StencilHoles packet_holes(llvm::Module& mod);
// Helpers keep their names; the dso_local declarations make calls direct
// (rel32), which the translator points at its trampolines.
void make_local(llvm::Module& mod);

// ---- Compiled code ----

struct Code {
    std::vector<uint8_t> bytes;
    std::vector<Reloc> relocs;
    std::string error;  // why the code cannot be a stencil
};

// The hole a relocation's symbol names, or -1.
int hole_of(llvm::StringRef name);
// The code of each function of an object (one .text section), by name.
std::map<std::string, Code> extract(const llvm::MemoryBuffer& buffer);
// The code ends in a jmp rel32 to the next packet, which the translator
// drops where the next packet follows (kTailCont).
bool tail_cont(const Code& code);

}  // namespace bcore_cp
