#pragma once

// The stencils of the copy-and-patch tier 0, as tools/stencilgen.cpp writes
// them (cp_stencils.inc in the build tree) and cp_translator.cpp reads them.
//
// A stencil is a packet's machine code, compiled at build time from the
// lifter (BBTranslator::translate_stencil) with holes the translator fills
// in when it copies the code: each relocation of the compiled object
// becomes a Reloc, which says where in the code a value goes, which value
// (Hole) and how (RelocKind). A 16-bit instruction has a stencil per
// encoding, its PC the only hole the lifter leaves; a 32-bit pattern of
// cp_patterns.def has one per value of the fields that are not holes, and
// its immediates and PC-relative values are field holes, which the
// translator computes from the instruction with the pattern's Formula.

#include <cstdint>

namespace bcore_cp {

// What a relocation fills in.
enum Hole : uint8_t {
    kHolePc,          // the packet's PC
    kHolePacket,      // the packet's index in the block, from 1
    kHoleField0,      // kHoleField0 + i: field hole i (LiftVisitor::set_stencil)
    kHoleCounter = kHoleField0 + 4,  // the block's tier counter (prologue)
    kHoleCountOffset, // the counter's count's offset in the TierTable
    kHoleThreshold,   // the tier threshold
    kHoleBlockPc,     // the block's PC (its exit, kSelfExits)
    kHoleCont,        // the next packet's code
    kHoleExit,        // the block's exit (its own, kSelfExits, or the arena's exit stencil)
    kHoleSelf,        // the block's code (its exit, kSelfExits)
    kHoleHelper,      // kHoleHelper + i: the helper kHelpers[i]
};

// The helpers stencils call, in the order of kHoleHelper.
inline constexpr const char* kHelpers[] = {
    "mem_read8", "mem_read16", "mem_read32", "mem_write8", "mem_write16", "mem_write32",
    "cec_data_alignment", "cec_exception", "cec_raise", "cec_return_rti", "cec_return_rtx",
    "cec_return_rtn", "cec_return_rte", "cec_cli", "cec_sti", "cec_push_reti", "cec_pop_reti",
    "cec_check_sup", "cec_is_user_mode", "cec_check_pending", "bfin_hwloop_step", "bfin_putchar",
    "bfin_syscall", "memset", "memcpy", "memmove",
};
inline constexpr unsigned kHelperCount = sizeof(kHelpers) / sizeof(kHelpers[0]);

// How a relocation fills its value in: the x86-64 relocation it came from.
enum RelocKind : uint8_t {
    kAbs32,    // R_X86_64_32: value + addend, zero-extended from 32 bits
    kAbs32S,   // R_X86_64_32S: value + addend, sign-extended from 32 bits
    kPcRel32,  // R_X86_64_PC32/PLT32: value + addend - the place's address
    kAbs64,    // R_X86_64_64: value + addend
};

struct Reloc {
    uint16_t offset;  // in the stencil's code
    uint8_t hole;     // Hole
    uint8_t kind;     // RelocKind
    int32_t addend;
};

enum StencilFlags : uint16_t {
    kTerminates = 1,  // no path goes on to the next packet
    kCycles = 2,      // reads or writes CYCLES: the block's prologue sets them (host cycles)
    kIdle = 4,        // IDLE, compiled as idle_waits off has it (Core::set_idle_waits)
    kTailCont = 8,    // the code ends in a jmp rel32 to the next packet, which may be dropped
    // The code ends in a jmp rel32 to the block's exit, which may be dropped
    // where the exit follows (kSelfExits).
    kTailExit = 16,
};

struct Stencil {
    uint32_t code;         // offset in the code blob
    uint16_t size;
    uint16_t flags;        // StencilFlags
    uint32_t relocs;       // first Reloc
    uint16_t reloc_count;
    uint8_t bytes;         // the packet's length in bytes
    uint8_t reserved;
};

// A 32-bit field hole's value from the instruction: bits [lsb, lsb + bits),
// sign-extended if `sign`, shifted left by `shift`, plus the PC if `pc`.
struct Formula {
    uint8_t lsb, bits, shift;
    uint8_t flags;  // kFormula*
};
enum FormulaFlags : uint8_t { kFormulaUsed = 1, kFormulaSign = 2, kFormulaPc = 4 };

inline uint32_t field_hole_value(const Formula& f, uint32_t insn, uint32_t pc) {
    uint32_t v = (insn >> f.lsb) & ((f.bits >= 32 ? 0u : 1u << f.bits) - 1u);
    if ((f.flags & kFormulaSign) && f.bits < 32 && (v >> (f.bits - 1)) & 1) v |= ~0u << f.bits;
    v <<= f.shift;
    if (f.flags & kFormulaPc) v += pc;
    return v;
}

// A 32-bit pattern, in the decoder's order (decodeInstruction32 takes the
// first that matches); `selected` indexes kSelected32, or is -1.
struct Pattern32 {
    uint32_t mask, value;
    int16_t selected;
};

// A pattern with stencils: one per value of the key fields (key_mask's
// bits, packed from the lowest), stencil + 1 in kKeys32[keys + key], 0 for
// a key with none.
struct Selected32 {
    uint32_t key_mask;
    uint32_t keys;
    Formula fields[4];
};

// The tables (cp_stencils.cpp, from the generated cp_stencils.inc), where
// the copy-and-patch tier is built (BCORE_COPY_PATCH).
extern const uint8_t kCode[];
extern const uint32_t kCodeSize;
extern const Stencil kStencils[];
extern const Reloc kRelocs[];
extern const uint32_t kIndex16[0x10000];  // a 16-bit word's stencil + 1, 0 for none
extern const uint32_t kKeys32[];
extern const Pattern32 kPatterns32[];
extern const uint32_t kPatterns32Count;
extern const Selected32 kSelected32[];
extern const uint32_t kPrologue[2];  // without, with host cycles
extern const uint32_t kExits[5];     // return; chain; count chained; dry; dry, count chained
// A chaining block's own exit, at its end (CpTranslator::Config::self_loop):
// to its own start when its next PC is its own, through the chain table
// otherwise. Without, with chained blocks counted.
extern const uint32_t kSelfExits[2];
// Where a block cut at its last packet goes on (pc the next PC, field 0
// the packet's length, packet its index): stores them and exits.
extern const uint32_t kFallthrough;

}  // namespace bcore_cp
