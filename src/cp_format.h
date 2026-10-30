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

namespace bcore {

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
    kHoleReg0,        // kHoleReg0 + i: register hole i, a data register's offset in CpuState
    kHoleCont = kHoleReg0 + 4,  // the next packet's code
    kHoleExit,        // the block's exit (its own, kSelfExits, or the arena's exit stencil)
    kHoleSelf,        // the block's code (its exit, kSelfExits)
    kHoleHot,         // the stencil's own hot code, from its cold code (Stencil::cold)
    kHoleCold,        // the stencil's own cold code, from its hot code
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

// WebAssembly's other indices a stencil's code names, after the helpers:
// the blocks' stack pointer (a global), the function table, and a function
// type (for call_indirect; its id in the addend, cp_wasm.h's
// wasm_signature_id).
inline constexpr unsigned kHoleWasmStack = kHoleHelper + kHelperCount;
inline constexpr unsigned kHoleWasmTable = kHoleWasmStack + 1;
inline constexpr unsigned kHoleWasmType = kHoleWasmStack + 2;

// Value holes after those (numbered so that the ones before keep theirs),
// the block's loop hint (CpTranslator::Config::hint) for its entry guard
// and its pinned loop bottoms: the hint's LB0, LB1, LT0 and LT1, and the
// block's span (its last packet's PC less its first's).
inline constexpr unsigned kHoleLb0 = kHoleWasmType + 1;
inline constexpr unsigned kHoleLb1 = kHoleLb0 + 1;
inline constexpr unsigned kHoleLt0 = kHoleLb0 + 2;
inline constexpr unsigned kHoleLt1 = kHoleLb0 + 3;
inline constexpr unsigned kHoleSpan = kHoleLb0 + 4;
inline constexpr unsigned kHoleEnd = kHoleSpan + 1;

// Whether a hole is a value the translator fills in (else code: a jump or
// a call, or on WebAssembly an index).
inline constexpr bool is_value_hole(unsigned hole) {
    return hole < kHoleCont || (hole >= kHoleLb0 && hole < kHoleEnd);
}

// How a relocation fills its value in: the relocation it came from.
enum RelocKind : uint8_t {
    // x86-64
    kAbs32,    // R_X86_64_32: value + addend, zero-extended from 32 bits
    kAbs32S,   // R_X86_64_32S: value + addend, sign-extended from 32 bits
    kPcRel32,  // R_X86_64_PC32/PLT32: value + addend - the place's address
    kAbs64,    // R_X86_64_64: value + addend
    // AArch64: a MOVZ's or MOVK's 16-bit immediate, bits [16 k, 16 k + 16)
    // of value + addend (R_AARCH64_MOVW_UABS_Gk), checked that none above
    // them is set, or not (_NC)
    kMovw0,
    kMovw0Nc,
    kMovw1,
    kMovw1Nc,
    kMovw2Nc,
    kMovw3,
    kBranch26,  // R_AARCH64_JUMP26/CALL26, a B or BL: (value + addend - place) / 4
    // A conditional branch between a stencil's hot and cold code, through a
    // B the translator places beside it (cp_translator.cpp's trampolines):
    kCondBr19,  // R_AARCH64_CONDBR19, a B.cond, CBZ or CBNZ: the same in 19 bits (+-1 MiB)
    kTestBr14,  // R_AARCH64_TSTBR14, a TBZ or TBNZ: in 14 bits (+-32 KiB)
    // WebAssembly: a LEB128 padded to 5 bytes, whatever its value
    kWasmSleb,  // R_WASM_MEMORY_ADDR_SLEB, an i32.const: value + addend, signed
    kWasmLeb,   // R_WASM_MEMORY_ADDR_LEB, a load's or store's offset: value + addend, unsigned
    kWasmIndex, // R_WASM_FUNCTION_INDEX_LEB, TYPE_INDEX_LEB, GLOBAL_INDEX_LEB, TABLE_NUMBER_LEB: an index
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
    kTailCont = 8,    // the code ends in a jump to the next packet (jmp rel32, B), which may be dropped
    // The code ends in a jump to the block's exit, which may be dropped
    // where the exit follows (kSelfExits).
    kTailExit = 16,
    // A parallel packet's pieces (kSlots, kKeys32 with the multi-issue bit):
    kBody = 32,         // slot 0's instruction alone (BBTranslator::translate_stencil_body)
    kSetsDisAlgn = 64,  // a slot 0 that leaves DISALGNEXCPT on: the slots' dis_algn entries
    kSlotStore = 128,   // a slot that writes memory: the packet's tail checks CEC
    kNop = 256,         // a slot that does nothing: the translator leaves it out
    // Writes LB or LT, or calls a helper that may (the memory callbacks
    // taken to keep the registers, as a parallel packet's must): the
    // block's loop hint holds no longer after it.
    kLoopsMoved = 512,
};

// Its code is in two parts: the hot code, which the translator lays out
// one stencil after the other, and the cold code (the machine function
// splitter's, the paths behind rare branches), which it puts in the code
// arena's cold region; each part branches to the other through kHoleHot and
// kHoleCold, the place in the other part in the addend.
struct Stencil {
    uint32_t code;         // offset in the code blob
    uint16_t size;
    uint16_t flags;        // StencilFlags
    uint32_t relocs;       // first Reloc
    uint16_t reloc_count;
    uint8_t bytes;         // the packet's length in bytes
    uint8_t reserved;
    uint32_t cold;         // the cold code: offset in the blob, size (0 for none), relocations
    uint16_t cold_size;
    uint16_t cold_reloc_count;
    uint32_t cold_relocs;
};

inline constexpr unsigned kRegHoles = 4;

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
// a key with none. Its register holes (regs) are data register fields: the
// register's offset in CpuState, its number << 2 (dpregs is at 0).
struct Selected32 {
    uint32_t key_mask;
    uint32_t keys;
    Formula fields[4];
    Formula regs[kRegHoles];
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
// The same where the block runs again past its entry guard (kGuards): a
// guarded block in which no packet moves a loop (kLoopsMoved) finds the
// loop registers as its guard did. On WebAssembly the way back is a branch
// to a loop around the block's pieces, after the guard (cp_emit_wasm.cpp);
// natively these are kSelfExits, their kHoleSelf the block's prologue.
extern const uint32_t kSelfExitsLoop[2];
// Where a block cut at its last packet goes on (pc the next PC, field 0
// the packet's length, packet its index): stores them and exits.
extern const uint32_t kFallthrough;
// A 32-bit instruction no pattern takes (the undefined instruction's
// exception, a packet of 2 that ends the block).
extern const uint32_t kUndef32;
// A parallel packet's slots 1 and 2: for each 16-bit word, its stencil and
// its commit's (+ 1, 0 for none: no stencil, an instruction the slot does
// not take; no commit, no register written), by [dis_algn * 2 + slot - 1].
struct SlotEntry {
    uint32_t stencil;
    uint32_t commit;
};
extern const SlotEntry kSlots[4][0x10000];
// A parallel packet's tail (its epilog), by [CEC check]; and where a loop
// ends at the packet when the block is translated (its step inline,
// BBTranslator::translate_parallel_tail's bottom), by [CEC check].
extern const uint32_t kParallelTail[2];
extern const uint32_t kParallelTailBottom[2];
// Under the block's entry guard (kGuards), its hint holding: where no loop
// ends at the packet, the tail without the compares; where loop 0, loop 1
// or both end there and run (pinned, LiftVisitor::pin_mask), the step to
// the hint's tops (kHoleLt0, kHoleLt1) with only the counts read. By [CEC
// check], and [pinned - 1][CEC check].
extern const uint32_t kParallelTailAbsent[2];
extern const uint32_t kParallelTailPinned[3][2];
// A block's entry guard, before its prologue, by the loops its tails pin
// (bit i: loop i): each LB the hint's (kHoleLb0, kHoleLb1) or outside the
// block (kHoleBlockPc, kHoleSpan), and for a pinned loop LB and LT the
// hint's. Otherwise it sets CpuState::lb_guard_miss and returns before the
// block's first packet, and Core::run compiles the block careful.
extern const uint32_t kGuards[4];

}  // namespace bcore_cp

}  // namespace bcore
