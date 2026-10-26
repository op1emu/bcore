// AArch64 stencils on any host (LLVM's AArch64 target compiles them here,
// nothing runs them): what bcore-stencilgen makes of a packet for AArch64
// (cp_build.h's compile_stencils: the value holes as MOVZ/MOVK immediates,
// the next packet, the exit and the helpers as B/BL) and how the translator
// fills those in (cp_patch.h). Its blocks running is the copy-and-patch
// tests' (cp_stencil_test, tier_test) on an AArch64 host.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include "bb_translator.h"
#include "block_abi.h"
#include "compiler.h"
#include "cp_build.h"
#include "cp_format.h"
#include "cp_patch.h"

using namespace llvm;
using namespace bcore_cp;

extern "C" {
void LLVMInitializeAArch64TargetInfo();
void LLVMInitializeAArch64Target();
void LLVMInitializeAArch64TargetMC();
void LLVMInitializeAArch64AsmPrinter();
void LLVMInitializeAArch64AsmParser();
}

static void require(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

static uint32_t insn_at(const Code& c, uint16_t offset) {
    uint32_t v;
    memcpy(&v, c.bytes.data() + offset, 4);
    return v;
}

static bool is_movw(uint8_t kind) { return kind >= kMovw0 && kind <= kMovw3; }

// A 16-bit instruction's stencil, compiled for AArch64; `after` sees the
// module as compiled.
static Code stencil16(Compiler& compiler, uint16_t word, const std::function<void(Module&)>& after = {}) {
    LLVMContext ctx;
    Module mod("a64", ctx);
    StencilMemory memory;
    BBTranslator translator(&memory);
    translator.set_lazy_state(true);
    memory.set(kPcA, word, 2);
    auto lifted = translator.translate_stencil(mod, "s", kPcA, packet_holes(mod));
    require(lifted.fn != nullptr, "the lift");
    auto out = compile_stencils(compiler, mod);
    require(out.count("s") == 1, "the stencil's code");
    if (after) after(mod);
    return out["s"];
}

// A narrow store's value is zeroext on the helper's declaration and call, in
// the convention AArch64 stencils call it in: Apple's ABI leaves the
// extension to the caller (a stencil's narrow load extends it today, so no
// run would show its loss).
static void check_narrow_store(Compiler& compiler, uint16_t word, const char* helper) {
    bool called = false;
    stencil16(compiler, word, [&](Module& mod) {
        Function* fn = mod.getFunction(helper);
        require(fn && fn->getCallingConv() == CallingConv::PreserveMost, "the helper in preserve_most");
        require(fn->hasParamAttribute(2, Attribute::ZExt), "the declaration's value zeroext");
        for (User* user : fn->users())
            if (auto* call = dyn_cast<CallInst>(user)) {
                require(call->getCallingConv() == CallingConv::PreserveMost, "the call in preserve_most");
                require(call->getAttributes().hasParamAttr(2, Attribute::ZExt), "the call's value zeroext");
                called = true;
            }
    });
    require(called, "a call of the helper");
}

// Every relocation one the translator fills in on AArch64; each 32-bit PC
// hole two halves, each 16-bit packet hole one.
static void check_relocs(const Code& c, const char* what) {
    char msg[160];
    snprintf(msg, sizeof msg, "%s: %s", what, c.error.c_str());
    require(c.error.empty(), msg);
    require(c.bytes.size() % 4 == 0, "whole instructions");
    unsigned pc_hi = 0, pc_lo = 0, packets = 0;
    for (const bcore_cp::Reloc& r : c.relocs) {
        const bool code_hole = r.hole >= kHoleCont;
        require(code_hole ? r.kind == kBranch26 : is_movw(r.kind), "a code hole as a branch, a value hole as MOVW");
        const uint32_t insn = insn_at(c, r.offset);
        if (r.kind == kBranch26) {
            require((insn & 0x7c000000u) == 0x14000000u, "a B or BL");
        } else {
            // MOVZ (opc 10) or MOVK (opc 11), 32- or 64-bit, hw = the part.
            require((insn & 0x1f800000u) == 0x12800000u && (insn >> 29 & 3) >= 2, "a MOVZ or MOVK");
            static constexpr unsigned kPart[] = {0, 0, 1, 1, 2, 3};
            require((insn >> 21 & 3) == kPart[r.kind - kMovw0], "the part its shift names");
        }
        if (r.hole == kHolePc && r.kind == kMovw1Nc) ++pc_hi;
        if (r.hole == kHolePc && r.kind == kMovw0Nc) ++pc_lo;
        if (r.hole == kHolePacket) {
            require(r.kind == kMovw0, "the packet's index in one MOVZ, checked");
            ++packets;
        }
    }
    require(pc_hi == pc_lo, "each PC in two halves");
    printf("%s: %zu bytes, %zu relocations, %u PCs, %u packet indexes\n", what, c.bytes.size(), c.relocs.size(),
           pc_hi, packets);
}

static void check_patch(const Code& c, uint32_t pc) {
    std::vector<uint8_t> code = c.bytes;
    for (const bcore_cp::Reloc& r : c.relocs) {
        if (r.hole == kHolePc) require(patch_reloc(code.data(), r, pc), "a PC fits");
        if (r.hole == kHolePacket) {
            require(!patch_reloc(code.data(), r, 0x10000), "a packet index over 16 bits is refused");
            require(patch_reloc(code.data(), r, 256), "packet 256 fits");
        }
    }
    // The halves at each MOVZ/MOVK pair give the value (+ addend) back; the
    // rest of each instruction as it was.
    for (const bcore_cp::Reloc& r : c.relocs) {
        if (!is_movw(r.kind)) continue;
        const uint32_t was = insn_at(c, r.offset), now = insn_at(Code{code}, r.offset);
        require((was & ~(0xffffu << 5)) == (now & ~(0xffffu << 5)), "only the immediate changes");
        const uint32_t imm = now >> 5 & 0xffff;
        if (r.hole == kHolePc) {
            const uint32_t v = pc + uint32_t(r.addend);
            require(imm == (r.kind == kMovw1Nc ? v >> 16 : v & 0xffff), "the PC's half");
        }
        if (r.hole == kHolePacket) require(imm == 256, "the packet's index");
    }
}

static void check_branch() {
    alignas(4) uint8_t code[8] = {};
    store32(code, 0x14000000);      // B
    store32(code + 4, 0x94000000);  // BL
    const bcore_cp::Reloc b{0, kHoleCont, kBranch26, 0}, bl{4, kHoleHelper, kBranch26, 0};
    const uintptr_t at = reinterpret_cast<uintptr_t>(code);
    require(patch_reloc(code, b, at + 0x1234 * 4), "a branch forward");
    require(load32(code) == (0x14000000u | 0x1234), "B's offset");
    require(patch_reloc(code, bl, at + 4 - 4), "a branch back");
    require(load32(code + 4) == 0x97ffffffu, "BL's offset, back one instruction");
    require(patch_reloc(code, b, at + (uintptr_t(1) << 27) - 4), "the furthest branch forward");
    require(!patch_reloc(code, b, at + (uintptr_t(1) << 27)), "128 MiB away is out of reach");
    require(patch_reloc(code, b, at - (uintptr_t(1) << 27)), "the furthest branch back");
    require(!patch_reloc(code, b, at - (uintptr_t(1) << 27) - 4), "further back is out of reach");
    require(!patch_reloc(code, b, at + 2), "a target not on an instruction");
}

// How wide a value hole's MOVZ/MOVK is: its range's and its type's.
static void check_widths(Compiler& compiler) {
    LLVMContext ctx;
    Module mod("widths", ctx);
    auto* i32 = Type::getInt32Ty(ctx);
    auto* i64 = Type::getInt64Ty(ctx);
    auto* fn = bcore_abi::make_block(mod, "w");
    IRBuilder<> b(BasicBlock::Create(ctx, "entry", fn));
    Value* cpu = bcore_abi::block_cpu(fn);
    auto store = [&](Constant* v, uint64_t offset) {
        b.CreateStore(v, b.CreateGEP(b.getInt8Ty(), cpu, b.getInt64(offset)), /*isVolatile=*/true);
    };
    store(value_hole(mod, kHolePacket, 1, 257, i32), 0);                     // MOVZ, checked
    store(value_hole(mod, kHolePc, 0, uint64_t(1) << 32, i32), 4);           // MOVZ + MOVK, unchecked
    store(value_hole(mod, kHoleCountOffset, 0, uint64_t(1) << 31, i64), 8);  // MOVZ (checked) + MOVK
    store(value_hole(mod, kHoleThreshold, 0, uint64_t(1) << 40, i64), 16);   // all four parts
    // An addend that takes a range past 2^16: two parts.
    store(ConstantExpr::getAdd(value_hole(mod, kHoleField0, 0xfff0, 0xfff8, i32), ConstantInt::get(i32, 0x20)), 24);
    b.CreateRetVoid();
    auto out = compile_stencils(compiler, mod);
    const Code& c = out["w"];
    require(c.error.empty(), c.error.c_str());
    std::map<uint8_t, std::vector<uint8_t>> kinds;
    for (const bcore_cp::Reloc& r : c.relocs) kinds[r.hole].push_back(r.kind);
    auto sorted = [&](uint8_t hole) {
        auto v = kinds[hole];
        std::sort(v.begin(), v.end());
        return v;
    };
    require(sorted(kHolePacket) == std::vector<uint8_t>{kMovw0}, "the packet's index: one checked part");
    require(sorted(kHolePc) == (std::vector<uint8_t>{kMovw0Nc, kMovw1Nc}), "an i32: two parts, modulo 2^32");
    require(sorted(kHoleCountOffset) == (std::vector<uint8_t>{kMovw0Nc, kMovw1}), "under 2^32: two, the top checked");
    require(sorted(kHoleThreshold) == (std::vector<uint8_t>{kMovw0Nc, kMovw1Nc, kMovw2Nc, kMovw3}), "over: four");
    require(sorted(kHoleField0) == (std::vector<uint8_t>{kMovw0Nc, kMovw1Nc}), "a range the addend moves");
    for (const bcore_cp::Reloc& r : c.relocs)
        if (r.hole == kHoleField0) require(r.addend == 0x20, "the addend in the relocation");
    printf("widths: packet 1, PC 2, offset 2, wide 4, field + 0x20 2\n");
}

int main() {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();
    Compiler compiler;
    require(compiler.init(2, 2, /*split=*/true, kStencilTripleA64), "an AArch64 stencil compiler");

    // R0 += 2: the flags, the loop bottoms' compares with the PC, the
    // hardware loop's step on a rare path; it goes on to the next packet.
    const Code add = stencil16(compiler, 0x6410);
    check_relocs(add, "R0 += 2");
    require(tail_jump(add, kHoleCont) == 4 || tail_jump(add, kHoleExit) == 4, "a last B that may be dropped");
    check_patch(add, 0x89abcdef);
    // The PC + 2 its hardware loop step is given wraps (an i32's two
    // unchecked halves): 0xfffffffe + 2 is 0.
    bool wraps = false;
    for (const bcore_cp::Reloc& r : add.relocs) wraps |= r.hole == kHolePc && r.addend == 2;
    require(wraps, "a PC + 2");
    check_patch(add, 0xfffffffe);
    check_patch(add, 0xffa01234);  // L1 instruction SRAM, op1emu's code
    // R0 = [P0]: a load, the slow path a BL to mem_read32's helper jump.
    const Code load = stencil16(compiler, 0x9100);
    check_relocs(load, "R0 = [P0]");
    bool calls = false;
    for (const bcore_cp::Reloc& r : load.relocs)
        calls |= r.hole == kHoleHelper + 2 && (insn_at(load, r.offset) & 0xfc000000u) == 0x94000000u;
    require(calls, "a BL to mem_read32");
    check_patch(load, 0x89abcdef);
    check_patch(load, 0xfffffffe);
    check_narrow_store(compiler, 0x9a00, "mem_write8");   // B[P0++] = R0
    check_narrow_store(compiler, 0x9600, "mem_write16");  // W[P0++] = R0
    check_branch();
    check_widths(compiler);
    printf("cp a64 test passed\n");
    return 0;
}
