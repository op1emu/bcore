#include "lift_visitor.h"
#include "cec.h"
#include "mem.h"

#include <cstdio>
#include <cstddef>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

#include "utils/signextend.h"

using namespace llvm;

// ---------------------------------------------------------------------------
// mmod field constants for dsp32mac / dsp32mult instructions.
// Values mirror refs/opcode/bfin.h.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t MMOD_S2RND = 1;
constexpr uint32_t MMOD_T     = 2;
constexpr uint32_t MMOD_W32   = 3;
constexpr uint32_t MMOD_FU    = 4;
constexpr uint32_t MMOD_TFU   = 6;
constexpr uint32_t MMOD_IS    = 8;
constexpr uint32_t MMOD_ISS2  = 9;
constexpr uint32_t MMOD_IH    = 11;
constexpr uint32_t MMOD_IU    = 12;

constexpr bool is_macmod_signed(uint32_t m) {
    return m == 0      || m == MMOD_IS  || m == MMOD_T   || m == MMOD_S2RND
        || m == MMOD_ISS2 || m == MMOD_IH || m == MMOD_W32;
}

constexpr bool is_macmod_unsigned_acc(uint32_t m) {
    return m == MMOD_FU || m == MMOD_TFU || m == MMOD_IU;
}
} // namespace

// ---- Constructor ----

LiftVisitor::LiftVisitor(IRBuilder<>& builder, Value* cpu_ptr, Value* mem_ptr,
                         Module* module, Memory* mem, bool unlimited,
                         bool fastmem, uint64_t fast_base, uint32_t rawmem_limit)
    : builder_(builder), cpu_ptr_(cpu_ptr), mem_ptr_(mem_ptr), module_(module),
      mem_(mem), unlimited_mode_(unlimited), fastmem_(fastmem), fast_base_value_(fast_base),
      rawmem_limit_(rawmem_limit) {
}

uint16_t LiftVisitor::iftech(uint32_t addr) {
    return mem_->read16(addr);
}

// ---- IR Helper Methods ----

Value* LiftVisitor::cpu_field_ptr(size_t offset, Type* ty, const Twine& name) {
    auto* i8ptr = builder_.CreateBitCast(cpu_ptr_, builder_.getInt8PtrTy());
    auto* gep = builder_.CreateGEP(builder_.getInt8Ty(), i8ptr,
                                   builder_.getInt32(static_cast<uint32_t>(offset)));
    return builder_.CreateBitCast(gep, ty->getPointerTo(), name);
}

Value* LiftVisitor::load_cpu_u32(size_t offset, const Twine& name) {
    // CYCLES read (cycles[0]): reload cycles[1] (CYCLES2) from shadow (cycles[2]).
    // Bypass store_cpu_u32 to avoid the cycles[1]->cycles[2] redirect below.
    if (offset == offsetof(CpuState, cycles[0])) {
        auto* shadow_ptr = cpu_field_ptr(offsetof(CpuState, cycles[2]), builder_.getInt32Ty());
        auto* shadow_val = builder_.CreateLoad(builder_.getInt32Ty(), shadow_ptr, "cycles2_shadow");
        auto* c2_ptr = cpu_field_ptr(offsetof(CpuState, cycles[1]), builder_.getInt32Ty());
        builder_.CreateStore(shadow_val, c2_ptr);
    }
    auto* ptr = cpu_field_ptr(offset, builder_.getInt32Ty(), name + ".ptr");
    return builder_.CreateLoad(builder_.getInt32Ty(), ptr, name);
}

void LiftVisitor::store_cpu_u32(size_t offset, Value* val) {
    // CYCLES2 write (cycles[1]): redirect to shadow (cycles[2]); readable value
    // only updates when CYCLES is read (see load_cpu_u32 above).
    if (offset == offsetof(CpuState, cycles[1]))
        offset = offsetof(CpuState, cycles[2]);
    if (in_parallel_) {
        shadow_writes_[offset] = val;
    } else {
        auto* ptr = cpu_field_ptr(offset, builder_.getInt32Ty());
        builder_.CreateStore(val, ptr);
    }
}

void LiftVisitor::on_parallel_end() {
    for (const auto& [offset, val] : shadow_writes_) {
        auto* ptr = cpu_field_ptr(offset, builder_.getInt32Ty());
        builder_.CreateStore(val, ptr);
    }
    shadow_writes_.clear();
    in_parallel_ = false;
    parallel_slot_ = 0;
}

Value* LiftVisitor::load_dreg(uint32_t idx, const Twine& name) {
    return load_cpu_u32(offsetof(CpuState, dpregs) + idx * 4, name);
}

Value* LiftVisitor::load_preg(uint32_t idx, const Twine& name) {
    return load_cpu_u32(offsetof(CpuState, dpregs) + (8 + idx) * 4, name);
}


void LiftVisitor::store_dreg(uint32_t idx, Value* val) {
    store_cpu_u32(offsetof(CpuState, dpregs) + idx * 4, val);
}

void LiftVisitor::store_preg(uint32_t idx, Value* val) {
    store_cpu_u32(offsetof(CpuState, dpregs) + (8 + idx) * 4, val);
}


size_t LiftVisitor::allreg_offset(uint32_t grp, uint32_t reg) const {
    int fullreg = (grp << 3) | reg;
    switch (fullreg >> 2) {
    case 0: case 1: return offsetof(CpuState, dpregs) + reg * 4;           // R0-R7
    case 2: case 3: return offsetof(CpuState, dpregs) + (8 + (reg & 7)) * 4; // P0-P5,SP,FP
    case 4: return offsetof(CpuState, iregs) + (reg & 3) * 4;
    case 5: return offsetof(CpuState, mregs) + (reg & 3) * 4;
    case 6: return offsetof(CpuState, bregs) + (reg & 3) * 4;
    case 7: return offsetof(CpuState, lregs) + (reg & 3) * 4;
    default:
        switch (fullreg) {
        case 32: return offsetof(CpuState, ax[0]);
        case 33: return offsetof(CpuState, aw[0]);
        case 34: return offsetof(CpuState, ax[1]);
        case 35: return offsetof(CpuState, aw[1]);
        case 39: return offsetof(CpuState, rets);
        case 48: return offsetof(CpuState, lc[0]);
        case 49: return offsetof(CpuState, lt[0]);
        case 50: return offsetof(CpuState, lb[0]);
        case 51: return offsetof(CpuState, lc[1]);
        case 52: return offsetof(CpuState, lt[1]);
        case 53: return offsetof(CpuState, lb[1]);
        case 54: return offsetof(CpuState, cycles[0]);
        case 55: return offsetof(CpuState, cycles[1]);
        case 56: return offsetof(CpuState, usp);
        case 57: return offsetof(CpuState, seqstat);
        case 58: return offsetof(CpuState, syscfg);
        case 59: return offsetof(CpuState, reti);
        case 60: return offsetof(CpuState, retx);
        case 61: return offsetof(CpuState, retn);
        case 62: return offsetof(CpuState, rete);
        case 63: return offsetof(CpuState, emudat[0]);
        default:
            fprintf(stderr, "allreg_offset: unknown grp=%u reg=%u (fullreg=%d)\n", grp, reg, fullreg);
            return offsetof(CpuState, dpregs[0]); // fallback
        }
    }
}

void LiftVisitor::store_cc(Value* val) {
    store_cpu_u32(offsetof(CpuState, cc), val);
}

void LiftVisitor::store_ac0(Value* val) {
    store_cpu_u32(offsetof(CpuState, ac0), val);
    store_cpu_u32(offsetof(CpuState, ac0_copy), val);
}

void LiftVisitor::store_v(Value* val) {
    store_cpu_u32(offsetof(CpuState, v), val);
    store_cpu_u32(offsetof(CpuState, v_copy), val);
}

// Table of {CpuState field offset, ASTAT bit position} for all 15 defined flags.
// Order matches astat_compose / astat_decompose in cpu_state.h.
static const std::pair<size_t, unsigned> kAstatFields[] = {
    { offsetof(CpuState, az),           AZ_BIT       },
    { offsetof(CpuState, an),           AN_BIT       },
    { offsetof(CpuState, ac0_copy),     AC0_COPY_BIT },
    { offsetof(CpuState, v_copy),       V_COPY_BIT   },
    { offsetof(CpuState, cc),           CC_BIT       },
    { offsetof(CpuState, aq),           AQ_BIT       },
    { offsetof(CpuState, rnd_mod),      RND_MOD_BIT  },
    { offsetof(CpuState, ac0),          AC0_BIT      },
    { offsetof(CpuState, ac1),          AC1_BIT      },
    { offsetof(CpuState, av0),          AV0_BIT      },
    { offsetof(CpuState, av0s),         AV0S_BIT     },
    { offsetof(CpuState, av1),          AV1_BIT      },
    { offsetof(CpuState, av1s),         AV1S_BIT     },
    { offsetof(CpuState, v),            V_BIT        },
    { offsetof(CpuState, vs),           VS_BIT       },
};

// Emit inline IR that packs all ASTAT flag fields into a single i32 value.
// Equivalent to the extern "C" cpu_astat_compose() without an indirect call.
Value* LiftVisitor::emit_astat_compose() {
    Value* result = load_cpu_u32(offsetof(CpuState, astat_reserved));
    for (auto [off, bit] : kAstatFields) {
        Value* f = load_cpu_u32(off);
        if (bit > 0)
            f = builder_.CreateShl(f, builder_.getInt32(bit));
        result = builder_.CreateOr(result, f);
    }
    return result;
}

// Emit inline IR that unpacks an i32 ASTAT value into all individual flag fields.
// Equivalent to the extern "C" cpu_astat_decompose() without an indirect call.
void LiftVisitor::emit_astat_decompose(Value* astat) {
    for (auto [off, bit] : kAstatFields) {
        Value* shifted = (bit > 0) ? builder_.CreateLShr(astat, builder_.getInt32(bit))
                                   : astat;
        Value* masked  = builder_.CreateAnd(shifted, builder_.getInt32(1));
        store_cpu_u32(off, masked);
    }
    Value* reserved = builder_.CreateAnd(astat,
        builder_.getInt32(~ASTAT_DEFINED_BITS));
    store_cpu_u32(offsetof(CpuState, astat_reserved), reserved);
}

void LiftVisitor::emit_jump(Value* target) {
    // For runtime (non-constant) targets, check instruction-fetch alignment.
    // Blackfin requires all instruction fetches to be 16-bit aligned (even addresses).
    // An odd target raises an instruction alignment exception (EXCAUSE=0x23) via EVT3.
    if (!llvm::isa<llvm::ConstantInt>(target)) {
        auto& ctx  = builder_.getContext();
        auto* fn   = builder_.GetInsertBlock()->getParent();
        auto* bb_aligned   = llvm::BasicBlock::Create(ctx, "jump_aligned",   fn);
        auto* bb_misalign  = llvm::BasicBlock::Create(ctx, "jump_misalign",  fn);

        auto* lsb       = builder_.CreateAnd(target, builder_.getInt32(1), "lsb");
        auto* is_odd    = builder_.CreateICmpNE(lsb, builder_.getInt32(0), "is_odd");
        builder_.CreateCondBr(is_odd, bb_misalign, bb_aligned);

        // Misaligned branch: raise alignment exception.
        builder_.SetInsertPoint(bb_misalign);
        auto* ft_exc = llvm::FunctionType::get(builder_.getVoidTy(),
            {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
        call_extern("cec_exception", ft_exc,
            {cpu_ptr_, builder_.getInt32(VEC_MISALIGNI)});
        builder_.CreateRetVoid();

        // Aligned branch: normal jump.
        builder_.SetInsertPoint(bb_aligned);
    }

    store_cpu_u32(offsetof(CpuState, pc), target);
    // did_jump = true (bool at offset)
    auto* ptr = cpu_field_ptr(offsetof(CpuState, did_jump), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), ptr);
    terminated_ = true;
}

void LiftVisitor::emit_jump_imm(uint32_t target) {
    emit_jump(builder_.getInt32(target));
}

// If the CALL instruction sits at the bottom of an active hardware loop
// (lc[i] > 1 && lb[i] == current_pc), RETS must be set to lt[i] so that
// the subroutine returns to the loop-top rather than past the loop-bottom.
// Loop 1 has higher priority over loop 0 (checked last, wins via select).
llvm::Value* LiftVisitor::emit_hwloop_next_pc(uint32_t insn_len) {
    auto* v_cur_pc      = builder_.getInt32(current_pc);
    auto* v_fallthrough = builder_.getInt32(current_pc + insn_len);
    llvm::Value* result = v_fallthrough;
    for (int i = 0; i <= 1; ++i) {
        auto* lc      = load_cpu_u32(offsetof(CpuState, lc[0]) + i * 4, "lc");
        auto* lb      = load_cpu_u32(offsetof(CpuState, lb[0]) + i * 4, "lb");
        auto* lt      = load_cpu_u32(offsetof(CpuState, lt[0]) + i * 4, "lt");
        auto* cond_lc = builder_.CreateICmpUGT(lc, builder_.getInt32(1), "lc_gt1");
        auto* cond_lb = builder_.CreateICmpEQ(lb, v_cur_pc, "at_lb");
        auto* cond    = builder_.CreateAnd(cond_lc, cond_lb, "hwloop_cond");
        result = builder_.CreateSelect(cond, lt, result, "hwloop_rets");
    }
    return result;
}

void LiftVisitor::emit_insn_len(uint32_t len) {
    store_cpu_u32(offsetof(CpuState, insn_len), builder_.getInt32(len));
}

void LiftVisitor::emit_did_jump_exit(bool force) {
    if (terminated_ && !force) return;
    if (!check_did_jump_ && !force) return;

    auto& ctx = builder_.getContext();
    auto* fn = builder_.GetInsertBlock()->getParent();

    auto* bb_exit = BasicBlock::Create(ctx, "did_jump_exit", fn);
    auto* bb_cont = BasicBlock::Create(ctx, "did_jump_cont", fn);

    // Load did_jump (bool, stored as i8)
    auto* dj_ptr = cpu_field_ptr(offsetof(CpuState, did_jump), builder_.getInt8Ty());
    auto* dj_val = builder_.CreateLoad(builder_.getInt8Ty(), dj_ptr, "dj");
    auto* dj_set = builder_.CreateICmpNE(dj_val, builder_.getInt8(0), "dj_set");
    builder_.CreateCondBr(dj_set, bb_exit, bb_cont);

    // On exit: cpu->pc is already set by the exception handler; just return.
    builder_.SetInsertPoint(bb_exit);
    builder_.CreateRetVoid();

    builder_.SetInsertPoint(bb_cont);
}

void LiftVisitor::emit_step_check(uint32_t post_insn_pc) {
    if (unlimited_mode_) return;

    auto& ctx = builder_.getContext();
    auto* fn = builder_.GetInsertBlock()->getParent();

    auto* bb_check = BasicBlock::Create(ctx, "step_check", fn);
    auto* bb_exit  = BasicBlock::Create(ctx, "step_exit",  fn);
    auto* bb_cont  = BasicBlock::Create(ctx, "step_cont",  fn);

    // Load steps_remaining
    auto* sr_ptr = cpu_field_ptr(offsetof(CpuState, steps_remaining),
                                 builder_.getInt32Ty());
    auto* sr_val = builder_.CreateLoad(builder_.getInt32Ty(), sr_ptr, "sr");

    // If steps_remaining != 0 (limited mode), go to step_check; else skip
    auto* is_limited = builder_.CreateICmpNE(sr_val, builder_.getInt32(0), "is_limited");
    builder_.CreateCondBr(is_limited, bb_check, bb_cont);

    // bb_check: decrement counter; if it hits 0, exit now
    builder_.SetInsertPoint(bb_check);
    auto* sr_dec = builder_.CreateSub(sr_val, builder_.getInt32(1), "sr_dec");
    builder_.CreateStore(sr_dec, sr_ptr);
    auto* expired = builder_.CreateICmpEQ(sr_dec, builder_.getInt32(0), "expired");
    builder_.CreateCondBr(expired, bb_exit, bb_cont);

    // bb_exit: store post-instruction PC and set did_jump=true so host uses cpu.pc directly
    builder_.SetInsertPoint(bb_exit);
    store_cpu_u32(offsetof(CpuState, pc), builder_.getInt32(post_insn_pc));
    auto* dj_ptr = cpu_field_ptr(offsetof(CpuState, did_jump), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), dj_ptr);
    builder_.CreateRetVoid();

    // Continue here for next instruction
    builder_.SetInsertPoint(bb_cont);
}

void LiftVisitor::emit_epilog(uint32_t insn_pc, uint32_t insn_len) {
    // Delegate the full hw-loop probe to an extern "C" helper to keep IR compact.
    // bfin_hwloop_step():
    //   Phase A (when !did_jump): redirects cpu->pc via hw-loop back-edge rules.
    //   Phase B (always): decrements loop counter(s) whose bottom == insn_pc.
    auto* ft = llvm::FunctionType::get(
        builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty(), builder_.getInt32Ty()},
        false);
    call_extern("bfin_hwloop_step", ft,
        {cpu_ptr_, builder_.getInt32(insn_pc), builder_.getInt32(insn_pc + insn_len)});
    if (check_cec_pending_) {
        auto* ft_check = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getInt8PtrTy()}, false);
        call_extern("cec_check_pending", ft_check, {cpu_ptr_});
        check_cec_pending_ = false;
    }
}

void LiftVisitor::finalize_pending_exits(uint32_t insn_pc, uint32_t insn_len) {
    emit_epilog(insn_pc, insn_len);
    if (!pending_exits_.empty()) {
        auto* saved_bb = builder_.GetInsertBlock();
        for (auto& exit : pending_exits_) {
            builder_.SetInsertPoint(exit.block);
            // Emit hw-loop counter decrement (Phase B) on the taken path too.
            // Phase A (PC redirect) is a no-op here because did_jump is already set.
            emit_epilog(insn_pc, insn_len);
            builder_.CreateRetVoid();
        }
        pending_exits_.clear();
        builder_.SetInsertPoint(saved_bb);
    }
    // If bfin_hwloop_step redirected cpu->pc (set did_jump=true),
    // break out to the host loop so it can dispatch the new PC.
    if (!terminated_) {
        check_did_jump_ = true; // set flag to check did_jump after epilog
        emit_did_jump_exit(false);
        check_did_jump_ = false; // reset flag until next instruction that may set it
    }
}

Value* LiftVisitor::call_extern(const char* name, FunctionType* ft,
                                ArrayRef<Value*> args) {
    auto* fn = module_->getOrInsertFunction(name, ft).getCallee();
    return builder_.CreateCall(ft, fn, args);
}

bool LiftVisitor::unimplemented(const char* name) {
    fprintf(stderr, "UNIMPLEMENTED: %s at PC=0x%08x\n", name, current_pc);
    // Set halted=true, did_jump=true to break out
    auto* ptr = cpu_field_ptr(offsetof(CpuState, halted), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), ptr);
    auto* ecode_ptr = cpu_field_ptr(offsetof(CpuState, exit_code), builder_.getInt32Ty());
    builder_.CreateStore(builder_.getInt32(1), ecode_ptr);
    auto* djptr = cpu_field_ptr(offsetof(CpuState, did_jump), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), djptr);
    terminated_ = true;
    return true;
}

Value* LiftVisitor::build_acc_i64(Value* ax, Value* aw) {
    auto* i64ty = builder_.getInt64Ty();
    // Sign-extend ax (8-bit value stored in i32) to i64, then shift left 32
    auto* ax8 = builder_.CreateTrunc(ax, builder_.getInt8Ty(), "ax8");
    auto* ax64 = builder_.CreateSExt(ax8, i64ty, "ax64");
    auto* shifted = builder_.CreateShl(ax64, 32, "shifted");
    auto* aw64 = builder_.CreateZExt(aw, i64ty, "aw64");
    return builder_.CreateOr(shifted, aw64, "acc");
}

// Load accumulator n (ax[n] + aw[n]) as a sign-extended 40-bit i64.
Value* LiftVisitor::emit_load_acc(int n) {
    auto* ax = load_cpu_u32(offsetof(CpuState, ax[0]) + n * 4, "ax");
    auto* aw = load_cpu_u32(offsetof(CpuState, aw[0]) + n * 4, "aw");
    return build_acc_i64(ax, aw);
}

// Emit inline IR for SIGNBITS on a 40-bit accumulator (ax = ext byte, aw = lower 32).
// Algorithm: normalized = acc ^ (acc >> 63)  [arithmetic]
//            result     = ctlz(normalized) - 33
// This counts redundant sign bits below bit 39, then subtracts 8 for the extension byte.
Value* LiftVisitor::emit_signbits_acc(Value* ax, Value* aw) {
    auto* acc = build_acc_i64(ax, aw);
    // Arithmetic right shift by 63: all-1s if negative, all-0s if positive
    auto* sign_ext = builder_.CreateAShr(acc, 63, "sign_ext");
    // XOR flips all bits for negative values, identity for positive
    auto* normalized = builder_.CreateXor(acc, sign_ext, "sb_norm");
    // ctlz with is_zero_poison=false: returns 64 for zero input (correct for acc=0 and acc=-1)
    auto* ctlz_fn = Intrinsic::getDeclaration(module_, Intrinsic::ctlz,
                                              {builder_.getInt64Ty()});
    auto* leading = builder_.CreateCall(ctlz_fn, {normalized, builder_.getFalse()}, "sb_clz");
    // Subtract 24 (64-40) for 40-bit context, 1 for the sign bit, 8 for extension byte = 33
    auto* result64 = builder_.CreateSub(leading, builder_.getInt64(33), "sb_result");
    return builder_.CreateTrunc(result64, builder_.getInt32Ty(), "sb_i32");
}

// Compute ABS of 40-bit accumulator src_acc, write result to dst_acc.
// Returns av overflow flag as i32 (1 = saturation occurred, 0 = normal).
Value* LiftVisitor::emit_acc_abs(int src_acc, int dst_acc) {
    auto* i32ty = builder_.getInt32Ty();

    // Load source accumulator as sign-extended 40-bit i64
    size_t ax_src_off = src_acc == 0 ? offsetof(CpuState, ax[0]) : offsetof(CpuState, ax[1]);
    size_t aw_src_off = src_acc == 0 ? offsetof(CpuState, aw[0]) : offsetof(CpuState, aw[1]);
    auto* ax_val = load_cpu_u32(ax_src_off, "ax_src");
    auto* aw_val = load_cpu_u32(aw_src_off, "aw_src");
    auto* acc = build_acc_i64(ax_val, aw_val);

    auto* abs_acc = emit_abs(acc);

    // Overflow: -(2^39) negated is still 2^39 which doesn't fit in 40-bit signed
    auto* min_val = builder_.getInt64(1LL << 39);
    auto* max_val = builder_.getInt64((1LL << 39) - 1);  // 0x7FFFFFFFFF
    auto* av_i1 = builder_.CreateICmpEQ(abs_acc, min_val, "acc_av");
    auto* result = builder_.CreateSelect(av_i1, max_val, abs_acc, "acc_sat");

    // Decompose result into ax (bits 39:32) and aw (bits 31:0)
    auto* aw_new = builder_.CreateTrunc(result, i32ty, "aw_new");
    auto* ax_new = builder_.CreateTrunc(
        builder_.CreateLShr(result, 32, "ax_shifted"), i32ty, "ax_new");

    size_t ax_dst_off = dst_acc == 0 ? offsetof(CpuState, ax[0]) : offsetof(CpuState, ax[1]);
    size_t aw_dst_off = dst_acc == 0 ? offsetof(CpuState, aw[0]) : offsetof(CpuState, aw[1]);
    store_cpu_u32(aw_dst_off, aw_new);
    store_cpu_u32(ax_dst_off, ax_new);

    return builder_.CreateZExt(av_i1, i32ty, "av");
}

// =============================================
// dsp32shift / dsp32shiftimm helpers
// =============================================

Value* LiftVisitor::emit_extract_shift6(uint32_t src0_dreg) {
    auto* rt   = load_dreg(src0_dreg, "rt");
    auto* shl2 = builder_.CreateShl(rt, builder_.getInt32(2), "shl2");
    auto* byte_ = builder_.CreateTrunc(shl2, builder_.getInt8Ty(), "byte");
    return builder_.CreateAShr(
        builder_.CreateSExt(byte_, builder_.getInt32Ty()), builder_.getInt32(2), "shft6");
}

Value* LiftVisitor::emit_load_acc_unsigned(int n) {
    auto* i64ty = builder_.getInt64Ty();
    auto* ax_byte = builder_.CreateAnd(
        load_cpu_u32(offsetof(CpuState, ax[0]) + n * 4, "ax"), builder_.getInt32(0xFF), "ax8");
    auto* acc = builder_.CreateOr(
        builder_.CreateShl(builder_.CreateZExt(ax_byte, i64ty), builder_.getInt64(32)),
        builder_.CreateZExt(load_cpu_u32(offsetof(CpuState, aw[0]) + n * 4, "aw"), i64ty),
        "acc_u");
    return acc;
}

void LiftVisitor::emit_store_acc(int n, Value* acc64) {
    auto* i32ty = builder_.getInt32Ty();
    auto* aw_new = builder_.CreateTrunc(acc64, i32ty, "aw_new");
    auto* ax_new = builder_.CreateTrunc(
        builder_.CreateLShr(acc64, builder_.getInt64(32)), i32ty, "ax_new");
    store_cpu_u32(offsetof(CpuState, aw[0]) + n * 4, aw_new);
    store_cpu_u32(offsetof(CpuState, ax[0]) + n * 4, ax_new);
}

void LiftVisitor::emit_flags_az_an_16(Value* result16) {
    auto* i32ty = builder_.getInt32Ty();
    auto* az = builder_.CreateZExt(
        builder_.CreateICmpEQ(result16, builder_.getInt32(0)), i32ty, "az");
    auto* an = builder_.CreateZExt(
        builder_.CreateICmpNE(
            builder_.CreateAnd(result16, builder_.getInt32(0x8000)),
            builder_.getInt32(0)), i32ty, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
}

// Vector 2×16-bit AZ/AN: reference ORs flags from both halves.
//   AZ = (val0 == 0) || (val1 == 0)  — either half zero
//   AN = sign(val0)  || sign(val1)   — either half negative
// val0 and val1 must be the 16-bit half results in i32 (low 16 bits valid).
void LiftVisitor::emit_flags_az_an_v2x16(Value* val0, Value* val1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* az0 = builder_.CreateICmpEQ(builder_.CreateAnd(val0, builder_.getInt32(0xFFFF)),
                                      builder_.getInt32(0), "az0");
    auto* az1 = builder_.CreateICmpEQ(builder_.CreateAnd(val1, builder_.getInt32(0xFFFF)),
                                      builder_.getInt32(0), "az1");
    auto* an0 = builder_.CreateICmpNE(builder_.CreateAnd(val0, builder_.getInt32(0x8000)),
                                      builder_.getInt32(0), "an0");
    auto* an1 = builder_.CreateICmpNE(builder_.CreateAnd(val1, builder_.getInt32(0x8000)),
                                      builder_.getInt32(0), "an1");
    store_cpu_u32(offsetof(CpuState, az),
        builder_.CreateZExt(builder_.CreateOr(az0, az1), i32ty, "az"));
    store_cpu_u32(offsetof(CpuState, an),
        builder_.CreateZExt(builder_.CreateOr(an0, an1), i32ty, "an"));
}

void LiftVisitor::emit_flags_az_an_acc(Value* result64) {
    auto* i32ty = builder_.getInt32Ty();
    auto* az = builder_.CreateZExt(
        builder_.CreateICmpEQ(result64, builder_.getInt64(0)), i32ty, "az");
    auto* an = builder_.CreateZExt(
        builder_.CreateICmpNE(
            builder_.CreateAnd(result64, builder_.getInt64(1ULL << 39)),
            builder_.getInt64(0)), i32ty, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
}

// lshift v_i for a single 16-bit immediate shift:
//   v_i = NOT(bits_lost==0 OR (bits_lost==all_ones_mask AND result_is_neg))
// bits_lost is the i32 with the spilled bits (shifted_before_mask >> 16).
// result16 is i32 with low 16 bits valid.
// all_ones_bits is the compile-time mask ((1<<cnt)-1).
// Returns i1.
llvm::Value* LiftVisitor::emit_lshift16_vi_imm(
        llvm::Value* bits_lost, llvm::Value* result16, uint32_t all_ones_bits) {
    auto& b = builder_;
    auto* v_zero  = b.CreateICmpEQ(bits_lost, b.getInt32(0));
    auto* res_neg = b.CreateICmpNE(b.CreateLShr(result16, b.getInt32(15)), b.getInt32(0));
    auto* all_ones = b.CreateICmpEQ(bits_lost, b.getInt32(all_ones_bits));
    auto* exception = b.CreateAnd(all_ones, res_neg);
    return b.CreateNot(b.CreateOr(v_zero, exception), "lshift_vi");
}

// Shared wrap computation for vector arithmetic right shift when newimmag > 16:
//   lamt = left-shift amount (already computed: 16 - (newimmag & 0xF))
// Returns {out0, out1, v_flag_i32} where v_flag includes lshift v_i + sign-change for both halves.
std::tuple<llvm::Value*, llvm::Value*, llvm::Value*>
LiftVisitor::emit_vashift_wrap(llvm::Value* val0, llvm::Value* val1, uint32_t lamt) {
    auto& b = builder_;
    auto* i32ty = b.getInt32Ty();
    uint32_t all_ones_bits = (1u << lamt) - 1u;
    auto* ao = b.getInt32(all_ones_bits);

    auto* shifted0 = b.CreateShl(val0, b.getInt32(lamt), "wrapped0");
    auto* shifted1 = b.CreateShl(val1, b.getInt32(lamt), "wrapped1");
    auto* out0 = b.CreateAnd(shifted0, b.getInt32(0xFFFF), "out0");
    auto* out1 = b.CreateAnd(shifted1, b.getInt32(0xFFFF), "out1");

    // lshift v_i per half
    auto* bl0 = b.CreateLShr(shifted0, b.getInt32(16), "bl0");
    auto* bl1 = b.CreateLShr(shifted1, b.getInt32(16), "bl1");
    auto* rn0 = b.CreateICmpNE(b.CreateLShr(out0, b.getInt32(15)), b.getInt32(0));
    auto* rn1 = b.CreateICmpNE(b.CreateLShr(out1, b.getInt32(15)), b.getInt32(0));
    auto* vi0 = b.CreateNot(b.CreateOr(b.CreateICmpEQ(bl0, b.getInt32(0)),
                                        b.CreateAnd(b.CreateICmpEQ(bl0, ao), rn0)), "vi0");
    auto* vi1 = b.CreateNot(b.CreateOr(b.CreateICmpEQ(bl1, b.getInt32(0)),
                                        b.CreateAnd(b.CreateICmpEQ(bl1, ao), rn1)), "vi1");
    // sign-change per half
    auto* in_s0  = b.CreateLShr(val0, b.getInt32(15), "in_s0");
    auto* in_s1  = b.CreateLShr(val1, b.getInt32(15), "in_s1");
    auto* out_s0 = b.CreateLShr(out0, b.getInt32(15), "out_s0");
    auto* out_s1 = b.CreateLShr(out1, b.getInt32(15), "out_s1");
    auto* sc0 = b.CreateICmpNE(in_s0, out_s0, "sc0");
    auto* sc1 = b.CreateICmpNE(in_s1, out_s1, "sc1");
    auto* any_ov = b.CreateOr(b.CreateOr(vi0, vi1), b.CreateOr(sc0, sc1), "any_ov");
    auto* v_flag = b.CreateZExt(any_ov, i32ty, "v_flag");
    return {out0, out1, v_flag};
}

void LiftVisitor::emit_v_vs_update(Value* v_flag) {
    store_v(v_flag);
    auto* vs_old = load_cpu_u32(offsetof(CpuState, vs), "vs_old");
    store_cpu_u32(offsetof(CpuState, vs), builder_.CreateOr(vs_old, v_flag, "vs_new"));
}

Value* LiftVisitor::emit_shift16_overflow(Value* left_shifted_64, Value* left_result,
                                           Value* shft, Value* in_sign) {
    // left_shifted_64 is i64: the untruncated (val << cnt) preserving overflow bits.
    // left_result is i32: the low 16 bits of the shifted value.
    // shft is i32: the shift count (positive).
    // in_sign is i32: bit 15 of input (0 or 1).
    //
    // Overflow detection mirrors refs/bfin-sim.c lshift():
    //   high_bits = (val << cnt) >> 16   (bits shifted above the 16-bit window)
    //   all_ones  = (1 << cnt) - 1       (mask of cnt low bits)
    //   v_i = !( high_bits==0  ||  (high_bits==all_ones && result_sign) )
    //   overflow = v_i || (in_sign != result_sign)
    auto& b = builder_;
    auto* i64ty = b.getInt64Ty();
    auto* result_sign = b.CreateAnd(
        b.CreateLShr(left_result, b.getInt32(15)), b.getInt32(1), "result_sign");
    auto* high_bits  = b.CreateTrunc(
        b.CreateLShr(left_shifted_64, b.getInt64(16)), b.getInt32Ty(), "high_bits");
    auto* shft64     = b.CreateZExt(shft, i64ty, "shft64_ov");
    auto* all_ones   = b.CreateTrunc(
        b.CreateSub(b.CreateShl(b.getInt64(1), shft64), b.getInt64(1)),
        b.getInt32Ty(), "all_ones");
    auto* hb_zero    = b.CreateICmpEQ(high_bits, b.getInt32(0), "hb_zero");
    auto* hb_ones    = b.CreateICmpEQ(high_bits, all_ones, "hb_ones");
    auto* res_neg    = b.CreateICmpNE(result_sign, b.getInt32(0), "res_neg");
    auto* no_vi      = b.CreateOr(hb_zero, b.CreateAnd(hb_ones, res_neg), "no_vi");
    auto* v_i        = b.CreateNot(no_vi, "v_i");
    auto* sign_chg   = b.CreateICmpNE(in_sign, result_sign, "sign_chg");
    return b.CreateOr(v_i, sign_chg, "overflow");
}

Value* LiftVisitor::emit_extract_half16(Value* rs, uint32_t HLs) {
    if (HLs & 1)
        return builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "in_hi");
    else
        return builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "in_lo");
}

void LiftVisitor::emit_merge_half16(uint32_t dst, Value* result16, uint32_t HLs) {
    auto* rd = load_dreg(dst, "rd");
    llvm::Value* out;
    if (HLs & 2)
        out = builder_.CreateOr(builder_.CreateAnd(rd, builder_.getInt32(0x0000FFFF)),
                                builder_.CreateShl(result16, 16), "out");
    else
        out = builder_.CreateOr(builder_.CreateAnd(rd, builder_.getInt32(0xFFFF0000)),
                                result16, "out");
    store_dreg(dst, out);
}

Value* LiftVisitor::emit_xor_reduce_parity(Value* a, Value* b) {
    auto* v = builder_.CreateAnd(a, b, "xr_v");
    auto shr64 = [&](llvm::Value* x, uint64_t n) {
        return builder_.CreateLShr(x, builder_.getInt64(n));
    };
    v = builder_.CreateXor(v, shr64(v, 32));
    v = builder_.CreateXor(v, shr64(v, 16));
    v = builder_.CreateXor(v, shr64(v,  8));
    v = builder_.CreateXor(v, shr64(v,  4));
    v = builder_.CreateXor(v, shr64(v,  2));
    v = builder_.CreateXor(v, shr64(v,  1));
    return builder_.CreateTrunc(
        builder_.CreateAnd(v, builder_.getInt64(1)), builder_.getInt32Ty(), "xr");
}

// =============================================
// Flag / memory helper implementations
// =============================================

void LiftVisitor::emit_flags_az_an(llvm::Value* result) {
    auto* az = builder_.CreateZExt(
        builder_.CreateICmpEQ(result, builder_.getInt32(0)),
        builder_.getInt32Ty(), "az");
    auto* an = builder_.CreateLShr(result, 31, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
}

void LiftVisitor::emit_flags_logic(llvm::Value* result) {
    emit_flags_az_an(result);
    store_ac0(builder_.getInt32(0));
    store_v(builder_.getInt32(0));
}

void LiftVisitor::emit_flags_arith(llvm::Value* result, llvm::Value* v_flag, llvm::Value* ac0) {
    emit_flags_az_an(result);
    auto* vs_new = builder_.CreateOr(
        load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag);
    store_v(v_flag);
    store_cpu_u32(offsetof(CpuState, vs), vs_new);
    store_ac0(ac0);
}

llvm::Value* LiftVisitor::emit_sext_half16(llvm::Value* r, bool hi) {
    auto* sh = hi ? builder_.CreateLShr(r, 16) : r;
    return builder_.CreateSExt(
        builder_.CreateTrunc(sh, builder_.getInt16Ty()),
        builder_.getInt32Ty());
}

void LiftVisitor::emit_flags_nz_2x16(llvm::Value* lo_half, llvm::Value* hi_half) {
    auto* m16 = builder_.getInt32(0xFFFF);
    auto* az = builder_.CreateZExt(
        builder_.CreateOr(
            builder_.CreateICmpEQ(builder_.CreateAnd(lo_half, m16), builder_.getInt32(0)),
            builder_.CreateICmpEQ(builder_.CreateAnd(hi_half, m16), builder_.getInt32(0))),
        builder_.getInt32Ty(), "az");
    auto* an = builder_.CreateZExt(
        builder_.CreateOr(
            builder_.CreateICmpNE(builder_.CreateAnd(lo_half, builder_.getInt32(0x8000)), builder_.getInt32(0)),
            builder_.CreateICmpNE(builder_.CreateAnd(hi_half, builder_.getInt32(0x8000)), builder_.getInt32(0))),
        builder_.getInt32Ty(), "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
}

llvm::Value* LiftVisitor::emit_clamp_u8(llvm::Value* v) {
    return emit_umin(emit_smax(v, builder_.getInt32(0)), builder_.getInt32(255));
}

Value* LiftVisitor::emit_smin(Value* a, Value* b) {
    return builder_.CreateBinaryIntrinsic(Intrinsic::smin, a, b);
}
Value* LiftVisitor::emit_smax(Value* a, Value* b) {
    return builder_.CreateBinaryIntrinsic(Intrinsic::smax, a, b);
}
Value* LiftVisitor::emit_umin(Value* a, Value* b) {
    return builder_.CreateBinaryIntrinsic(Intrinsic::umin, a, b);
}
Value* LiftVisitor::emit_abs(Value* v) {
    auto* fn = Intrinsic::getDeclaration(module_, Intrinsic::abs, {v->getType()});
    return builder_.CreateCall(fn, {v, builder_.getFalse()});
}
Value* LiftVisitor::emit_fshr(Value* hi, Value* lo, Value* amt) {
    return builder_.CreateIntrinsic(Intrinsic::fshr, {hi->getType()}, {hi, lo, amt});
}

// ---------------------------------------------------------------------------
// DSP saturation / rounding helpers (used by MAC extraction)
// ---------------------------------------------------------------------------

Value* LiftVisitor::emit_rnd16(Value* v64) {
    auto& b = builder_;
    auto* low16   = b.CreateAnd(v64, b.getInt64(0xFFFFLL), "low16");
    auto* gt_half = b.CreateICmpUGT(low16, b.getInt64(0x8000LL));
    auto* eq_half = b.CreateICmpEQ(low16, b.getInt64(0x8000LL));
    auto* bit16   = b.CreateICmpNE(b.CreateAnd(v64, b.getInt64(0x10000LL)), b.getInt64(0));
    auto* round_up = b.CreateOr(gt_half, b.CreateAnd(eq_half, bit16));
    auto* rounded  = b.CreateAdd(v64, b.CreateSelect(round_up, b.getInt64(0x8000LL), b.getInt64(0)));
    auto* sgnbits  = b.CreateAnd(rounded, b.getInt64((int64_t)(uint64_t)0xFFFF000000000000ULL), "sgnbits");
    auto* shifted  = b.CreateLShr(rounded, b.getInt64(16));
    return b.CreateOr(shifted, sgnbits);
}

Value* LiftVisitor::emit_trunc16(Value* v64) {
    auto& b = builder_;
    auto* sgnbits = b.CreateAnd(v64, b.getInt64((int64_t)(uint64_t)0xFFFF000000000000ULL), "sgnbits_t");
    auto* shifted = b.CreateLShr(v64, b.getInt64(16));
    return b.CreateOr(shifted, sgnbits);
}

Value* LiftVisitor::emit_sat_s16(Value* v64, Value** ov_out) {
    auto& b = builder_;
    auto* lo    = b.getInt64(-32768LL);
    auto* hi    = b.getInt64(32767LL);
    auto* under = b.CreateICmpSLT(v64, lo);
    auto* over  = b.CreateICmpSGT(v64, hi);
    auto* clamped = emit_smin(emit_smax(v64, lo), hi);
    if (ov_out)
        *ov_out = b.CreateZExt(b.CreateOr(under, over), b.getInt32Ty(), "ov_s16");
    return b.CreateTrunc(clamped, b.getInt32Ty(), "sat_s16");
}

Value* LiftVisitor::emit_sat_u16(Value* v64, Value** ov_out) {
    auto& b = builder_;
    auto* hi   = b.getInt64(0xFFFFLL);
    auto* over = b.CreateICmpUGT(v64, hi);
    auto* clamped = emit_umin(v64, hi);
    if (ov_out)
        *ov_out = b.CreateZExt(over, b.getInt32Ty(), "ov_u16");
    return b.CreateTrunc(clamped, b.getInt32Ty(), "sat_u16");
}

Value* LiftVisitor::emit_sat_s32(Value* v64, Value** ov_out) {
    auto& b = builder_;
    auto* lo32 = b.getInt64(-(int64_t)0x80000000LL);
    auto* hi32 = b.getInt64(0x7fffffffLL);
    auto* under = b.CreateICmpSLT(v64, lo32);
    auto* over  = b.CreateICmpSGT(v64, hi32);
    auto* clamped = emit_smin(emit_smax(v64, lo32), hi32);
    if (ov_out)
        *ov_out = b.CreateZExt(b.CreateOr(under, over), b.getInt32Ty(), "ov_s32");
    return b.CreateTrunc(clamped, b.getInt32Ty(), "sat_s32");
}

Value* LiftVisitor::emit_sat_u32(Value* v64, Value** ov_out) {
    auto& b = builder_;
    auto* hi_u32 = b.getInt64(0xFFFFFFFFLL);
    auto* over   = b.CreateICmpUGT(v64, hi_u32);
    auto* clamped = emit_umin(v64, hi_u32);
    if (ov_out)
        *ov_out = b.CreateZExt(over, b.getInt32Ty(), "ov_u32");
    return b.CreateTrunc(clamped, b.getInt32Ty(), "sat_u32");
}

// Shared signed-subtract comparison kernel: computes (src - dst) and returns
// the resulting AZ, AN, and AC0 flags used by all CCflag EQ/LT/LE variants.
LiftVisitor::CmpFlags LiftVisitor::emit_cc_cmp(Value* src, Value* dst) {
    auto* result = builder_.CreateSub(src, dst, "diff");
    auto* az     = builder_.CreateZExt(
        builder_.CreateICmpEQ(result, builder_.getInt32(0)), builder_.getInt32Ty(), "az");
    auto* an_neg = builder_.CreateLShr(result, builder_.getInt32(31), "flgn");
    auto* flgs   = builder_.CreateLShr(src,    builder_.getInt32(31), "flgs");
    auto* flgo   = builder_.CreateLShr(dst,    builder_.getInt32(31), "flgo");
    auto* of     = builder_.CreateAnd(builder_.CreateXor(flgs, flgo),
                                      builder_.CreateXor(an_neg, flgs), "overflow");
    auto* an     = builder_.CreateXor(an_neg, of, "an");
    auto* ac0    = builder_.CreateZExt(
        builder_.CreateICmpULE(dst, src), builder_.getInt32Ty(), "ac0");
    return {az, an, ac0};
}

llvm::Value* LiftVisitor::emit_mem_read(const char* fn, llvm::Type* ret_ty, llvm::Value* addr) {
    if (fastmem_) {
        auto* i64_ty = builder_.getInt64Ty();
        auto* base_c = ConstantInt::get(i64_ty, fast_base_value_);
        auto* addr64 = builder_.CreateZExt(addr, i64_ty);
        auto* host_addr = builder_.CreateAdd(base_c, addr64);
        auto* ptr = builder_.CreateIntToPtr(host_addr, ret_ty->getPointerTo());
        return builder_.CreateAlignedLoad(ret_ty, ptr, llvm::MaybeAlign(1));
    }
    // Non-fastmem rawmem fast path: inline load for addr < rawmem_limit_ (normal data range).
    // For MMR addresses (>= rawmem_limit_), fall back to extern call which handles them.
    if (rawmem_limit_ != 0) {
        auto& ctx = builder_.getContext();
        auto* fn_  = builder_.GetInsertBlock()->getParent();
        auto* bb_fast = llvm::BasicBlock::Create(ctx, "rawmem_fast", fn_);
        auto* bb_slow = llvm::BasicBlock::Create(ctx, "rawmem_slow", fn_);
        auto* bb_done = llvm::BasicBlock::Create(ctx, "rawmem_done", fn_);

        // Check: addr < rawmem_limit_
        auto* limit_c = builder_.getInt32(rawmem_limit_);
        auto* in_range = builder_.CreateICmpULT(addr, limit_c, "in_range");
        // Hint: fast path (addr in normal range) is overwhelmingly likely
        llvm::MDBuilder mdb(builder_.getContext());
        auto* likely_metadata = mdb.createBranchWeights(1000, 1);
        builder_.CreateCondBr(in_range, bb_fast, bb_slow, likely_metadata);

        // Fast path: direct load from fast_base + addr
        builder_.SetInsertPoint(bb_fast);
        auto* i64_ty = builder_.getInt64Ty();
        auto* base_c = ConstantInt::get(i64_ty, fast_base_value_);
        auto* addr64 = builder_.CreateZExt(addr, i64_ty);
        auto* host_addr = builder_.CreateAdd(base_c, addr64);
        auto* ptr = builder_.CreateIntToPtr(host_addr, ret_ty->getPointerTo());
        auto* fast_result = builder_.CreateAlignedLoad(ret_ty, ptr, llvm::MaybeAlign(1));
        auto* fast_bb = builder_.GetInsertBlock();
        builder_.CreateBr(bb_done);

        // Slow path: extern call (for MMR addresses, fault addresses, etc.)
        builder_.SetInsertPoint(bb_slow);
        auto* ft = llvm::FunctionType::get(ret_ty,
            {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
        auto* slow_result = call_extern(fn, ft, {mem_ptr_, addr});
        emit_did_jump_exit(true);  // fault may have set did_jump
        auto* slow_bb = builder_.GetInsertBlock();
        builder_.CreateBr(bb_done);

        builder_.SetInsertPoint(bb_done);
        auto* phi = builder_.CreatePHI(ret_ty, 2, "mem_val");
        phi->addIncoming(fast_result, fast_bb);
        phi->addIncoming(slow_result, slow_bb);
        return phi;
    }
    auto* ft = llvm::FunctionType::get(ret_ty,
        {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
    auto* result = call_extern(fn, ft, {mem_ptr_, addr});
    // If mem_read failed, must break out immediately to avoid follow-up instructions
    emit_did_jump_exit(true);
    return result;
}

void LiftVisitor::emit_mem_write(const char* fn, llvm::Value* addr, llvm::Value* val) {
    check_cec_pending_ = true; // mem_write may trigger CEC pending interrupt, so check after write.
    if (fastmem_) {
        // Tier 1: fastmem — direct store, no bounds check, no notify
        auto* i64_ty = builder_.getInt64Ty();
        auto* base_c = ConstantInt::get(i64_ty, fast_base_value_);
        auto* addr64 = builder_.CreateZExt(addr, i64_ty);
        auto* host_addr = builder_.CreateAdd(base_c, addr64);
        auto* ptr = builder_.CreateIntToPtr(host_addr, val->getType()->getPointerTo());
        builder_.CreateAlignedStore(val, ptr, llvm::MaybeAlign(1));
        return;
    }
    // Tier 2: non-fastmem with rawmem_limit — inline store for addr < rawmem_limit_,
    // extern call (for MMR addresses) otherwise.
    if (rawmem_limit_ != 0) {
        auto& ctx = builder_.getContext();
        auto* fn_  = builder_.GetInsertBlock()->getParent();
        auto* bb_fast = llvm::BasicBlock::Create(ctx, "rawmem_wr_fast", fn_);
        auto* bb_slow = llvm::BasicBlock::Create(ctx, "rawmem_wr_slow", fn_);
        auto* bb_done = llvm::BasicBlock::Create(ctx, "rawmem_wr_done", fn_);

        auto* limit_c = builder_.getInt32(rawmem_limit_);
        auto* in_range = builder_.CreateICmpULT(addr, limit_c, "in_range");
        llvm::MDBuilder mdb(builder_.getContext());
        auto* likely_metadata = mdb.createBranchWeights(1000, 1);
        builder_.CreateCondBr(in_range, bb_fast, bb_slow, likely_metadata);

        // Fast path: direct store to fast_base + addr (no notify, no extern call)
        builder_.SetInsertPoint(bb_fast);
        auto* i64_ty = builder_.getInt64Ty();
        auto* base_c = ConstantInt::get(i64_ty, fast_base_value_);
        auto* addr64 = builder_.CreateZExt(addr, i64_ty);
        auto* host_addr = builder_.CreateAdd(base_c, addr64);
        auto* ptr = builder_.CreateIntToPtr(host_addr, val->getType()->getPointerTo());
        builder_.CreateAlignedStore(val, ptr, llvm::MaybeAlign(1));
        builder_.CreateBr(bb_done);

        // Slow path: extern call for MMR addresses; may raise exception or set did_jump
        builder_.SetInsertPoint(bb_slow);
        auto* val_ty = val->getType();
        auto* ft = llvm::FunctionType::get(builder_.getVoidTy(),
            {builder_.getInt8PtrTy(), builder_.getInt32Ty(), val_ty}, false);
        call_extern(fn, ft, {mem_ptr_, addr, val});
        emit_did_jump_exit(true);  // MMR write may have raised an exception
        builder_.CreateBr(bb_done);

        builder_.SetInsertPoint(bb_done);
        return;
    }
    // Tier 3: always extern call (rawmem_limit not available)
    auto* val_ty = val->getType();
    auto* ft = llvm::FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty(), val_ty}, false);
    call_extern(fn, ft, {mem_ptr_, addr, val});
    // If mem_write failed, must break out immediately to avoid follow-up instructions
    emit_did_jump_exit(true);
}

// =============================================
// IMPLEMENTED instructions (needed for cc-alu.S)
// =============================================

bool LiftVisitor::decode_ProgCtrl_NOP() { return true; }
bool LiftVisitor::decode_ProgCtrl_EMUEXCPT() {
    if (parallel_slot_ > 0) return false;
    return true; // NOP in emulation
}
bool LiftVisitor::decode_ProgCtrl_CSYNC() {
    if (parallel_slot_ > 0) return false;
    return true;
}
bool LiftVisitor::decode_ProgCtrl_SSYNC() {
    if (parallel_slot_ > 0) return false;
    return true;
}

bool LiftVisitor::decode_ProgCtrl_EXCPT(uint16_t imm) {
    if (parallel_slot_ > 0) return false;

    // All EXCPT N (N=0..15) go through CEC exception dispatch.
    // The libgloss syscall convention (EXCPT 0 with P0=syscall_nr) is handled
    // inside cec_exception when EVT3 is not installed (see cec.cpp).
    auto* ft = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
    call_extern("cec_exception", ft, {cpu_ptr_, builder_.getInt32(imm)});
    // cec_exception sets cpu->pc = EVT3 and cpu->did_jump = true.
    terminated_ = true;
    return true;
}

bool LiftVisitor::decode_ProgCtrl_RTS() {
    if (parallel_slot_ > 0) return false;

    auto* rets = load_cpu_u32(offsetof(CpuState, rets), "rets");
    emit_jump(rets);
    return true;
}

bool LiftVisitor::decode_pseudoDEBUG_ABORT(uint16_t g) {
    auto* ptr = cpu_field_ptr(offsetof(CpuState, halted), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), ptr);
    auto* ecode_ptr = cpu_field_ptr(offsetof(CpuState, exit_code), builder_.getInt32Ty());
    builder_.CreateStore(builder_.getInt32(1), ecode_ptr);
    auto* djptr = cpu_field_ptr(offsetof(CpuState, did_jump), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), djptr);
    terminated_ = true;
    return true;
}

// UJUMP: PC = PC + sext12(offset) * 2
bool LiftVisitor::decode_UJUMP(uint16_t offset) {
    if (parallel_slot_ > 0) return false;

    int32_t soff = signextend<12>(offset);
    uint32_t target = current_pc + soff * 2;
    emit_jump_imm(target);
    return true;
}

// BRCC: conditional branches
bool LiftVisitor::decode_BRCC_BRF(uint16_t offset) {
    if (parallel_slot_ > 0) return false;

    int32_t soff = signextend<10>(offset);
    uint32_t target = current_pc + soff * 2;
    auto* cc_val = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* cond = builder_.CreateICmpEQ(cc_val, builder_.getInt32(0), "cc_false");

    auto* parent_fn = builder_.GetInsertBlock()->getParent();
    auto* taken_bb = BasicBlock::Create(module_->getContext(), "brcc_taken", parent_fn);
    auto* fallthru_bb = BasicBlock::Create(module_->getContext(), "brcc_fallthru", parent_fn);

    builder_.CreateCondBr(cond, taken_bb, fallthru_bb);

    builder_.SetInsertPoint(taken_bb);
    emit_jump_imm(target);
    // Don't RetVoid yet — emit_epilog must run on this path too (counter decrement).
    pending_exits_.push_back({taken_bb});
    terminated_ = false;  // reset so fallthru path continues

    builder_.SetInsertPoint(fallthru_bb);
    return true;
}

bool LiftVisitor::decode_BRCC_BRF_BP(uint16_t offset) {
    if (parallel_slot_ > 0) return false;

    return decode_BRCC_BRF(offset); // BP hint doesn't affect semantics
}

bool LiftVisitor::decode_BRCC_BRT(uint16_t offset) {
    if (parallel_slot_ > 0) return false;

    int32_t soff = signextend<10>(offset);
    uint32_t target = current_pc + soff * 2;
    auto* cc_val = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* cond = builder_.CreateICmpNE(cc_val, builder_.getInt32(0), "cc_true");

    auto* parent_fn = builder_.GetInsertBlock()->getParent();
    auto* taken_bb = BasicBlock::Create(module_->getContext(), "brt_taken", parent_fn);
    auto* fallthru_bb = BasicBlock::Create(module_->getContext(), "brt_fallthru", parent_fn);

    builder_.CreateCondBr(cond, taken_bb, fallthru_bb);

    builder_.SetInsertPoint(taken_bb);
    emit_jump_imm(target);
    // Don't RetVoid yet — emit_epilog must run on this path too (counter decrement).
    pending_exits_.push_back({taken_bb});
    terminated_ = false;  // reset so fallthru path continues

    builder_.SetInsertPoint(fallthru_bb);
    return true;
}

bool LiftVisitor::decode_BRCC_BRT_BP(uint16_t offset) {
    if (parallel_slot_ > 0) return false;

    return decode_BRCC_BRT(offset);
}

// REGMV: dst_reg = src_reg
bool LiftVisitor::decode_REGMV(uint16_t gd, uint16_t gs, uint16_t d, uint16_t s) {
    if (parallel_slot_ > 0) return false;

    // Reserved slots are always illegal (matches disasm_visitor)
    auto is_reserved = [](uint32_t g, uint32_t r) {
        return (g == 4 && (r == 4 || r == 5)) || (g == 5);
    };
    if (is_reserved(gs, s) || is_reserved(gd, d)) return false;

    // Valid combinations (matching disasm_visitor / reference bfin-dis.c)
    bool valid = false;
    if (gs < 2 || gd < 2)                                          valid = true; // Dreg/Preg
    else if (gs == 4 && s < 4)                                     valid = true; // Accum src
    else if (gd == 4 && d < 4 && gs < 4)                          valid = true; // Accum dst, non-sysreg src
    else if (gs == 7 && s == 7 && !(gd == 4 && d < 4))            valid = true; // EMUDAT src
    else if (gd == 7 && d == 7)                                    valid = true; // EMUDAT dst
    else if (gs < 4 && gd < 4)                                     valid = true; // DAGreg <-> DAGreg
    else if (gs == 7 && s == 0 && gd >= 4)                        valid = true; // USP -> sysreg
    else if ((gs == 7 && s == 0 && gd == 4 && d < 4) ||
             (gd == 7 && d == 0 && gs == 4 && s < 4))             valid = true; // USP <-> accum
    if (!valid) return false;

    // Group 7 (system registers: USP, SEQSTAT, SYSCFG, RETI, RETX, RETN, RETE, EMUDAT)
    // require supervisor mode. Raises VEC_ILL_RES (0x2e) from user mode.
    if (gs == 7 || gd == 7) {
        auto* ft_sup = FunctionType::get(builder_.getVoidTy(),
            {builder_.getInt8PtrTy()}, false);
        call_extern("cec_check_sup", ft_sup, {cpu_ptr_});
        emit_did_jump_exit(true); // exit before register read/write if exception was raised
    }

    // Special case: ASTAT read (grp=4, reg=6)
    bool src_is_astat = (gs == 4 && s == 6);
    bool dst_is_astat = (gd == 4 && d == 6);

    Value* val;
    if (src_is_astat) {
        val = emit_astat_compose();
    } else {
        // AX regs: sign-extend from 8 bits
        int src_fullreg = (gs << 3) | s;
        size_t off = allreg_offset(gs, s);
        val = load_cpu_u32(off, "regmv_src");
        if (src_fullreg == 32 || src_fullreg == 34) {
            // A0.X or A1.X — sign extend from 8 bits
            auto* sext = builder_.CreateShl(val, 24);
            val = builder_.CreateAShr(sext, 24, "ax_sext");
        }
    }

    if (dst_is_astat) {
        emit_astat_decompose(val);
    } else {
        int dst_fullreg = (gd << 3) | d;
        size_t off = allreg_offset(gd, d);
        // AX regs: mask to 8 bits
        if (dst_fullreg == 32 || dst_fullreg == 34) {
            val = builder_.CreateAnd(val, builder_.getInt32(0xFF), "ax_mask");
        }
        // LT regs: clear LSB
        if (dst_fullreg == 49 || dst_fullreg == 52) {
            val = builder_.CreateAnd(val, builder_.getInt32(~1u), "lt_clear_lsb");
        }
        // SEQSTAT (fullreg 57) is read-only: discard writes
        if (dst_fullreg == 57) return true;
        store_cpu_u32(off, val);
    }
    return true;
}

// COMPI2opD_EQ: Rd = sext7(imm)
bool LiftVisitor::decode_COMPI2opD_EQ(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    int32_t sval = signextend<7>(imm);
    store_dreg(d, builder_.getInt32(static_cast<uint32_t>(sval)));
    return true;
}

// COMPI2opD_ADD: Rd += sext7(imm)
bool LiftVisitor::decode_COMPI2opD_ADD(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    int32_t sval = signextend<7>(imm);
    auto* imm_val = builder_.getInt32(static_cast<uint32_t>(sval));
    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateAdd(rd, imm_val, "rd_add");
    store_dreg(d, result);

    // Signed overflow: (flgs ^ flgn) & (flgo ^ flgn)
    auto* flgs = builder_.CreateLShr(rd,      builder_.getInt32(31), "flgs");
    auto* flgo = builder_.CreateLShr(imm_val, builder_.getInt32(31), "flgo");
    auto* flgn = builder_.CreateLShr(result,  builder_.getInt32(31), "flgn");
    auto* overflow = builder_.CreateAnd(
        builder_.CreateXor(flgs, flgn),
        builder_.CreateXor(flgo, flgn), "overflow");
    auto* v_flag = builder_.CreateZExt(overflow, builder_.getInt32Ty(), "v");

    // Unsigned carry: ~rd < imm_val (unsigned)
    auto* ac0 = builder_.CreateZExt(
        builder_.CreateICmpULT(builder_.CreateNot(rd), imm_val),
        builder_.getInt32Ty(), "ac0");

    emit_flags_arith(result, v_flag, ac0);
    return true;
}

// COMPI2opP_EQ: Pd = sext7(imm)
bool LiftVisitor::decode_COMPI2opP_EQ(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    int32_t sval = signextend<7>(imm);
    store_preg(d, builder_.getInt32(static_cast<uint32_t>(sval)));
    return true;
}

// COMPI2opP_ADD: Pd += sext7(imm)
bool LiftVisitor::decode_COMPI2opP_ADD(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    int32_t sval = signextend<7>(imm);
    auto* pd = load_preg(d, "pd");
    auto* result = builder_.CreateAdd(pd, builder_.getInt32(static_cast<uint32_t>(sval)), "pd_add");
    store_preg(d, result);
    return true;
}

// LOGI2op shifts
// Flags: AZ/AN set, V=0 (V_COPY=0).  AC0 not touched.
bool LiftVisitor::decode_LOGI2op_LSHIFT_LEFT(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateShl(rd, builder_.getInt32(imm), "lsl");
    store_dreg(d, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}

// Flags: AZ/AN set, V=0 (V_COPY=0).  AC0 not touched.
bool LiftVisitor::decode_LOGI2op_LSHIFT_RIGHT(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateLShr(rd, builder_.getInt32(imm), "lsr");
    store_dreg(d, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}

// Flags: AZ/AN set, V=0 (V_COPY=0).  AC0 not touched.
bool LiftVisitor::decode_LOGI2op_ASHIFT_RIGHT(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateAShr(rd, builder_.getInt32(imm), "asr");
    store_dreg(d, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}

bool LiftVisitor::decode_LOGI2op_CC_EQ_BITTST(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* bit = builder_.CreateAnd(builder_.CreateLShr(rd, builder_.getInt32(imm)),
                                   builder_.getInt32(1), "bittst");
    store_cc(bit);
    return true;
}

// CCflag: CC comparisons
// CC = (Rx == Ry) for dreg/preg  (opc=0)
bool LiftVisitor::decode_CCflag_EQ(bool preg, uint16_t y, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = preg ? load_preg(y, "py") : load_dreg(y, "ry");
    auto [az, an, ac0] = emit_cc_cmp(src, dst);
    store_cc(az);  // opc=0 → CC = AZ
    // Pointer compares (preg=true) only touch CC, not ASTAT
    if (!preg) {
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}

// CC = (Rx < Ry signed) (opc=1)
bool LiftVisitor::decode_CCflag_LT(bool preg, uint16_t y, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = preg ? load_preg(y, "py") : load_dreg(y, "ry");
    auto [az, an, ac0] = emit_cc_cmp(src, dst);
    store_cc(an);  // opc=1 → CC = AN
    // Pointer compares (preg=true) only touch CC, not ASTAT
    if (!preg) {
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}

// CC = (Rx <= Ry signed) (opc=2)
bool LiftVisitor::decode_CCflag_LE(bool preg, uint16_t y, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = preg ? load_preg(y, "py") : load_dreg(y, "ry");
    auto [az, an, ac0] = emit_cc_cmp(src, dst);
    store_cc(builder_.CreateOr(an, az, "le"));  // opc=2 → CC = AN || AZ
    // Pointer compares (preg=true) only touch CC, not ASTAT
    if (!preg) {
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}

// Accumulator comparisons: CC = A0 == A1
bool LiftVisitor::decode_CCflag_A0_EQ_A1() {
    if (parallel_slot_ > 0) return false;

    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    auto* diff = builder_.CreateSub(acc0, acc1, "diff");
    store_cc(builder_.CreateZExt(builder_.CreateICmpEQ(acc0, acc1), builder_.getInt32Ty(), "cc"));
    auto* az_val = builder_.CreateZExt(builder_.CreateICmpEQ(diff, builder_.getInt64(0)), builder_.getInt32Ty(), "az");
    auto* an_val = builder_.CreateZExt(builder_.CreateICmpSLT(diff, builder_.getInt64(0)), builder_.getInt32Ty(), "an");
    // ac0 = (u40)acc1 <= (u40)acc0 — mask to 40 bits for unsigned compare
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* ac0_val = builder_.CreateZExt(
        builder_.CreateICmpULE(builder_.CreateAnd(acc1, mask40),
                               builder_.CreateAnd(acc0, mask40)), builder_.getInt32Ty(), "ac0");
    store_cpu_u32(offsetof(CpuState, az), az_val);
    store_cpu_u32(offsetof(CpuState, an), an_val);
    store_ac0(ac0_val);
    return true;
}

// CC = A0 < A1
bool LiftVisitor::decode_CCflag_A0_LT_A1() {
    if (parallel_slot_ > 0) return false;

    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    auto* diff = builder_.CreateSub(acc0, acc1, "diff");
    store_cc(builder_.CreateZExt(builder_.CreateICmpSLT(acc0, acc1), builder_.getInt32Ty(), "cc"));
    auto* az_val = builder_.CreateZExt(builder_.CreateICmpEQ(diff, builder_.getInt64(0)), builder_.getInt32Ty(), "az");
    auto* an_val = builder_.CreateZExt(builder_.CreateICmpSLT(diff, builder_.getInt64(0)), builder_.getInt32Ty(), "an");
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* ac0_val = builder_.CreateZExt(
        builder_.CreateICmpULE(builder_.CreateAnd(acc1, mask40),
                               builder_.CreateAnd(acc0, mask40)), builder_.getInt32Ty(), "ac0");
    store_cpu_u32(offsetof(CpuState, az), az_val);
    store_cpu_u32(offsetof(CpuState, an), an_val);
    store_ac0(ac0_val);
    return true;
}

// CC = A0 <= A1
bool LiftVisitor::decode_CCflag_A0_LE_A1() {
    if (parallel_slot_ > 0) return false;

    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    auto* diff = builder_.CreateSub(acc0, acc1, "diff");
    store_cc(builder_.CreateZExt(builder_.CreateICmpSLE(acc0, acc1), builder_.getInt32Ty(), "cc"));
    auto* az_val = builder_.CreateZExt(builder_.CreateICmpEQ(diff, builder_.getInt64(0)), builder_.getInt32Ty(), "az");
    auto* an_val = builder_.CreateZExt(builder_.CreateICmpSLT(diff, builder_.getInt64(0)), builder_.getInt32Ty(), "an");
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* ac0_val = builder_.CreateZExt(
        builder_.CreateICmpULE(builder_.CreateAnd(acc1, mask40),
                               builder_.CreateAnd(acc0, mask40)), builder_.getInt32Ty(), "ac0");
    store_cpu_u32(offsetof(CpuState, az), az_val);
    store_cpu_u32(offsetof(CpuState, an), an_val);
    store_ac0(ac0_val);
    return true;
}

// Push/Pop
bool LiftVisitor::decode_PushReg(uint16_t g, uint16_t r) {
    if (parallel_slot_ > 0) return false;

    // allreg: not reserved; also SP (g==1, r==6) cannot be pushed
    bool is_reserved = (g == 4 && (r == 4 || r == 5)) || (g == 5);
    if (is_reserved || (g == 1 && r == 6)) return false;
    // [--SP] = reg
    auto* ft_sup = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy()}, false);
    auto* sp = load_preg(6, "sp"); // SP = P6 = dpregs[14]
    auto* new_sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp_dec");
    store_preg(6, new_sp);
    Value* push_val;
    if (g == 4 && r == 6) {
        push_val = emit_astat_compose();
    } else {
        push_val = load_cpu_u32(allreg_offset(g, r), "push_val");
    }
    emit_mem_write("mem_write32", new_sp, push_val);
    if (g == 7) {
        if (r == 3) {
            call_extern("cec_push_reti", ft_sup, {cpu_ptr_});
            check_cec_pending_ = true;
        } else {
            call_extern("cec_check_sup", ft_sup, {cpu_ptr_});
        }
        // If the call raised an exception (did_jump=true), exit the BB now
        // so subsequent instructions in this BB don't execute.
        emit_did_jump_exit(true);
    }
    return true;
}

bool LiftVisitor::decode_PopReg(uint16_t g, uint16_t r) {
    if (parallel_slot_ > 0) return false;

    // mostreg: not dreg (g==0), not preg (g==1), not reserved
    bool is_reserved = (g == 4 && (r == 4 || r == 5)) || (g == 5);
    bool mostreg = !(g == 0 || g == 1 || is_reserved);
    if (!mostreg) return false;
    // reg = [SP++]
    auto* ft_sup = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy()}, false);
    auto* sp = load_preg(6, "sp");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
    store_preg(6, builder_.CreateAdd(sp, builder_.getInt32(4), "sp_inc"));
    if (g == 4 && r == 6) {
        emit_astat_decompose(val);
    } else {
        store_cpu_u32(allreg_offset(g, r), val);
    }
    if (g == 7) {
        if (r == 3) {
            call_extern("cec_pop_reti", ft_sup, {cpu_ptr_});
        } else if (r == 0) {
            // USP=[SP++]: valid in supervisor mode, raises VEC_ILGAL_I (0x22)
            // in user mode. Emit IR conditional so supervisor path is a no-op.
            auto& ctx2 = builder_.getContext();
            auto* fn2   = builder_.GetInsertBlock()->getParent();
            auto* bb_user = BasicBlock::Create(ctx2, "usp_pop_user", fn2);
            auto* bb_cont = BasicBlock::Create(ctx2, "usp_pop_cont", fn2);
            auto* ft_um = FunctionType::get(builder_.getInt1Ty(), {}, false);
            auto* is_um = call_extern("cec_is_user_mode", ft_um, {});
            builder_.CreateCondBr(is_um, bb_user, bb_cont);
            builder_.SetInsertPoint(bb_user);
            auto* ft_exc = FunctionType::get(builder_.getVoidTy(),
                {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
            call_extern("cec_exception", ft_exc, {cpu_ptr_, builder_.getInt32(VEC_ILGAL_I)});
            builder_.CreateRetVoid();
            builder_.SetInsertPoint(bb_cont);
        } else {
            call_extern("cec_check_sup", ft_sup, {cpu_ptr_});
        }
        emit_did_jump_exit(true);
    }
    return true;
}

// LDST_ST_32_ind: [Pp] = Rd
bool LiftVisitor::decode_LDST_ST_32_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    return true;
}

// LDST_ST_8: B[Pp++] = Rd
bool LiftVisitor::decode_LDST_ST_8(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* trunc_byte = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt8Ty(), "trunc_byte");
    emit_mem_write("mem_write8", addr, trunc_byte);
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(1), "ptr_inc"));
    return true;
}

// LDSTii_ST_32: [Pp + off*4] = Rd
bool LiftVisitor::decode_LDSTii_ST_32(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 4), "addr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    return true;
}

// CALLa_CALL: RETS = hwloop_next_pc; PC = PC + sext25(addr)*2
bool LiftVisitor::decode_CALLa_CALL(uint32_t addr) {
    if (parallel_slot_ > 0) return false;

    int32_t soff = signextend<24>(addr);
    uint32_t target = current_pc + static_cast<uint32_t>(soff << 1);
    store_cpu_u32(offsetof(CpuState, rets), emit_hwloop_next_pc(4));
    emit_jump_imm(target);
    return true;
}

// LDIMMhalf: load immediate into register halves
bool LiftVisitor::decode_LDIMMhalf_low(uint32_t g, uint32_t r, uint16_t hword) {
    if (parallel_slot_ > 0) return false;

    // reg.L = hword (replace low 16 bits, keep high)
    size_t off = allreg_offset(g, r);
    auto* old_val = load_cpu_u32(off, "old");
    auto* cleared = builder_.CreateAnd(old_val, builder_.getInt32(0xFFFF0000u), "hi_kept");
    auto* new_val = builder_.CreateOr(cleared, builder_.getInt32(hword), "with_lo");
    store_cpu_u32(off, new_val);
    return true;
}

bool LiftVisitor::decode_LDIMMhalf_high(uint32_t g, uint32_t r, uint16_t hword) {
    if (parallel_slot_ > 0) return false;

    size_t off = allreg_offset(g, r);
    auto* old_val = load_cpu_u32(off, "old");
    auto* cleared = builder_.CreateAnd(old_val, builder_.getInt32(0x0000FFFFu), "lo_kept");
    auto* new_val = builder_.CreateOr(cleared, builder_.getInt32(static_cast<uint32_t>(hword) << 16), "with_hi");
    store_cpu_u32(off, new_val);
    return true;
}

bool LiftVisitor::decode_LDIMMhalf_full(uint32_t g, uint32_t r, uint16_t hword) {
    if (parallel_slot_ > 0) return false;

    // reg = zero_extend(hword)
    size_t off = allreg_offset(g, r);
    store_cpu_u32(off, builder_.getInt32(static_cast<uint32_t>(hword)));
    return true;
}

bool LiftVisitor::decode_LDIMMhalf_full_sext(uint32_t g, uint32_t r, uint16_t hword) {
    if (parallel_slot_ > 0) return false;

    int32_t sval = static_cast<int32_t>(static_cast<int16_t>(hword));
    size_t off = allreg_offset(g, r);
    store_cpu_u32(off, builder_.getInt32(static_cast<uint32_t>(sval)));
    return true;
}

// LoopSetup
bool LiftVisitor::decode_LoopSetup_LC0_P(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (parallel_slot_ > 0) return false;

    if (reg > 7) return false;
    // LT0 = PC + soffset*2, LB0 = PC + eoffset*2, LC0 = Preg
    // soffset (4-bit) and eoffset (10-bit) are unsigned per reference pcrel4/lppcrel10
    uint32_t lt_val = current_pc + soffset * 2u;
    uint32_t lb_val = current_pc + eoffset * 2u;

    store_cpu_u32(offsetof(CpuState, lt[0]), builder_.getInt32(lt_val & ~1u)); // LT clears LSB
    store_cpu_u32(offsetof(CpuState, lb[0]), builder_.getInt32(lb_val));

    auto* lc = load_preg(reg, "lc_preg");
    store_cpu_u32(offsetof(CpuState, lc[0]), lc);
    // When the loop top immediately follows the lsetup, jump there directly.
    // Otherwise jump to the next instruction so the pre-loop prologue executes;
    // the translator will end the prologue BB just before lt_val.
    uint32_t next_insn = current_pc + 4; // lsetup is a 32-bit instruction
    emit_jump_imm((lt_val == next_insn) ? lt_val : next_insn);
    return true;
}

bool LiftVisitor::decode_LoopSetup_LC1_P(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (parallel_slot_ > 0) return false;

    if (reg > 7) return false;
    uint32_t lt_val = current_pc + soffset * 2u;
    uint32_t lb_val = current_pc + eoffset * 2u;

    store_cpu_u32(offsetof(CpuState, lt[1]), builder_.getInt32(lt_val & ~1u));
    store_cpu_u32(offsetof(CpuState, lb[1]), builder_.getInt32(lb_val));

    auto* lc = load_preg(reg, "lc_preg");
    store_cpu_u32(offsetof(CpuState, lc[1]), lc);
    uint32_t next_insn = current_pc + 4;
    emit_jump_imm((lt_val == next_insn) ? lt_val : next_insn);
    return true;
}

// dsp32alu ACC_LOAD variants — A0 = R0, A0.W = R0, A0.X = R0, etc.
// AOP0: s=0, HL=0 → A0 = Dreg (full load, sign extend to ax)
bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP0_full(uint32_t M, uint32_t HL, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A0 = R<src0> — sign-extends to 40 bits
    auto* val = load_dreg(src0, "src");
    store_cpu_u32(offsetof(CpuState, aw[0]), val);
    // ax = sign bit of val: if val >> 31 then 0xFF else 0x00
    auto* sign = builder_.CreateLShr(val, 31, "sign");
    auto* ax = builder_.CreateSelect(
        builder_.CreateICmpNE(sign, builder_.getInt32(0)),
        builder_.getInt32(0xFF), builder_.getInt32(0), "ax");
    store_cpu_u32(offsetof(CpuState, ax[0]), ax);
    return true;
}

// AOP0_lo: A0.L = Rs.L
bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP0_lo(uint32_t M, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A0.L = Rs.L: update lower 16 bits of aw[0], keep upper 16 bits
    auto* val = load_dreg(src0, "src");
    auto* old_aw = load_cpu_u32(offsetof(CpuState, aw[0]), "old_aw");
    auto* new_aw = builder_.CreateOr(
        builder_.CreateAnd(old_aw, builder_.getInt32(0xFFFF0000u)),
        builder_.CreateAnd(val, builder_.getInt32(0x0000FFFFu)), "new_aw");
    store_cpu_u32(offsetof(CpuState, aw[0]), new_aw);
    return true;
}

// AOP0_hi: A0.H = Rs.H — update upper 16 bits of aw[0], keep lower 16 bits
bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP0_hi(uint32_t M, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* val = load_dreg(src0, "src");
    auto* old_aw = load_cpu_u32(offsetof(CpuState, aw[0]), "old_aw");
    auto* new_aw = builder_.CreateOr(
        builder_.CreateAnd(val, builder_.getInt32(0xFFFF0000u)),
        builder_.CreateAnd(old_aw, builder_.getInt32(0x0000FFFFu)), "new_aw");
    store_cpu_u32(offsetof(CpuState, aw[0]), new_aw);
    return true;
}

// AOP1: sop=01, s=0 → A0.X = R<src0> (write A0 extension byte)
bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP1(uint32_t M, uint32_t HL, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* val = load_dreg(src0, "src");
    auto* masked = builder_.CreateAnd(val, builder_.getInt32(0xFF), "ax_mask");
    store_cpu_u32(offsetof(CpuState, ax[0]), masked);
    return true;
}

// AOP2: s=1, HL=0 → A1 = R<src0> (full load to A1)
bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP2_full(uint32_t M, uint32_t HL, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* val = load_dreg(src0, "src");
    store_cpu_u32(offsetof(CpuState, aw[1]), val);
    auto* sign = builder_.CreateLShr(val, 31, "sign");
    auto* ax = builder_.CreateSelect(
        builder_.CreateICmpNE(sign, builder_.getInt32(0)),
        builder_.getInt32(0xFF), builder_.getInt32(0), "ax");
    store_cpu_u32(offsetof(CpuState, ax[1]), ax);
    return true;
}

bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP2_lo(uint32_t M, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A1.L = Rs.L: update lower 16 bits of aw[1], keep upper 16 bits
    auto* val = load_dreg(src0, "src");
    auto* old_aw = load_cpu_u32(offsetof(CpuState, aw[1]), "old_aw");
    auto* new_aw = builder_.CreateOr(
        builder_.CreateAnd(old_aw, builder_.getInt32(0xFFFF0000u)),
        builder_.CreateAnd(val, builder_.getInt32(0x0000FFFFu)), "new_aw");
    store_cpu_u32(offsetof(CpuState, aw[1]), new_aw);
    return true;
}

bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP2_hi(uint32_t M, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A1.H = Rs.H: update upper 16 bits of aw[1], keep lower 16 bits
    auto* val = load_dreg(src0, "src");
    auto* old_aw = load_cpu_u32(offsetof(CpuState, aw[1]), "old_aw");
    auto* new_aw = builder_.CreateOr(
        builder_.CreateAnd(val, builder_.getInt32(0xFFFF0000u)),
        builder_.CreateAnd(old_aw, builder_.getInt32(0x0000FFFFu)), "new_aw");
    store_cpu_u32(offsetof(CpuState, aw[1]), new_aw);
    return true;
}

// AOP3: s=1, HL=1 → A1.X = R<src0>
bool LiftVisitor::decode_dsp32alu_ACC_LOAD_AOP3(uint32_t M, uint32_t HL, uint32_t x,
    uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* val = load_dreg(src0, "src");
    auto* masked = builder_.CreateAnd(val, builder_.getInt32(0xFF), "ax_mask");
    store_cpu_u32(offsetof(CpuState, ax[1]), masked);
    return true;
}

// =============================================
// STUB implementations for all unneeded opcodes
// =============================================

#define STUB_0(name) bool LiftVisitor::decode_##name() { return unimplemented(#name); }
#define STUB_1(name, T1) bool LiftVisitor::decode_##name(T1) { return unimplemented(#name); }
#define STUB_2(name, T1, T2) bool LiftVisitor::decode_##name(T1, T2) { return unimplemented(#name); }
#define STUB_3(name, T1, T2, T3) bool LiftVisitor::decode_##name(T1, T2, T3) { return unimplemented(#name); }
#define STUB_4(name, T1, T2, T3, T4) bool LiftVisitor::decode_##name(T1, T2, T3, T4) { return unimplemented(#name); }
#define STUB_5(name, T1, T2, T3, T4, T5) bool LiftVisitor::decode_##name(T1, T2, T3, T4, T5) { return unimplemented(#name); }
#define STUB_6(name, T1, T2, T3, T4, T5, T6) bool LiftVisitor::decode_##name(T1, T2, T3, T4, T5, T6) { return unimplemented(#name); }
#define STUB_7(name, T1, T2, T3, T4, T5, T6, T7) bool LiftVisitor::decode_##name(T1, T2, T3, T4, T5, T6, T7) { return unimplemented(#name); }
#define STUB_8(name, T1, T2, T3, T4, T5, T6, T7, T8) bool LiftVisitor::decode_##name(T1, T2, T3, T4, T5, T6, T7, T8) { return unimplemented(#name); }
#define STUB_9(name, T1, T2, T3, T4, T5, T6, T7, T8, T9) bool LiftVisitor::decode_##name(T1, T2, T3, T4, T5, T6, T7, T8, T9) { return unimplemented(#name); }
#define STUB_10(name, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10) bool LiftVisitor::decode_##name(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10) { return unimplemented(#name); }

// ProgCtrl stubs
bool LiftVisitor::decode_ProgCtrl_RTI() {
    if (parallel_slot_ > 0) return false;

    // cec_return_rti(CpuState*) → uint32_t newpc
    auto* ft = FunctionType::get(builder_.getInt32Ty(),
        {builder_.getInt8PtrTy()}, false);
    auto* newpc = call_extern("cec_return_rti", ft, {cpu_ptr_});
    emit_jump(newpc);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_RTX() {
    if (parallel_slot_ > 0) return false;

    // cec_return_rtx(CpuState*) → uint32_t newpc
    auto* ft = FunctionType::get(builder_.getInt32Ty(),
        {builder_.getInt8PtrTy()}, false);
    auto* newpc = call_extern("cec_return_rtx", ft, {cpu_ptr_});
    emit_jump(newpc);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_RTN() {
    if (parallel_slot_ > 0) return false;

    // cec_return_rtn(CpuState*) → uint32_t newpc
    auto* ft = FunctionType::get(builder_.getInt32Ty(),
        {builder_.getInt8PtrTy()}, false);
    auto* newpc = call_extern("cec_return_rtn", ft, {cpu_ptr_});
    emit_jump(newpc);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_RTE() {
    if (parallel_slot_ > 0) return false;

    // cec_return_rte(CpuState*) → uint32_t newpc
    auto* ft = FunctionType::get(builder_.getInt32Ty(),
        {builder_.getInt8PtrTy()}, false);
    auto* newpc = call_extern("cec_return_rte", ft, {cpu_ptr_});
    emit_jump(newpc);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_IDLE() {
    if (parallel_slot_ > 0) return false;

    // In single-threaded user-mode emulation, IDLE has no wake-up sources;
    // treat as a sync NOP (matches CSYNC/SSYNC handling).
    return true;
}
bool LiftVisitor::decode_ProgCtrl_CLI(uint16_t d) {
    if (parallel_slot_ > 0) return false;

    // CLI Rd: mask interrupts, store old IMASK in Rd
    auto* ft = FunctionType::get(builder_.getInt32Ty(),
        {builder_.getInt8PtrTy()}, false);
    auto* old_imask = call_extern("cec_cli", ft, {cpu_ptr_});
    store_dreg(d, old_imask);
    // CLI may raise VEC_ILL_RES if in user mode (sets did_jump).
    check_did_jump_ = true;
    return true;
}
bool LiftVisitor::decode_ProgCtrl_STI(uint16_t d) {
    if (parallel_slot_ > 0) return false;

    // STI Rd: restore IMASK from Rd
    auto* mask = load_dreg(d, "sti_mask");
    auto* ft = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
    call_extern("cec_sti", ft, {cpu_ptr_, mask});
    check_cec_pending_ = true;
    // STI may dispatch a pending interrupt or raise VEC_ILL_RES (sets did_jump).
    check_did_jump_ = true;
    return true;
}
bool LiftVisitor::decode_ProgCtrl_JUMP_PREG(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    auto* target = load_preg(p, "jump_target");
    emit_jump(target);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_CALL_PREG(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    store_cpu_u32(offsetof(CpuState, rets), emit_hwloop_next_pc(2));
    auto* target = load_preg(p, "call_target");
    emit_jump(target);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_CALL_PC_PREG(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    // CALL (PC + Preg): RETS = hwloop_next_pc; PC = PC + Preg
    store_cpu_u32(offsetof(CpuState, rets), emit_hwloop_next_pc(2));
    auto* preg   = load_preg(p, "preg");
    auto* pc     = builder_.getInt32(current_pc);
    auto* target = builder_.CreateAdd(pc, preg, "call_target");
    emit_jump(target);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_JUMP_PC_PREG(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    auto* preg   = load_preg(p, "preg");
    auto* pc     = builder_.getInt32(current_pc);
    auto* target = builder_.CreateAdd(pc, preg, "jump_target");
    emit_jump(target);
    return true;
}
bool LiftVisitor::decode_ProgCtrl_RAISE(uint16_t imm) {
    if (parallel_slot_ > 0) return false;

    auto* ft = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
    call_extern("cec_raise", ft, {cpu_ptr_, builder_.getInt32(imm)});
    check_cec_pending_ = true;
    // Terminate the BB: cec_raise may have redirected cpu->pc
    terminated_ = true;
    return true;
}
bool LiftVisitor::decode_ProgCtrl_TESTSET(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    // TESTSET(Preg): CC = (B[Preg] == 0); B[Preg] |= 0x80
    auto* addr = load_preg(p, "ptr");
    auto* byte_val = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);
    store_cc(builder_.CreateZExt(
        builder_.CreateICmpEQ(byte_val, builder_.getInt8(0), "testset_z"),
        builder_.getInt32Ty(), "cc"));
    emit_mem_write("mem_write8", addr,
        builder_.CreateOr(byte_val, builder_.getInt8(0x80), "set_msb"));
    return true;
}

// PushPopMultiple
bool LiftVisitor::decode_PushMultiple_RP(uint16_t ddd, uint16_t ppp) {
    if (parallel_slot_ > 0) return false;

    if (ppp > 5) return false;
    auto* sp = load_preg(6, "sp");
    // Reference pushes D-regs ascending (dr to 7), then P-regs ascending (pr to 5)
    for (int i = (int)ddd; i <= 7; i++) {
        sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp_dec");
        emit_mem_write("mem_write32", sp, load_dreg(i, "r"));
    }
    for (int i = (int)ppp; i <= 5; i++) {
        sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp_dec");
        emit_mem_write("mem_write32", sp, load_preg(i, "p"));
    }
    store_preg(6, sp);
    return true;
}
bool LiftVisitor::decode_PushMultiple_R(uint16_t ddd) {
    if (parallel_slot_ > 0) return false;

    auto* sp = load_preg(6, "sp");
    for (int i = (int)ddd; i <= 7; i++) {
        sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp_dec");
        emit_mem_write("mem_write32", sp, load_dreg(i, "r"));
    }
    store_preg(6, sp);
    return true;
}
bool LiftVisitor::decode_PushMultiple_P(uint16_t ppp) {
    if (parallel_slot_ > 0) return false;

    if (ppp > 5) return false;
    auto* sp = load_preg(6, "sp");
    for (int i = (int)ppp; i <= 5; i++) {
        sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp_dec");
        emit_mem_write("mem_write32", sp, load_preg(i, "p"));
    }
    store_preg(6, sp);
    return true;
}
bool LiftVisitor::decode_PopMultiple_RP(uint16_t ddd, uint16_t ppp) {
    if (parallel_slot_ > 0) return false;

    if (ppp > 5) return false;
    auto* sp = load_preg(6, "sp");
    // Reference pops P-regs descending (5 to pr), then D-regs descending (7 to dr)
    for (int i = 5; i >= (int)ppp; i--) {
        auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
        store_preg(i, val);
        sp = builder_.CreateAdd(sp, builder_.getInt32(4), "sp_inc");
    }
    for (int i = 7; i >= (int)ddd; i--) {
        auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
        store_dreg(i, val);
        sp = builder_.CreateAdd(sp, builder_.getInt32(4), "sp_inc");
    }
    store_preg(6, sp);
    return true;
}
bool LiftVisitor::decode_PopMultiple_R(uint16_t ddd) {
    if (parallel_slot_ > 0) return false;

    auto* sp = load_preg(6, "sp");
    for (int i = 7; i >= (int)ddd; i--) {
        auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
        store_dreg(i, val);
        sp = builder_.CreateAdd(sp, builder_.getInt32(4), "sp_inc");
    }
    store_preg(6, sp);
    return true;
}
bool LiftVisitor::decode_PopMultiple_P(uint16_t ppp) {
    if (parallel_slot_ > 0) return false;

    if (ppp > 5) return false;
    auto* sp = load_preg(6, "sp");
    for (int i = 5; i >= (int)ppp; i--) {
        auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
        store_preg(i, val);
        sp = builder_.CreateAdd(sp, builder_.getInt32(4), "sp_inc");
    }
    store_preg(6, sp);
    return true;
}

// CC2dreg
bool LiftVisitor::decode_CC2dreg_Read(uint16_t reg) {
    if (parallel_slot_ > 0) return false;

    auto* cc_val = load_cpu_u32(offsetof(CpuState, cc), "cc");
    store_dreg(reg, cc_val);
    return true;
}
bool LiftVisitor::decode_CC2dreg_Write(uint16_t reg) {
    if (parallel_slot_ > 0) return false;

    // CC = (Rs != 0)
    auto* val = load_dreg(reg, "rs");
    auto* cmp = builder_.CreateICmpNE(val, builder_.getInt32(0), "cc_bool");
    store_cc(builder_.CreateZExt(cmp, builder_.getInt32Ty(), "cc_val"));
    return true;
}
bool LiftVisitor::decode_CC2dreg_Negate() {
    if (parallel_slot_ > 0) return false;

    auto* cc  = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* neg = builder_.CreateICmpEQ(cc, builder_.getInt32(0), "cc_not");
    store_cc(builder_.CreateZExt(neg, builder_.getInt32Ty(), "cc_val"));
    return true;
}

// CaCTRL — no real cache simulation; non-pp variants are NOPs,
// pp variants post-increment the pointer register by one cache line (32 bytes).
bool LiftVisitor::decode_CaCTRL_PREFETCH(uint16_t p)  {
    if (parallel_slot_ > 0) return false;
    (void)p; return true;
}
bool LiftVisitor::decode_CaCTRL_FLUSHINV(uint16_t p)  {
    if (parallel_slot_ > 0) return false;
    (void)p; return true;
}
bool LiftVisitor::decode_CaCTRL_FLUSH(uint16_t p)     {
    if (parallel_slot_ > 0) return false;
    (void)p; return true;
}
bool LiftVisitor::decode_CaCTRL_IFLUSH(uint16_t p)    {
    if (parallel_slot_ > 0) return false;
    (void)p; return true;
}

bool LiftVisitor::decode_CaCTRL_PREFETCH_pp(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    auto* pd = load_preg(p, "pd");
    store_preg(p, builder_.CreateAdd(pd, builder_.getInt32(32), "pd_inc"));
    return true;
}
bool LiftVisitor::decode_CaCTRL_FLUSHINV_pp(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    auto* pd = load_preg(p, "pd");
    store_preg(p, builder_.CreateAdd(pd, builder_.getInt32(32), "pd_inc"));
    return true;
}
bool LiftVisitor::decode_CaCTRL_FLUSH_pp(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    auto* pd = load_preg(p, "pd");
    store_preg(p, builder_.CreateAdd(pd, builder_.getInt32(32), "pd_inc"));
    return true;
}
bool LiftVisitor::decode_CaCTRL_IFLUSH_pp(uint16_t p) {
    if (parallel_slot_ > 0) return false;

    auto* pd = load_preg(p, "pd");
    store_preg(p, builder_.CreateAdd(pd, builder_.getInt32(32), "pd_inc"));
    return true;
}

// CC2stat
bool LiftVisitor::decode_CC2stat_CC_EQ_ASTAT(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* astat   = emit_astat_compose();
    auto* shifted = builder_.CreateLShr(astat, builder_.getInt32(cbit), "shifted");
    auto* bit     = builder_.CreateAnd(shifted, builder_.getInt32(1), "bit");
    store_cc(bit);
    return true;
}
bool LiftVisitor::decode_CC2stat_CC_OR_ASTAT(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* astat = emit_astat_compose();
    auto* pval  = builder_.CreateAnd(builder_.CreateLShr(astat, builder_.getInt32(cbit)), builder_.getInt32(1), "pval");
    auto* cc    = load_cpu_u32(offsetof(CpuState, cc), "cc");
    store_cc(builder_.CreateOr(cc, pval, "cc_or"));
    return true;
}
bool LiftVisitor::decode_CC2stat_CC_AND_ASTAT(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* astat = emit_astat_compose();
    auto* pval  = builder_.CreateAnd(builder_.CreateLShr(astat, builder_.getInt32(cbit)), builder_.getInt32(1), "pval");
    auto* cc    = load_cpu_u32(offsetof(CpuState, cc), "cc");
    store_cc(builder_.CreateAnd(cc, pval, "cc_and"));
    return true;
}
bool LiftVisitor::decode_CC2stat_CC_XOR_ASTAT(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* astat = emit_astat_compose();
    auto* pval  = builder_.CreateAnd(builder_.CreateLShr(astat, builder_.getInt32(cbit)), builder_.getInt32(1), "pval");
    auto* cc    = load_cpu_u32(offsetof(CpuState, cc), "cc");
    store_cc(builder_.CreateXor(cc, pval, "cc_xor"));
    return true;
}
bool LiftVisitor::decode_CC2stat_ASTAT_EQ_CC(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* cc      = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* astat   = emit_astat_compose();
    auto* mask    = builder_.getInt32(~(1u << cbit));
    auto* cleared = builder_.CreateAnd(astat, mask, "cleared");
    auto* shifted = builder_.CreateShl(cc, builder_.getInt32(cbit), "shifted");
    emit_astat_decompose(builder_.CreateOr(cleared, shifted, "new_astat"));
    return true;
}
bool LiftVisitor::decode_CC2stat_ASTAT_OR_CC(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* cc      = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* astat   = emit_astat_compose();
    auto* pval    = builder_.CreateOr(builder_.CreateAnd(builder_.CreateLShr(astat, builder_.getInt32(cbit)), builder_.getInt32(1)), cc, "pval");
    auto* mask    = builder_.getInt32(~(1u << cbit));
    auto* cleared = builder_.CreateAnd(astat, mask, "cleared");
    auto* shifted = builder_.CreateShl(pval, builder_.getInt32(cbit), "shifted");
    emit_astat_decompose(builder_.CreateOr(cleared, shifted, "new_astat"));
    return true;
}
bool LiftVisitor::decode_CC2stat_ASTAT_AND_CC(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* cc      = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* astat   = emit_astat_compose();
    auto* pval    = builder_.CreateAnd(builder_.CreateAnd(builder_.CreateLShr(astat, builder_.getInt32(cbit)), builder_.getInt32(1)), cc, "pval");
    auto* mask    = builder_.getInt32(~(1u << cbit));
    auto* cleared = builder_.CreateAnd(astat, mask, "cleared");
    auto* shifted = builder_.CreateShl(pval, builder_.getInt32(cbit), "shifted");
    emit_astat_decompose(builder_.CreateOr(cleared, shifted, "new_astat"));
    return true;
}
bool LiftVisitor::decode_CC2stat_ASTAT_XOR_CC(uint16_t cbit) {
    if (parallel_slot_ > 0) return false;

    if (cbit == 5) return false;
    auto* cc      = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* astat   = emit_astat_compose();
    auto* pval    = builder_.CreateXor(builder_.CreateAnd(builder_.CreateLShr(astat, builder_.getInt32(cbit)), builder_.getInt32(1)), cc, "pval");
    auto* mask    = builder_.getInt32(~(1u << cbit));
    auto* cleared = builder_.CreateAnd(astat, mask, "cleared");
    auto* shifted = builder_.CreateShl(pval, builder_.getInt32(cbit), "shifted");
    emit_astat_decompose(builder_.CreateOr(cleared, shifted, "new_astat"));
    return true;
}

// ccMV
bool LiftVisitor::decode_ccMV_IF_NOT(uint16_t d, uint16_t s, uint16_t dst, uint16_t src) {
    if (parallel_slot_ > 0) return false;

    auto* cc    = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* cond  = builder_.CreateICmpEQ(cc, builder_.getInt32(0), "cc_false");
    size_t src_off = allreg_offset(s, src);
    size_t dst_off = allreg_offset(d, dst);
    auto* src_val = load_cpu_u32(src_off, "src_val");
    auto* old_dst = load_cpu_u32(dst_off, "old_dst");
    auto* result  = builder_.CreateSelect(cond, src_val, old_dst, "ccmv");
    store_cpu_u32(dst_off, result);
    return true;
}
bool LiftVisitor::decode_ccMV_IF(uint16_t d, uint16_t s, uint16_t dst, uint16_t src) {
    if (parallel_slot_ > 0) return false;

    auto* cc    = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* cond  = builder_.CreateICmpNE(cc, builder_.getInt32(0), "cc_true");
    size_t src_off = allreg_offset(s, src);
    size_t dst_off = allreg_offset(d, dst);
    auto* src_val = load_cpu_u32(src_off, "src_val");
    auto* old_dst = load_cpu_u32(dst_off, "old_dst");
    auto* result  = builder_.CreateSelect(cond, src_val, old_dst, "ccmv");
    store_cpu_u32(dst_off, result);
    return true;
}

// CCflag remaining
bool LiftVisitor::decode_CCflag_LT_U(bool preg, uint16_t y, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = preg ? load_preg(y, "py") : load_dreg(y, "ry");

    auto* cc_val = builder_.CreateZExt(builder_.CreateICmpULT(src, dst),
                                       builder_.getInt32Ty(), "cc");
    store_cc(cc_val);

    if (!preg) {
        auto* az  = builder_.CreateZExt(builder_.CreateICmpEQ(src, dst),
                                        builder_.getInt32Ty(), "az");
        auto* an  = builder_.CreateZExt(builder_.CreateICmpUGT(dst, src),
                                        builder_.getInt32Ty(), "an");
        auto* ac0 = builder_.CreateZExt(builder_.CreateICmpULE(dst, src),
                                        builder_.getInt32Ty(), "ac0");
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}
bool LiftVisitor::decode_CCflag_LE_U(bool preg, uint16_t y, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = preg ? load_preg(y, "py") : load_dreg(y, "ry");

    auto* cc_val = builder_.CreateZExt(builder_.CreateICmpULE(src, dst),
                                       builder_.getInt32Ty(), "cc");
    store_cc(cc_val);

    if (!preg) {
        auto* az  = builder_.CreateZExt(builder_.CreateICmpEQ(src, dst),
                                        builder_.getInt32Ty(), "az");
        auto* an  = builder_.CreateZExt(builder_.CreateICmpUGT(dst, src),
                                        builder_.getInt32Ty(), "an");
        auto* ac0 = builder_.CreateZExt(builder_.CreateICmpULE(dst, src),
                                        builder_.getInt32Ty(), "ac0");
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}
bool LiftVisitor::decode_CCflag_EQ_imm(bool preg, uint16_t i, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    // CC = Rx == imm3  (src - sign_extend_3(i); CC = AZ)
    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = builder_.getInt32(static_cast<uint32_t>(signextend<3>(i)));
    auto [az, an, ac0] = emit_cc_cmp(src, dst);
    store_cc(az);  // opc=0 → CC = AZ
    if (!preg) {   // P-reg compares only update CC, not ASTAT
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}
bool LiftVisitor::decode_CCflag_LT_imm(bool preg, uint16_t i, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = builder_.getInt32(static_cast<uint32_t>(signextend<3>(i)));
    auto [az, an, ac0] = emit_cc_cmp(src, dst);
    store_cc(an);  // opc=1 → CC = AN
    if (!preg) {
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}
bool LiftVisitor::decode_CCflag_LE_imm(bool preg, uint16_t i, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = builder_.getInt32(static_cast<uint32_t>(signextend<3>(i)));
    auto [az, an, ac0] = emit_cc_cmp(src, dst);
    store_cc(builder_.CreateOr(an, az, "le"));  // opc=2 → CC = AN || AZ
    if (!preg) {
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}
bool LiftVisitor::decode_CCflag_LT_U_imm(bool preg, uint16_t i, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = builder_.getInt32(static_cast<uint32_t>(i));  // uimm3: zero-extend

    auto* cc_val = builder_.CreateZExt(builder_.CreateICmpULT(src, dst),
                                       builder_.getInt32Ty(), "cc");
    store_cc(cc_val);

    if (!preg) {
        auto* az  = builder_.CreateZExt(builder_.CreateICmpEQ(src, dst),
                                        builder_.getInt32Ty(), "az");
        auto* an  = builder_.CreateZExt(builder_.CreateICmpUGT(dst, src),
                                        builder_.getInt32Ty(), "an");
        auto* ac0 = builder_.CreateZExt(builder_.CreateICmpULE(dst, src),
                                        builder_.getInt32Ty(), "ac0");
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}
bool LiftVisitor::decode_CCflag_LE_U_imm(bool preg, uint16_t i, uint16_t x) {
    if (parallel_slot_ > 0) return false;

    Value* src = preg ? load_preg(x, "px") : load_dreg(x, "rx");
    Value* dst = builder_.getInt32(static_cast<uint32_t>(i));  // uimm3: zero-extend

    auto* cc_val = builder_.CreateZExt(builder_.CreateICmpULE(src, dst),
                                       builder_.getInt32Ty(), "cc");
    store_cc(cc_val);

    if (!preg) {
        auto* az  = builder_.CreateZExt(builder_.CreateICmpEQ(src, dst),
                                        builder_.getInt32Ty(), "az");
        auto* an  = builder_.CreateZExt(builder_.CreateICmpUGT(dst, src),
                                        builder_.getInt32Ty(), "an");
        auto* ac0 = builder_.CreateZExt(builder_.CreateICmpULE(dst, src),
                                        builder_.getInt32Ty(), "ac0");
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_ac0(ac0);
    }
    return true;
}

// ALU2op
// ALU2op_ASHIFT_RIGHT: Rd >>>= Rs  (arithmetic right shift)
// Semantics: ashiftrt(cpu, Rd, Rs, 32) from refs/bfin-sim.c.
// Clamp shift to [0,31] (LLVM AShr >= 32 is poison; clamping to 31 gives all-sign-bits, same as hardware >= 32).
// Flags: AZ/AN set, V=0.  AC0 not touched.
bool LiftVisitor::decode_ALU2op_ASHIFT_RIGHT(uint16_t src, uint16_t dst) {
    auto* rd  = load_dreg(dst, "rd");
    auto* rs  = load_dreg(src, "rs");
    auto* clamp = emit_umin(rs, builder_.getInt32(31));
    auto* result = builder_.CreateAShr(rd, clamp, "asr");
    store_dreg(dst, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
// ALU2op_LSHIFT_RIGHT: Rd >>= Rs  (logical right shift)
// Semantics: if Rs > 0x1F, result = 0; else lshiftrt(cpu, Rd, Rs, 32).
// Flags: AZ/AN set, V=0.  AC0 not touched.
bool LiftVisitor::decode_ALU2op_LSHIFT_RIGHT(uint16_t src, uint16_t dst) {
    auto* rd  = load_dreg(dst, "rd");
    auto* rs  = load_dreg(src, "rs");
    auto* in_range = builder_.CreateICmpULE(rs, builder_.getInt32(31));
    auto* shifted  = builder_.CreateLShr(rd, rs, "lsr");
    auto* result   = builder_.CreateSelect(in_range, shifted, builder_.getInt32(0), "lsr_res");
    store_dreg(dst, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
// ALU2op_LSHIFT_LEFT: Rd <<= Rs  (logical left shift, no saturation)
// Semantics: lshift(cpu, Rd, Rs, 32, saturate=0, overflow=0).
// With overflow=0: V is always 0.  Result = 0 if Rs >= 32.
// Flags: AZ/AN set, V=0.  AC0 not touched.
bool LiftVisitor::decode_ALU2op_LSHIFT_LEFT(uint16_t src, uint16_t dst) {
    auto* rd  = load_dreg(dst, "rd");
    auto* rs  = load_dreg(src, "rs");
    auto* in_range = builder_.CreateICmpULE(rs, builder_.getInt32(31));
    auto* shifted  = builder_.CreateShl(rd, rs, "lsl");
    auto* result   = builder_.CreateSelect(in_range, shifted, builder_.getInt32(0), "lsl_res");
    store_dreg(dst, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_ALU2op_MUL(uint16_t src, uint16_t dst) {
    auto* a = load_dreg(dst, "a");
    auto* b = load_dreg(src, "b");
    auto* v = builder_.CreateMul(a, b, "mul");
    store_dreg(dst, v);
    return true;
}
// ALU2op_ADD_SHIFT1: Rd = (Rd + Rs) << 1
// Semantics mirror add_and_shift(shift=1) in refs/bfin-sim.c.
// v_internal accumulates overflow from BOTH the add step and each shift step:
//   - add overflow (signed): (a^sum)[31] & (b^sum)[31]
//   - shift overflow: bits[31:30] of pre-shift value differ (01 or 10)
// Final: AZ=(v==0), AN=(v<0), V=v_internal, if V then VS=1 (sticky). AC0 not updated.
bool LiftVisitor::decode_ALU2op_ADD_SHIFT1(uint16_t src, uint16_t dst) {
    auto* a = load_dreg(dst, "a");
    auto* s = load_dreg(src, "s");
    auto* v = builder_.CreateAdd(a, s, "sum");
    // Add overflow: (a^sum)[31] & (b^sum)[31]
    auto* add_xor_a = builder_.CreateLShr(builder_.CreateXor(a, v), 31);
    auto* add_xor_b = builder_.CreateLShr(builder_.CreateXor(s, v), 31);
    auto* v_internal = builder_.CreateAnd(add_xor_a, add_xor_b, "v_int_add");
    // Shift step: bits[31:30] differ → overflow
    auto* top2 = builder_.CreateAnd(builder_.CreateLShr(v, 30), builder_.getInt32(3));
    auto* ov = builder_.CreateXor(builder_.CreateAnd(top2, builder_.getInt32(1)),
                                  builder_.CreateLShr(top2, 1));
    v_internal = builder_.CreateOr(v_internal, ov, "v_internal");
    v = builder_.CreateShl(v, 1, "v_shifted");
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(v, builder_.getInt32(0)),
                                   builder_.getInt32Ty(), "az");
    auto* an = builder_.CreateZExt(builder_.CreateICmpSLT(v, builder_.getInt32(0)),
                                   builder_.getInt32Ty(), "an");
    auto* v_flag = builder_.CreateZExt(builder_.CreateICmpNE(v_internal, builder_.getInt32(0)),
                                       builder_.getInt32Ty(), "v_flag");
    auto* vs_new = builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag);
    store_dreg(dst, v);
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_v(v_flag);  // sets V and V_COPY
    store_cpu_u32(offsetof(CpuState, vs), vs_new);
    return true;
}
// ALU2op_ADD_SHIFT2: Rd = (Rd + Rs) << 2
// Same as ADD_SHIFT1 but two shift steps.
bool LiftVisitor::decode_ALU2op_ADD_SHIFT2(uint16_t src, uint16_t dst) {
    auto* a = load_dreg(dst, "a");
    auto* s = load_dreg(src, "s");
    auto* v = builder_.CreateAdd(a, s, "sum");
    // Add overflow
    auto* add_xor_a = builder_.CreateLShr(builder_.CreateXor(a, v), 31);
    auto* add_xor_b = builder_.CreateLShr(builder_.CreateXor(s, v), 31);
    auto* v_internal = builder_.CreateAnd(add_xor_a, add_xor_b, "v_int_add");
    // Step 1
    auto* top2_0 = builder_.CreateAnd(builder_.CreateLShr(v, 30), builder_.getInt32(3));
    auto* ov0 = builder_.CreateXor(builder_.CreateAnd(top2_0, builder_.getInt32(1)),
                                   builder_.CreateLShr(top2_0, 1));
    v_internal = builder_.CreateOr(v_internal, ov0);
    v = builder_.CreateShl(v, 1, "v1");
    // Step 2
    auto* top2_1 = builder_.CreateAnd(builder_.CreateLShr(v, 30), builder_.getInt32(3));
    auto* ov1 = builder_.CreateXor(builder_.CreateAnd(top2_1, builder_.getInt32(1)),
                                   builder_.CreateLShr(top2_1, 1));
    v_internal = builder_.CreateOr(v_internal, ov1, "v_internal");
    v = builder_.CreateShl(v, 1, "v2");
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(v, builder_.getInt32(0)),
                                   builder_.getInt32Ty(), "az");
    auto* an = builder_.CreateZExt(builder_.CreateICmpSLT(v, builder_.getInt32(0)),
                                   builder_.getInt32Ty(), "an");
    auto* v_flag = builder_.CreateZExt(builder_.CreateICmpNE(v_internal, builder_.getInt32(0)),
                                       builder_.getInt32Ty(), "v_flag");
    auto* vs_new = builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag);
    store_dreg(dst, v);
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_v(v_flag);  // sets V and V_COPY
    store_cpu_u32(offsetof(CpuState, vs), vs_new);
    return true;
}
// ALU2op_DIVS: divs(Rd, Rs)  — division sign initialization
// Semantics: divs() from refs/bfin-sim.c line 1166.
// Computes initial quotient sign bit from MSBs of upper 16 bits of Rd and low 16 bits of Rs.
// Sets ASTAT.AQ, left-shifts Rd, inserts AQ as LSB, patches upper 15 bits with original upper 16 bits.
// No ASTAT arithmetic/logic flags updated.
bool LiftVisitor::decode_ALU2op_DIVS(uint16_t src, uint16_t dst) {
    auto* pquo    = load_dreg(dst, "pquo");
    auto* rs      = load_dreg(src, "rs");
    auto* divisor = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "divisor");
    auto* r       = builder_.CreateLShr(pquo, builder_.getInt32(16), "r");
    // aq = (r ^ divisor) >> 15
    auto* xored   = builder_.CreateXor(r, divisor, "xor_rd");
    auto* aq      = builder_.CreateLShr(xored, builder_.getInt32(15), "aq");
    store_cpu_u32(offsetof(CpuState, aq), aq);
    // pquo <<= 1; pquo |= aq
    auto* shifted = builder_.CreateShl(pquo, builder_.getInt32(1), "pquo_sh");
    auto* with_aq = builder_.CreateOr(shifted, aq, "pquo_aq");
    // pquo = (pquo & 0x1FFFF) | (r << 17)
    auto* low17   = builder_.CreateAnd(with_aq, builder_.getInt32(0x1FFFF), "low17");
    auto* r_up    = builder_.CreateShl(r, builder_.getInt32(17), "r_up");
    auto* result  = builder_.CreateOr(low17, r_up, "result");
    store_dreg(dst, result);
    return true;
}
// ALU2op_DIVQ: divq(Rd, Rs)  — non-restoring division step
// Semantics: divq() from refs/bfin-sim.c line 1186.
// Reads ASTAT.AQ: if set, add divisor to partial remainder af; else subtract.
// Recomputes AQ from MSBs, updates ASTAT.AQ, left-shifts Rd, inserts !AQ as LSB (builds quotient bits).
// No ASTAT arithmetic/logic flags updated.
bool LiftVisitor::decode_ALU2op_DIVQ(uint16_t src, uint16_t dst) {
    auto* pquo    = load_dreg(dst, "pquo");
    auto* rs      = load_dreg(src, "rs");
    auto* divisor = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "divisor");
    auto* cur_aq  = load_cpu_u32(offsetof(CpuState, aq), "cur_aq");
    auto* af      = builder_.CreateLShr(pquo, builder_.getInt32(16), "af");
    // r = aq ? (af + divisor) : (af - divisor)
    auto* r_add   = builder_.CreateAdd(af, divisor, "r_add");
    auto* r_sub   = builder_.CreateSub(af, divisor, "r_sub");
    auto* aq_bool = builder_.CreateICmpNE(cur_aq, builder_.getInt32(0), "aq_bool");
    auto* r       = builder_.CreateSelect(aq_bool, r_add, r_sub, "r");
    // Mask r to 16 bits (unsigned short arithmetic)
    auto* r16     = builder_.CreateAnd(r, builder_.getInt32(0xFFFF), "r16");
    // aq = (r ^ divisor) >> 15
    auto* xored   = builder_.CreateXor(r16, divisor, "xor_rd");
    auto* aq      = builder_.CreateLShr(xored, builder_.getInt32(15), "aq");
    store_cpu_u32(offsetof(CpuState, aq), aq);
    // pquo <<= 1; pquo |= !aq  (logical NOT: XOR with 1)
    auto* shifted = builder_.CreateShl(pquo, builder_.getInt32(1), "pquo_sh");
    auto* not_aq  = builder_.CreateXor(aq, builder_.getInt32(1), "not_aq");
    auto* with_q  = builder_.CreateOr(shifted, not_aq, "pquo_q");
    // pquo = (with_q & 0x1FFFF) | (r16 << 17)
    auto* low17   = builder_.CreateAnd(with_q, builder_.getInt32(0x1FFFF), "low17");
    auto* r_up    = builder_.CreateShl(r16, builder_.getInt32(17), "r_up");
    auto* result  = builder_.CreateOr(low17, r_up, "result");
    store_dreg(dst, result);
    return true;
}
bool LiftVisitor::decode_ALU2op_SEXT_L(uint16_t src, uint16_t dst) {
    auto* rs     = load_dreg(src, "rs");
    auto* result = builder_.CreateSExt(
        builder_.CreateTrunc(rs, builder_.getInt16Ty()),
        builder_.getInt32Ty(), "sext_l");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_ALU2op_ZEXT_L(uint16_t src, uint16_t dst) {
    auto* rs     = load_dreg(src, "rs");
    auto* result = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "zext_l");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_ALU2op_SEXT_B(uint16_t src, uint16_t dst) {
    auto* rs     = load_dreg(src, "rs");
    auto* result = builder_.CreateSExt(
        builder_.CreateTrunc(rs, builder_.getInt8Ty()),
        builder_.getInt32Ty(), "sext_b");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_ALU2op_ZEXT_B(uint16_t src, uint16_t dst) {
    auto* rs   = load_dreg(src, "rs");
    auto* byte = builder_.CreateAnd(rs, builder_.getInt32(0xFF), "byte");
    store_dreg(dst, byte);
    emit_flags_logic(byte);
    return true;
}
bool LiftVisitor::decode_ALU2op_NEG(uint16_t src, uint16_t dst) {
    auto* rs     = load_dreg(src, "rs");
    auto* result = builder_.CreateNeg(rs, "neg");
    store_dreg(dst, result);
    // V = overflow when negating INT_MIN (0x80000000)
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateICmpEQ(rs, builder_.getInt32(0x80000000u)),
        builder_.getInt32Ty(), "v");
    // AC0 = (src == 0): no borrow when negating zero
    auto* ac0 = builder_.CreateZExt(
        builder_.CreateICmpEQ(rs, builder_.getInt32(0)),
        builder_.getInt32Ty(), "ac0");
    emit_flags_arith(result, v_flag, ac0);
    return true;
}
bool LiftVisitor::decode_ALU2op_NOT(uint16_t src, uint16_t dst) {
    auto* rs     = load_dreg(src, "rs");
    auto* result = builder_.CreateNot(rs, "not");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}

// PTR2op — pointer register arithmetic, no ASTAT flags
bool LiftVisitor::decode_PTR2op_SUB(uint16_t src, uint16_t dst) {
    auto* pd = load_preg(dst, "pd");
    auto* ps = load_preg(src, "ps");
    auto* result = builder_.CreateSub(pd, ps, "sub");
    store_preg(dst, result);
    return true;
}

bool LiftVisitor::decode_PTR2op_LSHIFT_LEFT2(uint16_t src, uint16_t dst) {
    auto* ps = load_preg(src, "ps");
    auto* result = builder_.CreateShl(ps, builder_.getInt32(2), "lsl2");
    store_preg(dst, result);
    return true;
}

bool LiftVisitor::decode_PTR2op_LSHIFT_RIGHT2(uint16_t src, uint16_t dst) {
    auto* ps = load_preg(src, "ps");
    auto* result = builder_.CreateLShr(ps, builder_.getInt32(2), "lsr2");
    store_preg(dst, result);
    return true;
}

bool LiftVisitor::decode_PTR2op_LSHIFT_RIGHT1(uint16_t src, uint16_t dst) {
    auto* ps = load_preg(src, "ps");
    auto* result = builder_.CreateLShr(ps, builder_.getInt32(1), "lsr1");
    store_preg(dst, result);
    return true;
}

bool LiftVisitor::decode_PTR2op_ADD_BREV(uint16_t src, uint16_t dst) {
    // add_brev(a, b) = bitreverse(bitreverse(a) + bitreverse(b))
    // Carry propagates MSB-to-LSB, equivalent to reversing both inputs,
    // doing normal addition (carry LSB-to-MSB), then reversing the result.
    auto* pd = load_preg(dst, "pd");
    auto* ps = load_preg(src, "ps");
    auto* brev_fn = Intrinsic::getDeclaration(module_, Intrinsic::bitreverse,
                                              {builder_.getInt32Ty()});
    auto* ra = builder_.CreateCall(brev_fn, {pd}, "brev_a");
    auto* rb = builder_.CreateCall(brev_fn, {ps}, "brev_b");
    auto* sum = builder_.CreateAdd(ra, rb, "brev_sum");
    auto* result = builder_.CreateCall(brev_fn, {sum}, "add_brev");
    store_preg(dst, result);
    return true;
}

bool LiftVisitor::decode_PTR2op_ADD_SHIFT1(uint16_t src, uint16_t dst) {
    auto* pd = load_preg(dst, "pd");
    auto* ps = load_preg(src, "ps");
    auto* sum = builder_.CreateAdd(pd, ps, "sum");
    auto* result = builder_.CreateShl(sum, builder_.getInt32(1), "shl1");
    store_preg(dst, result);
    return true;
}

bool LiftVisitor::decode_PTR2op_ADD_SHIFT2(uint16_t src, uint16_t dst) {
    auto* pd = load_preg(dst, "pd");
    auto* ps = load_preg(src, "ps");
    auto* sum = builder_.CreateAdd(pd, ps, "sum");
    auto* result = builder_.CreateShl(sum, builder_.getInt32(2), "shl2");
    store_preg(dst, result);
    return true;
}

// LOGI2op remaining
bool LiftVisitor::decode_LOGI2op_CC_EQ_AND(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd  = load_dreg(d, "rd");
    auto* bit = builder_.CreateAnd(builder_.CreateLShr(rd, builder_.getInt32(imm)),
                                   builder_.getInt32(1), "bittst");
    auto* cc  = builder_.CreateXor(bit, builder_.getInt32(1), "not_bittst");
    store_cc(cc);
    return true;
}
bool LiftVisitor::decode_LOGI2op_BITSET(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateOr(rd, builder_.getInt32(1u << imm), "bitset");
    store_dreg(d, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_LOGI2op_BITTGL(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateXor(rd, builder_.getInt32(1u << imm), "bittgl");
    store_dreg(d, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_LOGI2op_BITCLR(uint16_t imm, uint16_t d) {
    if (parallel_slot_ > 0) return false;

    auto* rd = load_dreg(d, "rd");
    auto* result = builder_.CreateAnd(rd, builder_.getInt32(~(1u << imm)), "bitclr");
    store_dreg(d, result);
    emit_flags_logic(result);
    return true;
}

// COMP3op
bool LiftVisitor::decode_COMP3op_ADD(uint16_t dst, uint16_t t, uint16_t s) {
    // Rd = Rs + Rt  (3-register 32-bit add)
    // Reference: add32(a=DREG(src0)=s, b=DREG(src1)=t, carry=1, sat=0)
    auto* rs = load_dreg(s, "rs");   // a = src0
    auto* rt = load_dreg(t, "rt");   // b = src1
    auto* result = builder_.CreateAdd(rs, rt, "add32");
    store_dreg(dst, result);
    // Signed overflow: (flgs ^ flgn) & (flgo ^ flgn)
    auto* flgs = builder_.CreateLShr(rs,     builder_.getInt32(31), "flgs");
    auto* flgo = builder_.CreateLShr(rt,     builder_.getInt32(31), "flgo");
    auto* flgn = builder_.CreateLShr(result, builder_.getInt32(31), "flgn");
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateAnd(builder_.CreateXor(flgs, flgn),
                           builder_.CreateXor(flgo, flgn)),
        builder_.getInt32Ty(), "v");
    // Unsigned carry: ~a < b  (a=rs, b=rt)
    auto* ac0 = builder_.CreateZExt(
        builder_.CreateICmpULT(builder_.CreateNot(rs), rt),
        builder_.getInt32Ty(), "ac0");
    emit_flags_arith(result, v_flag, ac0);
    return true;
}
bool LiftVisitor::decode_COMP3op_SUB(uint16_t dst, uint16_t t, uint16_t s) {
    auto* rs = load_dreg(s, "rs");
    auto* rt = load_dreg(t, "rt");
    auto* result = builder_.CreateSub(rs, rt, "sub32");
    store_dreg(dst, result);
    // Signed overflow: (rs ^ rt)[31] & (rs ^ result)[31]
    auto* flgs   = builder_.CreateLShr(rs, 31);
    auto* flgo   = builder_.CreateLShr(rt, 31);
    auto* flgn   = builder_.CreateLShr(result, 31);
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateAnd(builder_.CreateXor(flgs, flgo),
                           builder_.CreateXor(flgn, flgs)),
        builder_.getInt32Ty(), "v");
    // AC0 = borrow: (rt <= rs) unsigned
    auto* ac0 = builder_.CreateZExt(
        builder_.CreateICmpULE(rt, rs), builder_.getInt32Ty(), "ac0");
    emit_flags_arith(result, v_flag, ac0);
    return true;
}
bool LiftVisitor::decode_COMP3op_AND(uint16_t dst, uint16_t t, uint16_t s) {
    auto* rs = load_dreg(s, "rs");
    auto* rt = load_dreg(t, "rt");
    auto* result = builder_.CreateAnd(rt, rs, "and32");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_COMP3op_OR(uint16_t dst, uint16_t t, uint16_t s) {
    auto* rs = load_dreg(s, "rs");
    auto* rt = load_dreg(t, "rt");
    auto* result = builder_.CreateOr(rt, rs, "or32");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_COMP3op_XOR(uint16_t dst, uint16_t t, uint16_t s) {
    auto* rs = load_dreg(s, "rs");
    auto* rt = load_dreg(t, "rt");
    auto* result = builder_.CreateXor(rt, rs, "xor32");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_COMP3op_PADD(uint16_t dst, uint16_t t, uint16_t s) {
    store_preg(dst, builder_.CreateAdd(load_preg(s, "ps"), load_preg(t, "pt"), "padd"));
    return true;
}
bool LiftVisitor::decode_COMP3op_LSHIFT(uint16_t dst, uint16_t t, uint16_t s) {
    store_preg(dst, builder_.CreateAdd(load_preg(s, "ps"),
        builder_.CreateShl(load_preg(t, "pt"), 1), "plshift1"));
    return true;
}
bool LiftVisitor::decode_COMP3op_LSHIFT2(uint16_t dst, uint16_t t, uint16_t s) {
    store_preg(dst, builder_.CreateAdd(load_preg(s, "ps"),
        builder_.CreateShl(load_preg(t, "pt"), 2), "plshift2"));
    return true;
}

// LDSTpmod
bool LiftVisitor::decode_LDSTpmod_LD_32(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // Rd = [Pp ++ Pi] — load 32-bit from [Pp], post-increment by Pi
    auto* addr = load_preg(ptr, "ptr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_dreg(d, val);
    // Only update pointer if ptr != idx (otherwise it's just [Pp])
    auto* idx_val = load_preg(idx, "idx");
    auto* new_ptr = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_LD_16_lo(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // Rd.L = W[Pp++Pi] — load 16-bit into low half, preserve high half
    auto* addr  = load_preg(ptr, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16");
    store_dreg_lo(d, val32);
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_LD_16_hi(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // Rd.H = W[Pp++Pi] — load 16-bit into high half, preserve low half
    auto* addr  = load_preg(ptr, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16");
    store_dreg_hi(d, val32);
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_LD_16_Z(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // Rd = W[Pp++Pi](Z) — load 16-bit, zero-extend to 32-bit
    auto* addr  = load_preg(ptr, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16"));
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_LD_16_X(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // Rd = W[Pp++Pi](X) — 16-bit load with sign-extension (name is misleading; this is a load)
    auto* addr  = load_preg(ptr, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val16, builder_.getInt32Ty(), "sext16"));
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_ST_32(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // [Pp++Pi] = Rd — 32-bit store with post-modify
    auto* addr = load_preg(ptr, "ptr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_ST_16_lo(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // W[Pp++Pi] = Rd.L — store low 16 bits of Rd
    auto* addr  = load_preg(ptr, "ptr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
bool LiftVisitor::decode_LDSTpmod_ST_16_hi(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (parallel_slot_ == 2) return false;

    // W[Pp++Pi] = Rd.H — store high 16 bits of Rd
    auto* addr    = load_preg(ptr, "ptr");
    auto* shifted = builder_.CreateLShr(load_dreg(d, "val"), builder_.getInt32(16), "hi16");
    auto* val16   = builder_.CreateTrunc(shifted, builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    auto* idx_val    = load_preg(idx, "idx");
    auto* new_ptr    = builder_.CreateAdd(addr, idx_val, "ptr_inc");
    auto* ptr_ne_idx = builder_.CreateICmpNE(builder_.getInt32(ptr), builder_.getInt32(idx), "ptr_ne_idx");
    store_preg(ptr, builder_.CreateSelect(ptr_ne_idx, new_ptr, addr, "ptr_out"));
    return true;
}
// LDST remaining - implemented where needed

// X-macro generator for 32-bit D-register load triplet (pp, mm, ind).
// base: instruction name without _mm/_ind suffix
// mem_fn: mem_read32 string, step: post-modify byte count
#define LDST_D_LOAD32(base, step)                                                           \
bool LiftVisitor::decode_##base(uint16_t p, uint16_t d) {                                  \
    auto* addr = load_preg(p, "ptr");                                                       \
    store_dreg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));                \
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(step), "ptr_inc"));           \
    return true;                                                                            \
}                                                                                           \
bool LiftVisitor::decode_##base##_mm(uint16_t p, uint16_t d) {                             \
    auto* addr = load_preg(p, "ptr");                                                       \
    store_dreg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));                \
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(step), "ptr_dec"));           \
    return true;                                                                            \
}                                                                                           \
bool LiftVisitor::decode_##base##_ind(uint16_t p, uint16_t d) {                            \
    auto* addr = load_preg(p, "ptr");                                                       \
    store_dreg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));                \
    return true;                                                                            \
}
LDST_D_LOAD32(LDST_LD_32, 4)

bool LiftVisitor::decode_LDST_ST_32(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    // [Pp++] = Rd
    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(4), "ptr_inc"));
    return true;
}
// LDST_ST_32_mm: [Pp--] = Rd
bool LiftVisitor::decode_LDST_ST_32_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(4), "ptr_dec"));
    return true;
}
// LDST_LD_16_Z: Rd = W[Pp++] (Z) — 16-bit zero-extended load
bool LiftVisitor::decode_LDST_LD_16_Z(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16"));
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(2), "ptr_inc"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_16_Z_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16"));
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(2), "ptr_dec"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_16_Z_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16"));
    return true;
}
// LDST_ST_16: W[Pp++] = Rd
bool LiftVisitor::decode_LDST_ST_16(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(2), "ptr_inc"));
    return true;
}
// LDST_ST_16_mm: W[Pp--] = Rd
bool LiftVisitor::decode_LDST_ST_16_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(2), "ptr_dec"));
    return true;
}
// LDST_ST_16_ind: W[Pp] = Rd
bool LiftVisitor::decode_LDST_ST_16_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    return true;
}
// LDST_LD_16_X: Rd = W[Pp++] (X) — 16-bit sign-extended load
bool LiftVisitor::decode_LDST_LD_16_X(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val16, builder_.getInt32Ty(), "sext16"));
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(2), "ptr_inc"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_16_X_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val16, builder_.getInt32Ty(), "sext16"));
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(2), "ptr_dec"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_16_X_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val16, builder_.getInt32Ty(), "sext16"));
    return true;
}

// X-macro generator for byte load zero-extended triplet (pp, mm, ind).
#define LDST_D_LOAD8_Z(base, step)                                                          \
bool LiftVisitor::decode_##base(uint16_t p, uint16_t d) {                                  \
    auto* addr = load_preg(p, "ptr");                                                       \
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);                   \
    store_dreg(d, builder_.CreateZExt(val8, builder_.getInt32Ty(), "zext8"));              \
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(step), "ptr_inc"));           \
    return true;                                                                            \
}                                                                                           \
bool LiftVisitor::decode_##base##_mm(uint16_t p, uint16_t d) {                             \
    auto* addr = load_preg(p, "ptr");                                                       \
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);                   \
    store_dreg(d, builder_.CreateZExt(val8, builder_.getInt32Ty(), "zext8"));              \
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(step), "ptr_dec"));           \
    return true;                                                                            \
}                                                                                           \
bool LiftVisitor::decode_##base##_ind(uint16_t p, uint16_t d) {                            \
    auto* addr = load_preg(p, "ptr");                                                       \
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);                   \
    store_dreg(d, builder_.CreateZExt(val8, builder_.getInt32Ty(), "zext8"));              \
    return true;                                                                            \
}
LDST_D_LOAD8_Z(LDST_LD_8_Z, 1)
// LDST_ST_8_mm: B[Pp--] = Rd
bool LiftVisitor::decode_LDST_ST_8_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* trunc_byte = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt8Ty(), "trunc_byte");
    emit_mem_write("mem_write8", addr, trunc_byte);
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(1), "ptr_dec"));
    return true;
}
// LDST_ST_8_ind: B[Pp] = Rd
bool LiftVisitor::decode_LDST_ST_8_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* trunc_byte = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt8Ty(), "trunc_byte");
    emit_mem_write("mem_write8", addr, trunc_byte);
    return true;
}
// LDST_LD_8_X: Rd = B[Pp++] (X) — 8-bit sign-extended load
bool LiftVisitor::decode_LDST_LD_8_X(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val8, builder_.getInt32Ty(), "sext8"));
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(1), "ptr_inc"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_8_X_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val8, builder_.getInt32Ty(), "sext8"));
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(1), "ptr_dec"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_8_X_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val8, builder_.getInt32Ty(), "sext8"));
    return true;
}
// LDST_LD_P_32: Pp = [Ps++] — 32-bit load into preg (sz=11)
bool LiftVisitor::decode_LDST_LD_P_32(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_preg(d, val);
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(4), "ptr_inc"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_P_32_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_preg(d, val);
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(4), "ptr_dec"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_P_32_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_preg(d, val);
    return true;
}
// LDST_ST_P_32: [Ps++] = Pp  (sz=11, d is preg)
bool LiftVisitor::decode_LDST_ST_P_32(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(4), "ptr_inc"));
    return true;
}
// LDST_ST_P_32_mm: [Ps--] = Pp
bool LiftVisitor::decode_LDST_ST_P_32_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(4), "ptr_dec"));
    return true;
}
// LDST_ST_P_32_ind: [Ps] = Pp
bool LiftVisitor::decode_LDST_ST_P_32_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    return true;
}
bool LiftVisitor::decode_LDST_LD_P_32_z(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    // Pd = [Pp++]
    if (p == d) return false; // illegal: src and dest same reg with post-modify
    auto* addr = load_preg(p, "ptr");
    auto* val  = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_preg(d, val);
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(4), "ptr_inc"));
    return true;
}
// LDST_LD_P_32_z_mm: Pd = [Ps--] (sz=00, Z=1)
bool LiftVisitor::decode_LDST_LD_P_32_z_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    if (p == d) return false; // illegal: src and dest same reg with post-modify
    auto* addr = load_preg(p, "ptr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_preg(d, val);
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(4), "ptr_dec"));
    return true;
}
// LDST_LD_P_32_z_ind: Pd = [Ps]
bool LiftVisitor::decode_LDST_LD_P_32_z_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_preg(d, val);
    return true;
}
// LDST_ST_P_32_z: [Ps++] = Pd  (sz=00, Z=1, d is preg)
bool LiftVisitor::decode_LDST_ST_P_32_z(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    store_preg(p, builder_.CreateAdd(addr, builder_.getInt32(4), "ptr_inc"));
    return true;
}
// LDST_ST_P_32_z_mm: [Ps--] = Pd
bool LiftVisitor::decode_LDST_ST_P_32_z_mm(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    store_preg(p, builder_.CreateSub(addr, builder_.getInt32(4), "ptr_dec"));
    return true;
}
// LDST_ST_P_32_z_ind: [Ps] = Pd
bool LiftVisitor::decode_LDST_ST_P_32_z_ind(uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* addr = load_preg(p, "ptr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    return true;
}

// Carry-bit accurate dagadd, matching reference bfin-sim.c dagadd().
// m_val is i32 (signed). Handles both M>=0 and M<0 paths.
void LiftVisitor::emit_dagmod_add(uint16_t dagno, llvm::Value* m_val) {
    auto& B = builder_;
    auto* i64ty = B.getInt64Ty();

    auto* i_val = load_cpu_u32(offsetof(CpuState, iregs) + dagno * 4, "ireg");
    auto* l_val = load_cpu_u32(offsetof(CpuState, lregs) + dagno * 4, "lreg");
    auto* b_val = load_cpu_u32(offsetof(CpuState, bregs) + dagno * 4, "breg");

    // Widen to 64-bit (unsigned extension, matching ref: bu64 m = (bu32)M)
    auto* i64 = B.CreateZExt(i_val, i64ty, "i64");
    auto* l64 = B.CreateZExt(l_val, i64ty, "l64");
    auto* b64 = B.CreateZExt(b_val, i64ty, "b64");
    auto* m64 = B.CreateZExt(m_val, i64ty, "m64");  // (bu32)M then widen

    auto* msb = B.getInt64(UINT64_C(1) << 31);       // 0x80000000
    auto* car = B.getInt64(UINT64_C(1) << 32);       // 0x100000000

    auto* IM    = B.CreateAdd(i64, m64, "IM");
    auto* im32  = B.CreateTrunc(IM, B.getInt32Ty(), "im32");
    auto* LB    = B.CreateAdd(l64, b64, "LB");
    auto* lb32  = B.CreateTrunc(LB, B.getInt32Ty(), "lb32");

    auto* m_neg = B.CreateICmpSLT(m_val, B.getInt32(0), "m_neg");

    // === Positive path (M >= 0) ===
    // IML_pos = i + m - l
    auto* IML_pos   = B.CreateSub(IM, l64, "IML_pos");
    auto* iml32_pos = B.CreateTrunc(IML_pos, B.getInt32Ty(), "iml32_pos");
    // Check: (IM & car) == (LB & car)
    auto* IM_car  = B.CreateAnd(IM, car, "IM_car");
    auto* LB_car  = B.CreateAnd(LB, car, "LB_car");
    auto* same_carry = B.CreateICmpEQ(IM_car, LB_car, "same_carry");
    auto* cmp_pos    = B.CreateICmpULT(im32, lb32, "cmp_pos");
    // if same_carry: res = (im32 < lb32) ? im32 : iml32
    // else:          res = (im32 < lb32) ? iml32 : im32
    auto* pos_same = B.CreateSelect(cmp_pos, im32, iml32_pos, "pos_same");
    auto* pos_diff = B.CreateSelect(cmp_pos, iml32_pos, im32, "pos_diff");
    auto* pos_result = B.CreateSelect(same_carry, pos_same, pos_diff, "pos_res");

    // === Negative path (M < 0) ===
    // IML_neg = i + m + l
    auto* IML_neg   = B.CreateAdd(IM, l64, "IML_neg");
    auto* iml32_neg = B.CreateTrunc(IML_neg, B.getInt32Ty(), "iml32_neg");
    // Check: (i & msb) || (IM & car)
    auto* i_msb    = B.CreateICmpNE(B.CreateAnd(i64, msb, "i_msb_v"), B.getInt64(0), "i_msb");
    auto* im_carry = B.CreateICmpNE(B.CreateAnd(IM, car, "im_car_v"), B.getInt64(0), "im_carry");
    auto* flag_neg = B.CreateOr(i_msb, im_carry, "flag_neg");
    auto* cmp_neg  = B.CreateICmpULT(im32, b_val, "cmp_neg");
    // if flag_neg: res = (im32 < b) ? iml32 : im32
    // else:        res = (im32 < b) ? im32 : iml32
    auto* neg_flag = B.CreateSelect(cmp_neg, iml32_neg, im32, "neg_flag");
    auto* neg_noflag = B.CreateSelect(cmp_neg, im32, iml32_neg, "neg_noflag");
    auto* neg_result = B.CreateSelect(flag_neg, neg_flag, neg_noflag, "neg_res");

    auto* result = B.CreateSelect(m_neg, neg_result, pos_result, "dag_result");
    store_cpu_u32(offsetof(CpuState, iregs) + dagno * 4, result);
}

// Carry-bit accurate dagsub, matching reference bfin-sim.c dagsub().
// m_val is i32 (signed). Handles both M>=0 and M<0 paths.
void LiftVisitor::emit_dagmod_sub(uint16_t dagno, llvm::Value* m_val) {
    auto& B = builder_;
    auto* i64ty = B.getInt64Ty();

    auto* i_val = load_cpu_u32(offsetof(CpuState, iregs) + dagno * 4, "ireg");
    auto* l_val = load_cpu_u32(offsetof(CpuState, lregs) + dagno * 4, "lreg");
    auto* b_val = load_cpu_u32(offsetof(CpuState, bregs) + dagno * 4, "breg");

    auto* i64 = B.CreateZExt(i_val, i64ty, "i64");
    auto* l64 = B.CreateZExt(l_val, i64ty, "l64");
    auto* b64 = B.CreateZExt(b_val, i64ty, "b64");

    // mbar = (bu32)(~(bu32)M + 1) — two's complement negation at 32-bit, then zext
    auto* m_neg_32 = B.CreateNeg(m_val, "m_neg32");  // -M at i32 (same as ~M+1)
    auto* mbar64   = B.CreateZExt(m_neg_32, i64ty, "mbar64");

    auto* msb = B.getInt64(UINT64_C(1) << 31);
    auto* car = B.getInt64(UINT64_C(1) << 32);

    auto* IM    = B.CreateAdd(i64, mbar64, "IM");
    auto* im32  = B.CreateTrunc(IM, B.getInt32Ty(), "im32");
    auto* LB    = B.CreateAdd(l64, b64, "LB");
    auto* lb32  = B.CreateTrunc(LB, B.getInt32Ty(), "lb32");

    auto* m_is_neg = B.CreateICmpSLT(m_val, B.getInt32(0), "m_is_neg");

    // === M < 0 path (actually adding |M|) ===
    // IML = i + mbar - l
    auto* IML_mneg   = B.CreateSub(IM, l64, "IML_mneg");
    auto* iml32_mneg = B.CreateTrunc(IML_mneg, B.getInt32Ty(), "iml32_mneg");
    // Condition: !!((i & msb) && (IM & car)) == !!(LB & car)
    auto* i_has_msb  = B.CreateICmpNE(B.CreateAnd(i64, msb), B.getInt64(0), "i_has_msb");
    auto* im_has_car = B.CreateICmpNE(B.CreateAnd(IM, car), B.getInt64(0), "im_has_car");
    auto* both       = B.CreateAnd(i_has_msb, im_has_car, "both_msb_car");
    auto* lb_has_car = B.CreateICmpNE(B.CreateAnd(LB, car), B.getInt64(0), "lb_has_car");
    auto* mneg_cond  = B.CreateICmpEQ(both, lb_has_car, "mneg_cond");
    auto* cmp_mneg   = B.CreateICmpULT(im32, lb32, "cmp_mneg");
    // if cond: res = (im32 < lb32) ? im32 : iml32
    // else:    res = (im32 < lb32) ? iml32 : im32
    auto* mneg_yes = B.CreateSelect(cmp_mneg, im32, iml32_mneg, "mneg_yes");
    auto* mneg_no  = B.CreateSelect(cmp_mneg, iml32_mneg, im32, "mneg_no");
    auto* mneg_result = B.CreateSelect(mneg_cond, mneg_yes, mneg_no, "mneg_res");

    // === M >= 0 path (actually subtracting M) ===
    // IML = i + mbar + l
    auto* IML_mpos   = B.CreateAdd(IM, l64, "IML_mpos");
    auto* iml32_mpos = B.CreateTrunc(IML_mpos, B.getInt32Ty(), "iml32_mpos");
    // Condition: M == 0 || (IM & car)
    auto* m_is_zero  = B.CreateICmpEQ(m_val, B.getInt32(0), "m_is_zero");
    auto* im_car     = B.CreateICmpNE(B.CreateAnd(IM, car), B.getInt64(0), "im_car");
    auto* mpos_cond  = B.CreateOr(m_is_zero, im_car, "mpos_cond");
    auto* cmp_mpos   = B.CreateICmpULT(im32, b_val, "cmp_mpos");
    // if cond: res = (im32 < b) ? iml32 : im32
    // else:    res = (im32 < b) ? im32 : iml32
    auto* mpos_yes = B.CreateSelect(cmp_mpos, iml32_mpos, im32, "mpos_yes");
    auto* mpos_no  = B.CreateSelect(cmp_mpos, im32, iml32_mpos, "mpos_no");
    auto* mpos_result = B.CreateSelect(mpos_cond, mpos_yes, mpos_no, "mpos_res");

    auto* result = B.CreateSelect(m_is_neg, mneg_result, mpos_result, "dag_result");
    store_cpu_u32(offsetof(CpuState, iregs) + dagno * 4, result);
}

// Convenience: dagadd with positive constant delta
void LiftVisitor::emit_dagadd(uint16_t dagno, uint32_t delta) {
    emit_dagmod_add(dagno, builder_.getInt32(delta));
}

// Convenience: dagsub with positive constant delta
void LiftVisitor::emit_dagsub(uint16_t dagno, uint32_t delta) {
    emit_dagmod_sub(dagno, builder_.getInt32(delta));
}

// dagadd with M register (signed)
void LiftVisitor::emit_dagadd_mreg(uint16_t dagno, uint16_t mreg_idx) {
    auto* m_val = load_cpu_u32(offsetof(CpuState, mregs) + mreg_idx * 4, "mreg");
    emit_dagmod_add(dagno, m_val);
}

// dagsub with M register (signed)
void LiftVisitor::emit_dagsub_mreg(uint16_t dagno, uint16_t mreg_idx) {
    auto* m_val = load_cpu_u32(offsetof(CpuState, mregs) + mreg_idx * 4, "mreg");
    emit_dagmod_sub(dagno, m_val);
}

// BYTEOP alignment rotation: aln=0 → l; else (l >> 8*aln) | (h << (32 - 8*aln))
llvm::Value* LiftVisitor::emit_byte_align(llvm::Value* l, llvm::Value* h, llvm::Value* aln) {
    auto* shift_r = builder_.CreateMul(aln, builder_.getInt32(8), "sr");
    // fshr(h, l, sr) = (h << (32-sr)) | (l >> sr); sr=0 yields l (no special case needed)
    return emit_fshr(h, l, shift_r);
}

llvm::Value* LiftVisitor::emit_byteop_load_align(uint32_t src, uint32_t ireg_idx,
                                                  bool reversed, const Twine& name) {
    auto* i_raw = load_cpu_u32(offsetof(CpuState, iregs) + ireg_idx * 4, "i");
    auto* i_aln = builder_.CreateAnd(i_raw, builder_.getInt32(3), "aln");
    return emit_byteop_load_align_v(src, i_aln, reversed, name);
}

llvm::Value* LiftVisitor::emit_byteop_load_align_v(uint32_t src, llvm::Value* i_aln,
                                                    bool reversed, const Twine& name) {
    auto* sL = load_dreg(src,     name + "L");
    auto* sH = load_dreg(src + 1, name + "H");
    return reversed ? emit_byte_align(sH, sL, i_aln) : emit_byte_align(sL, sH, i_aln);
}

llvm::Value* LiftVisitor::emit_byteop_extract_ub(llvm::Value* v, unsigned k) {
    auto* shifted = builder_.CreateLShr(v, builder_.getInt32(8 * k));
    return builder_.CreateAnd(shifted, builder_.getInt32(0xff), "ub");
}

llvm::Value* LiftVisitor::emit_byteop_pack2(llvm::Value* b0, llvm::Value* b1, bool hi_slot) {
    auto& B = builder_;
    if (hi_slot)
        return B.CreateOr(B.CreateShl(b1, B.getInt32(24)),
                          B.CreateShl(b0, B.getInt32(8)));
    return B.CreateOr(B.CreateShl(b1, B.getInt32(16)), b0);
}

// dspLDST
bool LiftVisitor::decode_dspLDST_LD_dreg(uint16_t i, uint16_t d) {
    // Rd = [Ii++]  (32-bit load, post-increment by 4 with circular buffer)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    if (dis_algn_expt_)
        addr = builder_.CreateAnd(addr, builder_.getInt32(~3u), "algn_addr");
    auto* val  = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_dreg(d, val);
    emit_dagadd(i, 4);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_lo(uint16_t i, uint16_t d) {
    // Rd.L = W[Ii++]  (16-bit load into low halfword, post-increment by 2)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "val32");
    store_dreg_lo(d, val32);
    emit_dagadd(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_hi(uint16_t i, uint16_t d) {
    // Rd.H = W[Ii++]  (16-bit load into high halfword, post-increment by 2)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "val32");
    store_dreg_hi(d, val32);
    emit_dagadd(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_mm(uint16_t i, uint16_t d) {
    // Rd = [Ii--]  (32-bit load, post-decrement Ii by 4 with circular buffer)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    if (dis_algn_expt_)
        addr = builder_.CreateAnd(addr, builder_.getInt32(~3u), "algn_addr");
    auto* val  = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_dreg(d, val);
    emit_dagsub(i, 4);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_mm_lo(uint16_t i, uint16_t d) {
    // Rd.L = W[Ii--]  (16-bit load into low halfword, post-decrement by 2)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "val32");
    store_dreg_lo(d, val32);
    emit_dagsub(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_mm_hi(uint16_t i, uint16_t d) {
    // Rd.H = W[Ii--]  (16-bit load into high halfword, post-decrement by 2)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "val32");
    store_dreg_hi(d, val32);
    emit_dagsub(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_Mmod(uint16_t i, uint16_t d) {
    // Rd = [Ii]  (32-bit load, no post-modify)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    if (dis_algn_expt_)
        addr = builder_.CreateAnd(addr, builder_.getInt32(~3u), "algn_addr");
    auto* val  = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_dreg(d, val);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_Mmod_lo(uint16_t i, uint16_t d) {
    // Rd.L = W[Ii]  (16-bit load into low halfword, no post-modify)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "val32");
    store_dreg_lo(d, val32);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_Mmod_hi(uint16_t i, uint16_t d) {
    // Rd.H = W[Ii]  (16-bit load into high halfword, no post-modify)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    auto* val32 = builder_.CreateZExt(val16, builder_.getInt32Ty(), "val32");
    store_dreg_hi(d, val32);
    return true;
}
bool LiftVisitor::decode_dspLDST_LD_dreg_brev(uint16_t m, uint16_t i, uint16_t d) {
    // Rd = [Ii ++ Mm]  (32-bit load, then circular dagadd with signed M register)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    if (dis_algn_expt_)
        addr = builder_.CreateAnd(addr, builder_.getInt32(~3u), "algn_addr");
    auto* val  = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    store_dreg(d, val);
    emit_dagadd_mreg(i, m);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg(uint16_t i, uint16_t d) {
    // [Ii++] = Rd  (32-bit store, post-increment Ii by 4 with circular buffer)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    emit_dagadd(i, 4);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_lo(uint16_t i, uint16_t d) {
    // W[Ii++] = Rd.L
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    emit_dagadd(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_hi(uint16_t i, uint16_t d) {
    // W[Ii++] = Rd.H
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = builder_.CreateTrunc(
        builder_.CreateLShr(load_dreg(d, "val"), 16u), builder_.getInt16Ty(), "hi16");
    emit_mem_write("mem_write16", addr, val16);
    emit_dagadd(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_mm(uint16_t i, uint16_t d) {
    // [Ii--] = Rd
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    emit_dagsub(i, 4);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_mm_lo(uint16_t i, uint16_t d) {
    // W[Ii--] = Rd.L
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    emit_dagsub(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_mm_hi(uint16_t i, uint16_t d) {
    // W[Ii--] = Rd.H
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = builder_.CreateTrunc(
        builder_.CreateLShr(load_dreg(d, "val"), 16u), builder_.getInt16Ty(), "hi16");
    emit_mem_write("mem_write16", addr, val16);
    emit_dagsub(i, 2);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_Mmod(uint16_t i, uint16_t d) {
    // [Ii] = Rd  (no post-modify; Ii unchanged)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_Mmod_lo(uint16_t i, uint16_t d) {
    // W[Ii] = Rd.L  (no post-modify)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_Mmod_hi(uint16_t i, uint16_t d) {
    // W[Ii] = Rd.H  (no post-modify)
    auto* addr  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    auto* val16 = builder_.CreateTrunc(
        builder_.CreateLShr(load_dreg(d, "val"), 16u), builder_.getInt16Ty(), "hi16");
    emit_mem_write("mem_write16", addr, val16);
    return true;
}
bool LiftVisitor::decode_dspLDST_ST_dreg_brev(uint16_t m, uint16_t i, uint16_t d) {
    // [Ii ++ Mm] = Rd  (32-bit store, then circular dagadd with signed M register)
    auto* addr = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "i_addr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    emit_dagadd_mreg(i, m);
    return true;
}

// LDSTii remaining
bool LiftVisitor::decode_LDSTii_LD_32(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    // Rd = [Pp + uimm4s4(offset)]  (load 32-bit)
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 4u), "addr");
    store_dreg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));
    return true;
}
bool LiftVisitor::decode_LDSTii_LD_16_Z(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    // Rd = W[Pp + uimm4s2(offset)] (Z)  (16-bit load zero-extend, uimm4s2 scaling)
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 2u), "addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16"));
    return true;
}
bool LiftVisitor::decode_LDSTii_LD_16_X(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    // Rd = W[Pp + offset*2] (X)  (load 16-bit, sign-extend to 32-bit)
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 2u), "addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val16, builder_.getInt32Ty(), "sext16"));
    return true;
}
// LDSTii_LD_P_32: Pp = [Ps + uimm4s4(offset)]
bool LiftVisitor::decode_LDSTii_LD_P_32(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 4u), "addr");
    store_preg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));
    return true;
}
bool LiftVisitor::decode_LDSTii_ST_16(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    // W[Pp + offset*2] = Rd.L  (store low 16 bits, uimm4s2 scaling)
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 2u), "addr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    return true;
}

// LDSTii_ST_P_32: [Pp + uimm4s4(offset)] = Pp
bool LiftVisitor::decode_LDSTii_ST_P_32(uint16_t offset, uint16_t p, uint16_t d) {
    if (parallel_slot_ == 2) return false;

    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32(offset * 4u), "addr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    return true;
}

// LDSTiiFP
bool LiftVisitor::decode_LDSTiiFP_LD_32(uint16_t offset, uint16_t r) {
    if (parallel_slot_ == 2) return false;

    // Reg = [FP + negimm5s4(offset)]
    // negimm5s4: force bit5=1, sign-extend from 6 bits, scale by 4 → always negative
    int32_t imm = (int32_t)(int8_t)((uint8_t)((offset | 0x20u) << 2));
    auto* fp = load_preg(7, "fp");
    auto* addr = builder_.CreateAdd(fp, builder_.getInt32((uint32_t)imm), "addr");
    auto* val = emit_mem_read("mem_read32", builder_.getInt32Ty(), addr);
    uint16_t grp = (r >> 3) & 1;
    uint16_t reg = r & 7;
    if (grp == 0) store_dreg(reg, val);
    else          store_preg(reg, val);
    return true;
}
bool LiftVisitor::decode_LDSTiiFP_ST_32(uint16_t offset, uint16_t r) {
    if (parallel_slot_ == 2) return false;

    // [FP + negimm5s4(offset)] = Reg
    int32_t imm = (int32_t)(int8_t)((uint8_t)((offset | 0x20u) << 2));
    auto* fp = load_preg(7, "fp");
    auto* addr = builder_.CreateAdd(fp, builder_.getInt32((uint32_t)imm), "addr");
    uint16_t grp = (r >> 3) & 1;
    uint16_t reg = r & 7;
    Value* val = (grp == 0) ? load_dreg(reg, "val") : load_preg(reg, "val");
    emit_mem_write("mem_write32", addr, val);
    return true;
}

// dagMODim/dagMODik
bool LiftVisitor::decode_dagMODim_ADD(uint16_t m, uint16_t i) {
    if (parallel_slot_ == 2) return false;

    // I<i> += M<m> — M register is signed; delegates to carry-bit accurate dagadd
    emit_dagadd_mreg(i, m);
    return true;
}
bool LiftVisitor::decode_dagMODim_SUB(uint16_t m, uint16_t i) {
    if (parallel_slot_ == 2) return false;

    // I<i> -= M<m> — M register is signed; delegates to carry-bit accurate dagsub
    emit_dagsub_mreg(i, m);
    return true;
}
bool LiftVisitor::decode_dagMODim_ADD_BREV(uint16_t m, uint16_t i) {
    if (parallel_slot_ == 2) return false;

    // I<i> += M<m> (BREV) — bit-reversed addition, no circular-buffer wrap.
    // Equivalent to: bitreverse(bitreverse(i_val) + bitreverse(m_val))
    auto* i_val  = load_cpu_u32(offsetof(CpuState, iregs) + i * 4, "ireg");
    auto* m_val  = load_cpu_u32(offsetof(CpuState, mregs) + m * 4, "mreg");
    auto* mod    = builder_.GetInsertBlock()->getModule();
    auto* brev   = llvm::Intrinsic::getDeclaration(mod, llvm::Intrinsic::bitreverse,
                                                   {builder_.getInt32Ty()});
    auto* brev_i = builder_.CreateCall(brev, {i_val}, "brev_i");
    auto* brev_m = builder_.CreateCall(brev, {m_val}, "brev_m");
    auto* sum    = builder_.CreateAdd(brev_i, brev_m, "brev_sum");
    auto* result = builder_.CreateCall(brev, {sum}, "brev_res");
    store_cpu_u32(offsetof(CpuState, iregs) + i * 4, result);
    return true;
}
bool LiftVisitor::decode_dagMODik_ADD2(uint16_t i) {
    if (parallel_slot_ == 2) return false;

    emit_dagadd(i, 2);
    return true;
}
bool LiftVisitor::decode_dagMODik_SUB2(uint16_t i) {
    if (parallel_slot_ == 2) return false;

    emit_dagsub(i, 2);
    return true;
}
bool LiftVisitor::decode_dagMODik_ADD4(uint16_t i) {
    if (parallel_slot_ == 2) return false;

    emit_dagadd(i, 4);
    return true;
}
bool LiftVisitor::decode_dagMODik_SUB4(uint16_t i) {
    if (parallel_slot_ == 2) return false;

    emit_dagsub(i, 4);
    return true;
}

// pseudoDEBUG remaining
// DBG Rx, PRNT, DBG A0/A1 are diagnostic-print-only; no CPU state side effects → NOP
bool LiftVisitor::decode_pseudoDEBUG_DBG_reg(uint16_t, uint16_t) { return true; }
bool LiftVisitor::decode_pseudoDEBUG_PRNT_reg(uint16_t, uint16_t) { return true; }
bool LiftVisitor::decode_pseudoDEBUG_OUTC_dreg(uint16_t r) {
    auto* val = load_dreg(r, "outc_val");
    auto* ft = FunctionType::get(builder_.getVoidTy(), {builder_.getInt32Ty()}, false);
    call_extern("bfin_putchar", ft, {val});
    return true;
}
bool LiftVisitor::decode_pseudoDEBUG_DBG_A0(uint16_t) { return true; }
bool LiftVisitor::decode_pseudoDEBUG_DBG_A1(uint16_t) { return true; }
bool LiftVisitor::decode_pseudoDEBUG_HLT(uint16_t) {
    auto* ptr = cpu_field_ptr(offsetof(CpuState, halted), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), ptr);
    auto* ecode_ptr = cpu_field_ptr(offsetof(CpuState, exit_code), builder_.getInt32Ty());
    builder_.CreateStore(builder_.getInt32(0), ecode_ptr);
    auto* djptr = cpu_field_ptr(offsetof(CpuState, did_jump), builder_.getInt8Ty());
    builder_.CreateStore(builder_.getInt8(1), djptr);
    terminated_ = true;
    return true;
}
// DBGHALT/DBGCMPLX/DBG bare: also unhandled in reference sim → NOP
bool LiftVisitor::decode_pseudoDEBUG_DBGHALT(uint16_t) { return true; }
bool LiftVisitor::decode_pseudoDEBUG_DBGCMPLX(uint16_t) { return true; }
bool LiftVisitor::decode_pseudoDEBUG_DBG(uint16_t) { return true; }

// pseudoChr
bool LiftVisitor::decode_pseudoChr_OUTC(uint16_t ch) {
    auto* ft = FunctionType::get(builder_.getVoidTy(), {builder_.getInt32Ty()}, false);
    call_extern("bfin_putchar", ft, {builder_.getInt32(ch)});
    return true;
}

// 32-bit LoopSetup variants
// rop=0: set LT/LB only, no LC write
bool LiftVisitor::decode_LoopSetup_LC0(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (parallel_slot_ > 0) return false;

    if (reg > 7) return false;
    uint32_t lt_val = current_pc + soffset * 2u;
    uint32_t lb_val = current_pc + eoffset * 2u;
    store_cpu_u32(offsetof(CpuState, lt[0]), builder_.getInt32(lt_val & ~1u));
    store_cpu_u32(offsetof(CpuState, lb[0]), builder_.getInt32(lb_val));
    uint32_t next_insn = current_pc + 4;
    emit_jump_imm((lt_val == next_insn) ? lt_val : next_insn);
    return true;
}
bool LiftVisitor::decode_LoopSetup_LC1(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (parallel_slot_ > 0) return false;

    if (reg > 7) return false;
    uint32_t lt_val = current_pc + soffset * 2u;
    uint32_t lb_val = current_pc + eoffset * 2u;
    store_cpu_u32(offsetof(CpuState, lt[1]), builder_.getInt32(lt_val & ~1u));
    store_cpu_u32(offsetof(CpuState, lb[1]), builder_.getInt32(lb_val));
    uint32_t next_insn = current_pc + 4;
    emit_jump_imm((lt_val == next_insn) ? lt_val : next_insn);
    return true;
}
// rop=3: set LT/LB and LC = Preg>>1
bool LiftVisitor::decode_LoopSetup_LC0_half(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (parallel_slot_ > 0) return false;

    if (reg > 7) return false;
    uint32_t lt_val = current_pc + soffset * 2u;
    uint32_t lb_val = current_pc + eoffset * 2u;
    store_cpu_u32(offsetof(CpuState, lt[0]), builder_.getInt32(lt_val & ~1u));
    store_cpu_u32(offsetof(CpuState, lb[0]), builder_.getInt32(lb_val));
    auto* lc = load_preg(reg, "lc_preg");
    store_cpu_u32(offsetof(CpuState, lc[0]),
                  builder_.CreateLShr(lc, builder_.getInt32(1), "lc_half"));
    uint32_t next_insn = current_pc + 4;
    emit_jump_imm((lt_val == next_insn) ? lt_val : next_insn);
    return true;
}
bool LiftVisitor::decode_LoopSetup_LC1_half(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (parallel_slot_ > 0) return false;

    if (reg > 7) return false;
    uint32_t lt_val = current_pc + soffset * 2u;
    uint32_t lb_val = current_pc + eoffset * 2u;
    store_cpu_u32(offsetof(CpuState, lt[1]), builder_.getInt32(lt_val & ~1u));
    store_cpu_u32(offsetof(CpuState, lb[1]), builder_.getInt32(lb_val));
    auto* lc = load_preg(reg, "lc_preg");
    store_cpu_u32(offsetof(CpuState, lc[1]),
                  builder_.CreateLShr(lc, builder_.getInt32(1), "lc_half"));
    uint32_t next_insn = current_pc + 4;
    emit_jump_imm((lt_val == next_insn) ? lt_val : next_insn);
    return true;
}

bool LiftVisitor::decode_CALLa_JUMP(uint32_t addr) {
    if (parallel_slot_ > 0) return false;

    // JUMP.L: PC = PC + sext24(addr)*2  (no RETS update)
    int32_t soff = signextend<24>(addr);
    uint32_t target = current_pc + static_cast<uint32_t>(soff << 1);
    emit_jump_imm(target);
    return true;
}

bool LiftVisitor::decode_LDSTidxI_LD_32(uint32_t p, uint32_t d, uint32_t offset) {
    // Rd = [Pp + imm16s4(offset)]   (32-bit load, offset = sext16 * 4)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 4;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    store_dreg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_LD_16_Z(uint32_t p, uint32_t d, uint32_t offset) {
    // Rd = W[Pp + imm16s2(offset)] (Z)  (16-bit load zero-extend, offset = sext16 * 2)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 2;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val16, builder_.getInt32Ty(), "zext16"));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_LD_16_X(uint32_t p, uint32_t d, uint32_t offset) {
    // Rd = W[Pp + imm16s2(offset)] (X)  (16-bit load sign-extend, offset = sext16 * 2)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 2;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    auto* val16 = emit_mem_read("mem_read16", builder_.getInt16Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val16, builder_.getInt32Ty(), "sext16"));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_LD_B_Z(uint32_t p, uint32_t d, uint32_t offset) {
    // Rd = B[Pp + imm16(offset)] (Z)  (byte load zero-extend, offset = sext16)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);
    store_dreg(d, builder_.CreateZExt(val8, builder_.getInt32Ty(), "zext8"));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_LD_B_X(uint32_t p, uint32_t d, uint32_t offset) {
    // Rd = B[Pp + imm16(offset)] (X)  (byte load sign-extend, offset = sext16)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    auto* val8 = emit_mem_read("mem_read8", builder_.getInt8Ty(), addr);
    store_dreg(d, builder_.CreateSExt(val8, builder_.getInt32Ty(), "sext8"));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_ST_32(uint32_t p, uint32_t d, uint32_t offset) {
    // [Pp + imm16s4(offset)] = Rd   (32-bit store)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 4;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    emit_mem_write("mem_write32", addr, load_dreg(d, "val"));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_ST_16(uint32_t p, uint32_t d, uint32_t offset) {
    // W[Pp + imm16s2(offset)] = Rd.L  (store low 16 bits, offset = sext16 * 2)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 2;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    auto* val16 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt16Ty(), "trunc16");
    emit_mem_write("mem_write16", addr, val16);
    return true;
}
bool LiftVisitor::decode_LDSTidxI_ST_B(uint32_t p, uint32_t d, uint32_t offset) {
    // B[Pp + imm16(offset)] = Rd.B  (store low byte, offset = sext16)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    auto* val8 = builder_.CreateTrunc(load_dreg(d, "val"), builder_.getInt8Ty(), "trunc8");
    emit_mem_write("mem_write8", addr, val8);
    return true;
}
bool LiftVisitor::decode_LDSTidxI_LD_P_32(uint32_t p, uint32_t d, uint32_t offset) {
    // Pd = [Pp + imm16s4(offset)]   (32-bit load into P-register, offset = sext16 * 4)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 4;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    store_preg(d, emit_mem_read("mem_read32", builder_.getInt32Ty(), addr));
    return true;
}
bool LiftVisitor::decode_LDSTidxI_ST_P_32(uint32_t p, uint32_t d, uint32_t offset) {
    // [Pp + imm16s4(offset)] = Pd   (32-bit store from P-register, offset = sext16 * 4)
    int32_t imm = (int32_t)(int16_t)(uint16_t)offset * 4;
    auto* base = load_preg(p, "ptr");
    auto* addr = builder_.CreateAdd(base, builder_.getInt32((uint32_t)imm), "addr");
    emit_mem_write("mem_write32", addr, load_preg(d, "val"));
    return true;
}

bool LiftVisitor::decode_Linkage_LINK(uint32_t framesize) {
    uint32_t size = framesize * 4; // uimm16s4
    auto* sp   = load_preg(6, "sp");
    auto* rets = load_cpu_u32(offsetof(CpuState, rets), "rets");
    auto* fp   = load_preg(7, "fp");

    // [--SP] = RETS
    sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp1");
    emit_mem_write("mem_write32", sp, rets);

    // [--SP] = FP
    sp = builder_.CreateSub(sp, builder_.getInt32(4), "sp2");
    emit_mem_write("mem_write32", sp, fp);

    // FP = SP
    store_preg(7, sp);

    // SP -= size
    sp = builder_.CreateSub(sp, builder_.getInt32(size), "sp3");
    store_preg(6, sp);
    return true;
}
bool LiftVisitor::decode_Linkage_UNLINK() {
    // SP = FP
    auto* sp = load_preg(7, "fp");

    // FP = [SP]; SP += 4
    auto* fp_new = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
    sp = builder_.CreateAdd(sp, builder_.getInt32(4), "sp1");

    // RETS = [SP]; SP += 4
    auto* rets_new = emit_mem_read("mem_read32", builder_.getInt32Ty(), sp);
    sp = builder_.CreateAdd(sp, builder_.getInt32(4), "sp2");

    store_preg(7, fp_new);
    store_cpu_u32(offsetof(CpuState, rets), rets_new);
    store_preg(6, sp);
    return true;
}

// DSP32 mac stubs
bool LiftVisitor::decode_dsp32mac_MNOP(uint32_t M) {
    (void)M;
    return true;  // MNOP is a pure no-op
}

// ---------------------------------------------------------------------------
// emit_saturate_acc: clamp accumulator to mode-appropriate range, update AV/AVS.
// Returns the saturation flag as i32 (0 or 1). new_acc is updated in place.
// ---------------------------------------------------------------------------
llvm::Value* LiftVisitor::emit_saturate_acc(
    llvm::Value*& new_acc, llvm::Value* tsat_flag, llvm::Value* sgn40,
    uint32_t mmod, uint32_t MM, uint32_t which, bool update_acc)
{
    auto& b = builder_;
    auto* i32ty = b.getInt32Ty();
    bool is_unsigned_acc = (!MM && is_macmod_unsigned_acc(mmod));

    llvm::Value* sat_flag;
    if (mmod == MMOD_IH) {
        // IH: clamp to signed 32-bit range [-0x80000000, 0x7fffffff]
        auto* lo32 = b.getInt64(-(int64_t)0x80000000LL);
        auto* hi32 = b.getInt64(0x7fffffffLL);
        auto* under = b.CreateICmpSLT(new_acc, lo32, "ih_under");
        auto* over  = b.CreateICmpSGT(new_acc, hi32, "ih_over");
        sat_flag = b.CreateZExt(b.CreateOr(under, over), i32ty, "acc_sat_ih");
        new_acc = emit_smin(emit_smax(new_acc, lo32), hi32);
    } else if (mmod == MMOD_W32) {
        // W32: 32-bit window clamp driven by sgn40 (bit 39 of original acc)
        auto* shr31    = b.CreateLShr(new_acc, b.getInt64(31), "w32_shr31");
        auto* is_zero  = b.CreateICmpEQ(shr31, b.getInt64(0), "w32_zero");
        auto* is_ones  = b.CreateICmpEQ(shr31, b.getInt64(0x1ffffffffLL), "w32_ones");
        auto* is_valid  = b.CreateOr(is_zero, is_ones, "w32_valid");
        auto* not_valid = b.CreateNot(is_valid, "w32_not_valid");
        auto* neg_clamp = b.CreateAnd(sgn40, not_valid, "w32_neg_clamp");
        auto* pos_clamp = b.CreateAnd(b.CreateNot(sgn40), not_valid, "w32_pos_clamp");
        new_acc = b.CreateSelect(neg_clamp,
            b.getInt64((int64_t)(uint64_t)0xFFFFFFFF80000000ULL), new_acc, "w32_nc");
        new_acc = b.CreateSelect(pos_clamp,
            b.getInt64(0x7FFFFFFFLL), new_acc, "w32_pc");
        new_acc = b.CreateAnd(new_acc, b.getInt64(0xFFFFFFFFLL));
        auto* has_sign = b.CreateICmpNE(
            b.CreateAnd(new_acc, b.getInt64(0x80000000LL)), b.getInt64(0));
        new_acc = b.CreateSelect(has_sign,
            b.CreateOr(new_acc, b.getInt64((int64_t)(uint64_t)0xFFFFFFFF00000000ULL)),
            new_acc, "w32_sext");
        auto* range_sat = b.CreateOr(neg_clamp, pos_clamp, "w32_range_sat");
        sat_flag = b.CreateZExt(b.CreateOr(range_sat, tsat_flag), i32ty, "acc_sat_w32");
    } else if (mmod == MMOD_IU && MM) {
        // M_IU + MM=1: truncate to 40 bits, do NOT set sat_flag.
        // Reference: acc &= 0xFFFFFFFFFF (no sat=1).
        new_acc = b.CreateAnd(new_acc, b.getInt64(0xFFFFFFFFFFLL), "iu_mm_trunc");
        // Sign-extend bit 39
        auto* bit39 = b.CreateICmpNE(
            b.CreateAnd(new_acc, b.getInt64(0x8000000000LL)), b.getInt64(0), "bit39");
        new_acc = b.CreateSelect(bit39,
            b.CreateOr(new_acc, b.getInt64((int64_t)(uint64_t)0xFFFFFF0000000000ULL)),
            new_acc, "iu_mm_sext");
        sat_flag = b.getInt32(0);
    } else {
        // General 40-bit saturation (signed or unsigned)
        llvm::Value *lo40, *hi40;
        if (is_unsigned_acc) {
            lo40 = b.getInt64(0);
            hi40 = b.getInt64(0xFFFFFFFFFFLL);
        } else {
            lo40 = b.getInt64(-(int64_t)(1LL << 39));
            hi40 = b.getInt64(0x7fffffffffLL);
        }
        auto* under = b.CreateICmpSLT(new_acc, lo40, "under40");
        auto* over  = b.CreateICmpSGT(new_acc, hi40, "over40");
        sat_flag = b.CreateZExt(b.CreateOr(under, over), i32ty, "acc_sat");
        new_acc = emit_smin(emit_smax(new_acc, lo40), hi40);
    }

    // Set AV[which] and AVS[which] (sticky) based on accumulator saturation
    if (update_acc) {
        size_t av_off  = (which == 0) ? offsetof(CpuState, av0)  : offsetof(CpuState, av1);
        size_t avs_off = (which == 0) ? offsetof(CpuState, av0s) : offsetof(CpuState, av1s);
        store_cpu_u32(av_off, sat_flag);
        auto* avs_cur = load_cpu_u32(avs_off, "avs_cur");
        store_cpu_u32(avs_off, b.CreateOr(avs_cur, sat_flag));
    }

    return sat_flag;
}

// ---------------------------------------------------------------------------
// dsp32mac helper: emit IR for one MAC side (accumulate + extract).
//
// which   : 0 = A0, 1 = A1
// op      : 0=overwrite, 1=+=, 2=-=,  3=don't-multiply (just extract acc)
// h0,h1   : select hi(1) or lo(0) half of src0/src1 for the multiply
// mmod    : mode constant (M_IS=8 → signed, no fractional shift, sat16)
// MM      : mixed-sign override (0 = normal)
// fullword: 0 → extract 16-bit saturated half, 1 → extract 32-bit
//
// Returns an i32 Value with the extracted result:
//   - fullword=0: 16-bit saturated value in bits[15:0]
//   - fullword=1: 32-bit saturated value
// Also writes updated ax/aw back to CpuState.
// ---------------------------------------------------------------------------
llvm::Value* LiftVisitor::emit_mac_common(
    uint32_t which, uint32_t op,
    uint32_t h0, uint32_t h1,
    uint32_t src0reg, uint32_t src1reg,
    uint32_t mmod, uint32_t MM,
    bool fullword, llvm::Value** v_out, bool update_acc)
{
    auto& b = builder_;
    auto* i32ty = b.getInt32Ty();
    auto* i64ty = b.getInt64Ty();
    auto& ctx = module_->getContext();

    bool is_signed = is_macmod_signed(mmod);

    auto* ax_val = load_cpu_u32(offsetof(CpuState, ax[0]) + which * 4, "ax");
    auto* aw_val = load_cpu_u32(offsetof(CpuState, aw[0]) + which * 4, "aw");
    bool use_unext = (!MM && is_macmod_unsigned_acc(mmod));
    llvm::Value* acc;
    if (use_unext) {
        auto* ax8  = b.CreateAnd(ax_val, b.getInt32(0xFF), "ax8");
        auto* ax64 = b.CreateZExt(ax8, b.getInt64Ty(), "ax64");
        auto* aw64 = b.CreateZExt(aw_val, b.getInt64Ty(), "aw64");
        acc = b.CreateOr(b.CreateShl(ax64, b.getInt64(32)), aw64, "acc_unext");
    } else {
        acc = build_acc_i64(ax_val, aw_val);
    }

    // acc_sat_flag: 1 if the 40-bit accumulator was clamped (drives V for fullword,
    // and selects nosat_acc for halfword V computation). See refs/bfin-sim.c decode_macfunc.
    llvm::Value* acc_sat_flag = b.getInt32(0);
    // nosat_acc: accumulator value before 40-bit saturation clamping (used for halfword V).
    llvm::Value* nosat_acc = acc;

    if (op != 3) {
        // Load src halves
        auto* s0_32 = load_dreg(src0reg, "s0_32");
        auto* s1_32 = load_dreg(src1reg, "s1_32");

        // Select hi or lo 16-bit half
        llvm::Value* s0_16 = h0 ? b.CreateLShr(s0_32, b.getInt32(16), "s0_hi") : s0_32;
        llvm::Value* s1_16 = h1 ? b.CreateLShr(s1_32, b.getInt32(16), "s1_hi") : s1_32;
        s0_16 = b.CreateAnd(s0_16, b.getInt32(0xFFFF), "s0_16");
        s1_16 = b.CreateAnd(s1_16, b.getInt32(0xFFFF), "s1_16");

        // Sign- or zero-extend to i64
        // Reference decode_multfunc rules:
        //   MM=1: s0 sign-extended (mixed mode: signed * unsigned), s1 zero-extended
        //   MM=0 with signed mmod: both sign-extended
        //   MM=0 with FU/TFU/IU: both zero-extended
        llvm::Value* s0_64, *s1_64;
        if (MM) {
            // Mixed mode: s0 signed, s1 unsigned
            s0_64 = b.CreateSExt(b.CreateTrunc(s0_16, llvm::Type::getInt16Ty(ctx)), i64ty, "s0_se");
            s1_64 = b.CreateZExt(s1_16, i64ty, "s1_ze");
        } else if (is_signed) {
            s0_64 = b.CreateSExt(b.CreateTrunc(s0_16, llvm::Type::getInt16Ty(ctx)), i64ty, "s0_se");
            s1_64 = b.CreateSExt(b.CreateTrunc(s1_16, llvm::Type::getInt16Ty(ctx)), i64ty, "s1_se");
        } else {
            s0_64 = b.CreateZExt(s0_16, i64ty, "s0_ze");
            s1_64 = b.CreateZExt(s1_16, i64ty, "s1_ze");
        }

        auto* prod64 = b.CreateMul(s0_64, s1_64, "prod");

        // W32: capture sgn40 (bit 39 of acc BEFORE accumulation) per refs/bfin-sim.c decode_macfunc.
        // tsat_flag: set when the product was clamped from 0x40000000 to 0x7FFFFFFF (W32 frac sat).
        auto* sgn40 = b.CreateTrunc(b.CreateLShr(acc, b.getInt64(39)),
                                    llvm::Type::getInt1Ty(ctx), "sgn40");
        llvm::Value* tsat_flag = b.getInt1(false);

        // Fractional left-shift (×2) for modes 0, T, S2RND, W32
        bool do_frac_shift = (!MM && (mmod == 0 || mmod == MMOD_T || mmod == MMOD_S2RND || mmod == MMOD_W32));
        if (do_frac_shift) {
            auto* sat_val = (mmod == MMOD_W32)
                ? b.getInt64(0x7fffffffLL)
                : b.getInt64(0x80000000LL);
            auto* is_sat  = b.CreateICmpEQ(prod64, b.getInt64(0x40000000), "is_sat");
            auto* shifted = b.CreateShl(prod64, b.getInt64(1), "frac_shift");
            prod64 = b.CreateSelect(is_sat, sat_val, shifted, "prod_frac");
            if (mmod == MMOD_W32)
                tsat_flag = is_sat;
        }

        // Accumulate
        llvm::Value* new_acc;
        switch (op) {
            case 0:  new_acc = prod64; break;
            case 1:  new_acc = b.CreateAdd(acc, prod64, "acc_add"); break;
            case 2:  new_acc = b.CreateSub(acc, prod64, "acc_sub"); break;
            default: new_acc = acc; break;
        }

        // Saturate accumulator
        nosat_acc = new_acc;
        acc_sat_flag = emit_saturate_acc(new_acc, tsat_flag, sgn40,
                                         mmod, MM, which, update_acc);
        acc = new_acc;

        // Write back accumulator (skip for pure multiply operations like dsp32mult)
        if (update_acc) {
            auto* ax_new = b.CreateTrunc(b.CreateLShr(acc, b.getInt64(32)), i32ty, "ax_new");
            store_cpu_u32(offsetof(CpuState, ax[0]) + which * 4,
                          b.CreateAnd(ax_new, b.getInt32(0xFF), "ax_8"));
            store_cpu_u32(offsetof(CpuState, aw[0]) + which * 4,
                          b.CreateTrunc(acc, i32ty, "aw_new"));
        }
    }

    // Extract result from accumulator
    // sat_nonzero: runtime boolean selecting nosat_acc vs acc for halfword V computation.
    // Per refs/bfin-sim.c decode_macfunc: when sat fired on the accumulator,
    //   - fullword: *overflow = 1 directly (V = acc_sat)
    //   - halfword: ov = extract_mult(nosat_acc, ...) to determine V (not acc)
    auto* sat_nonzero = b.CreateICmpNE(acc_sat_flag, b.getInt32(0), "sat_nonzero");

    llvm::Value* extract_result = nullptr;
    if (!fullword) {
        // For halfword: when acc was saturated, compute V from nosat_acc extraction;
        // otherwise compute V from acc extraction. Result always comes from acc.
        llvm::Value* ov_from_acc = b.getInt32(0);
        llvm::Value* ov_from_nosat = b.getInt32(0);

        // Step A: apply mode-specific pre-saturation transform to both values
        auto apply_hw_transform = [&](llvm::Value* v) -> llvm::Value* {
            if (mmod == MMOD_IS || mmod == MMOD_IU)
                return v;                                            // identity
            if (mmod == MMOD_ISS2)
                return b.CreateShl(v, b.getInt64(1));               // <<1
            if (mmod == MMOD_T || mmod == MMOD_TFU)
                return emit_trunc16(v);                              // trunc16
            if (mmod == MMOD_S2RND)
                return emit_rnd16(b.CreateShl(v, b.getInt64(1)));   // rnd16(<<1)
            return emit_rnd16(v);                                    // mmod 0/FU/W32/IH
        };
        auto* xformed_acc   = apply_hw_transform(acc);
        auto* xformed_nosat = apply_hw_transform(nosat_acc);

        // Step B: apply saturation (signed or unsigned depending on mmod+MM)
        bool use_u16 = !MM && is_macmod_unsigned_acc(mmod);
        if (use_u16) {
            extract_result = emit_sat_u16(xformed_acc,   &ov_from_acc);
            emit_sat_u16(xformed_nosat, &ov_from_nosat);
        } else {
            extract_result = emit_sat_s16(xformed_acc,   &ov_from_acc);
            emit_sat_s16(xformed_nosat, &ov_from_nosat);
        }

        // When acc saturation fired, V = ov_from_nosat OR ov_from_acc (OR-accumulate).
        // When acc saturation didn't fire, V = ov_from_acc.
        auto* ov = b.CreateOr(ov_from_acc,
            b.CreateAnd(b.CreateZExt(sat_nonzero, b.getInt32Ty()), ov_from_nosat),
            "ov16");
        if (v_out) *v_out = ov;
        return extract_result;
    } else {
        // Fullword extraction
        llvm::Value* ext_ov = b.getInt32(0);
        if (mmod == 0 || mmod == MMOD_IS) {
            extract_result = emit_sat_s32(acc, &ext_ov);
        } else if (mmod == MMOD_ISS2 || mmod == MMOD_S2RND) {
            extract_result = emit_sat_s32(b.CreateShl(acc, b.getInt64(1)), &ext_ov);
        } else if (mmod == MMOD_FU || mmod == MMOD_IU) {
            if (MM)
                extract_result = emit_sat_s32(acc, &ext_ov);
            else
                extract_result = emit_sat_u32(acc, &ext_ov);
        } else {
            // W32, IH, TFU, and others: truncate (no overflow)
            extract_result = b.CreateTrunc(acc, i32ty, "res32_trunc");
        }
        // V = acc saturation flag OR extraction overflow (both drive V for fullword).
        if (v_out) *v_out = b.CreateOr(acc_sat_flag, ext_ov, "ov32");
        return extract_result;
    }
}

void LiftVisitor::store_dreg_lo(uint32_t dst, llvm::Value* val16) {
    auto* old = load_dreg(dst, "old_rd");
    auto* hi  = builder_.CreateAnd(old, builder_.getInt32(0xFFFF0000), "rd_hi");
    auto* lo  = builder_.CreateAnd(val16, builder_.getInt32(0x0000FFFF), "val_lo");
    store_dreg(dst, builder_.CreateOr(hi, lo, "new_rd"));
}

void LiftVisitor::store_dreg_hi(uint32_t dst, llvm::Value* val16) {
    auto* old = load_dreg(dst, "old_rd");
    auto* lo  = builder_.CreateAnd(old, builder_.getInt32(0x0000FFFF), "rd_lo");
    auto* hi  = builder_.CreateShl(builder_.CreateAnd(val16, builder_.getInt32(0xFFFF)),
                                   builder_.getInt32(16), "val_hi");
    store_dreg(dst, builder_.CreateOr(lo, hi, "new_rd"));
}

bool LiftVisitor::emit_dsp32mac(
    uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11,
    uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1,
    bool w0, bool w1, bool P, bool is_mult)
{
    uint32_t h00 = (h00h10 >> 1) & 1;
    uint32_t h10 = (h00h10 >> 0) & 1;
    uint32_t h01 = (h01h11 >> 1) & 1;
    uint32_t h11 = (h01h11 >> 0) & 1;

    // For dsp32mult, do not write back to accumulator registers
    bool update_acc = !is_mult;

    llvm::Value* v0 = nullptr, *v1 = nullptr;
    // ARCHITECTURAL RULE: A0 operation always uses MM=0; MM only affects A1.
    // See decode_dsp32mac_0 in refs/bfin-sim.c: A1 uses the instruction's MM bit,
    // but A0 is called with MM=0 unconditionally.
    //
    // Reference guard: skip emit_mac_common entirely when w==0 && op==3 (pure no-op side).
    // Also: when w==0 (not writing result), v must be forced to 0 regardless.
    // See refs/bfin-sim.c decode_dsp32mac_0:
    //   if (w0 == 1 || op0 != 3) { ... if (!w0) v_0 = 0; }
    //   v_0/v_1 are initialized to 0 and stay 0 when the guard is false.
    llvm::Value* res0 = nullptr;
    if (w0 || op0 != 3)
        res0 = emit_mac_common(0, op0, h00, h10, src0, src1, mmod, /*MM=*/0, P, &v0, update_acc);
    if (!res0) res0 = builder_.getInt32(0);
    if (!w0)        v0 = builder_.getInt32(0);  // v_0=0 when not writing result
    else if (!v0)   v0 = builder_.getInt32(0);  // op=3 path sets no v_out

    llvm::Value* res1 = nullptr;
    if (w1 || op1 != 3)
        res1 = emit_mac_common(1, op1, h01, h11, src0, src1, mmod, MM, P, &v1, update_acc);
    if (!res1) res1 = builder_.getInt32(0);
    if (!w1)        v1 = builder_.getInt32(0);  // v_1=0 when not writing result
    else if (!v1)   v1 = builder_.getInt32(0);  // op=3 path sets no v_out

    if (!P) {
        if (w0) store_dreg_lo(dst, res0);
        if (w1) store_dreg_hi(dst, res1);
    } else {
        if (w0) store_dreg(dst,     res0);
        if (w1) store_dreg(dst + 1, res1);
    }

    // Set V/V_COPY/VS flags if any result had overflow
    // For P=1, reference always updates V/VS (even when w0==w1==0, which forces v=0)
    if (w0 || w1 || P) {
        auto* v_new = builder_.CreateOr(v0, v1, "v_new");
        store_v(v_new);
        auto* vs_cur = load_cpu_u32(offsetof(CpuState, vs), "vs_cur");
        store_cpu_u32(offsetof(CpuState, vs), builder_.CreateOr(vs_cur, v_new));
    }

    // AZ/AN are only updated when writing directly from accumulator (op==3).
    // Per refs/bfin-sim.c decode_dsp32mac_0: SET_ASTATREG(az, zero); SET_ASTATREG(an, n_1|n_0)
    // n_0/n_1 are set in decode_macfunc when (ret & 0x8000) for halfword or (ret & 0x80000000) for full.
    if ((w0 && op0 == 3) || (w1 && op1 == 3)) {
        auto* i32ty = builder_.getInt32Ty();
        llvm::Value* az = builder_.getInt32(0);
        llvm::Value* an = builder_.getInt32(0);
        if (w0 && op0 == 3) {
            auto* sign_bit = P ? builder_.getInt32(0x80000000u) : builder_.getInt32(0x8000u);
            an = builder_.CreateOr(an, builder_.CreateZExt(
                builder_.CreateICmpNE(builder_.CreateAnd(res0, sign_bit), builder_.getInt32(0)), i32ty));
            az = builder_.CreateOr(az, builder_.CreateZExt(
                builder_.CreateICmpEQ(res0, builder_.getInt32(0)), i32ty));
        }
        if (w1 && op1 == 3) {
            auto* sign_bit = P ? builder_.getInt32(0x80000000u) : builder_.getInt32(0x8000u);
            an = builder_.CreateOr(an, builder_.CreateZExt(
                builder_.CreateICmpNE(builder_.CreateAnd(res1, sign_bit), builder_.getInt32(0)), i32ty));
            az = builder_.CreateOr(az, builder_.CreateZExt(
                builder_.CreateICmpEQ(res1, builder_.getInt32(0)), i32ty));
        }
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
    }
    return true;
}

bool LiftVisitor::decode_dsp32mac_P0_nn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (op1 == 3 && op0 == 3) return false;
    if (((1 << mmod) & 0x1b5f) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,false,false,false);
}

bool LiftVisitor::decode_dsp32mac_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (mmod == 3 || ((1 << mmod) & 0x1b5f) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,false,true,false);
}

bool LiftVisitor::decode_dsp32mac_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (mmod == 3 || ((1 << mmod) & 0x1b5f) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,true,false,false);
}

bool LiftVisitor::decode_dsp32mac_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (mmod == 3 || ((1 << mmod) & 0x1b5f) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,true,true,false);
}

bool LiftVisitor::decode_dsp32mac_P1_nn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (op1 == 3 && op0 == 3) return false;
    if (((1 << mmod) & 0x131b) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,false,false,true);
}

bool LiftVisitor::decode_dsp32mac_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (mmod == 3 || ((1 << mmod) & 0x131b) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,false,true,true);
}

bool LiftVisitor::decode_dsp32mac_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (mmod == 3 || ((1 << mmod) & 0x131b) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,true,false,true);
}

bool LiftVisitor::decode_dsp32mac_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (mmod == 3 || ((1 << mmod) & 0x131b) == 0) return false;
    return emit_dsp32mac(M,mmod,MM,op1,h01h11,op0,h00h10,dst,src0,src1,true,true,true);
}
bool LiftVisitor::decode_dsp32mult_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x1b57) == 0) return false;
    return emit_dsp32mac(M, mmod, MM, 0, h01h11, 0, h00h10, dst, src0, src1,
                         /*w0=*/false, /*w1=*/true, /*P=*/false, /*is_mult=*/true);
}
bool LiftVisitor::decode_dsp32mult_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x1b57) == 0) return false;
    return emit_dsp32mac(M, mmod, MM, 0, h01h11, 0, h00h10, dst, src0, src1,
                         /*w0=*/true, /*w1=*/false, /*P=*/false, /*is_mult=*/true);
}
bool LiftVisitor::decode_dsp32mult_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x1b57) == 0) return false;
    return emit_dsp32mac(M, mmod, MM, 0, h01h11, 0, h00h10, dst, src0, src1,
                         /*w0=*/true, /*w1=*/true, /*P=*/false, /*is_mult=*/true);
}
bool LiftVisitor::decode_dsp32mult_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x313) == 0) return false;
    return emit_dsp32mac(M, mmod, MM, 0, h01h11, 0, h00h10, dst, src0, src1,
                         /*w0=*/false, /*w1=*/true, /*P=*/true, /*is_mult=*/true);
}
bool LiftVisitor::decode_dsp32mult_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x313) == 0) return false;
    return emit_dsp32mac(M, mmod, MM, 0, h01h11, 0, h00h10, dst, src0, src1,
                         /*w0=*/true, /*w1=*/false, /*P=*/true, /*is_mult=*/true);
}
bool LiftVisitor::decode_dsp32mult_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10,
    uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x313) == 0) return false;
    return emit_dsp32mac(M, mmod, MM, 0, h01h11, 0, h00h10, dst, src0, src1,
                         /*w0=*/true, /*w1=*/true, /*P=*/true, /*is_mult=*/true);
}

// dsp32alu stubs (remaining)
bool LiftVisitor::emit_addsubpair(bool hi_is_sub, bool lo_is_sub,
                                   uint32_t s, uint32_t x,
                                   uint32_t dst0, uint32_t src0, uint32_t src1) {
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");

    // Sign-extend each 16-bit half to 32 bits
    auto* rs_l = builder_.CreateSExt(
        builder_.CreateTrunc(rs, builder_.getInt16Ty()), builder_.getInt32Ty(), "rs_l");
    auto* rs_h = builder_.CreateSExt(
        builder_.CreateTrunc(builder_.CreateLShr(rs, 16), builder_.getInt16Ty()),
        builder_.getInt32Ty(), "rs_h");
    auto* rt_l = builder_.CreateSExt(
        builder_.CreateTrunc(rt, builder_.getInt16Ty()), builder_.getInt32Ty(), "rt_l");
    auto* rt_h = builder_.CreateSExt(
        builder_.CreateTrunc(builder_.CreateLShr(rt, 16), builder_.getInt16Ty()),
        builder_.getInt32Ty(), "rt_h");

    auto* sum_h = hi_is_sub ? builder_.CreateSub(rs_h, rt_h, "sum_h")
                            : builder_.CreateAdd(rs_h, rt_h, "sum_h");
    auto* sum_l = lo_is_sub ? builder_.CreateSub(rs_l, rt_l, "sum_l")
                            : builder_.CreateAdd(rs_l, rt_l, "sum_l");

    // 16-bit signed overflow detection
    auto* ov_h = builder_.CreateOr(
        builder_.CreateICmpSGT(sum_h, builder_.getInt32(0x7FFF)),
        builder_.CreateICmpSLT(sum_h, builder_.getInt32(static_cast<uint32_t>(-32768))));
    auto* ov_l = builder_.CreateOr(
        builder_.CreateICmpSGT(sum_l, builder_.getInt32(0x7FFF)),
        builder_.CreateICmpSLT(sum_l, builder_.getInt32(static_cast<uint32_t>(-32768))));
    auto* v_any = builder_.CreateOr(ov_h, ov_l);

    // Saturate when s==1 (static at JIT time)
    llvm::Value* res_h = sum_h;
    llvm::Value* res_l = sum_l;
    if (s) {
        auto* sat_max = builder_.getInt32(0x7FFF);
        auto* sat_min = builder_.getInt32(static_cast<uint32_t>(-32768));
        res_h = emit_smin(emit_smax(sum_h, sat_min), sat_max);
        res_l = emit_smin(emit_smax(sum_l, sat_min), sat_max);
    }

    // Mask to 16 bits and pack (x=1 swaps halves)
    auto* t0 = builder_.CreateAnd(res_h, builder_.getInt32(0xFFFF));
    auto* t1 = builder_.CreateAnd(res_l, builder_.getInt32(0xFFFF));
    llvm::Value* packed = x
        ? builder_.CreateOr(builder_.CreateShl(t1, 16), t0)  // CO: swap halves
        : builder_.CreateOr(builder_.CreateShl(t0, 16), t1); // normal
    store_dreg(dst0, packed);

    // ASTAT flags
    auto* az_l  = builder_.CreateICmpEQ(t1, builder_.getInt32(0));
    auto* az_h  = builder_.CreateICmpEQ(t0, builder_.getInt32(0));
    auto* az    = builder_.CreateZExt(builder_.CreateOr(az_l, az_h), builder_.getInt32Ty(), "az");
    auto* an_l  = builder_.CreateICmpSGT(t1, builder_.getInt32(0x7FFF));
    auto* an_h  = builder_.CreateICmpSGT(t0, builder_.getInt32(0x7FFF));
    auto* an    = builder_.CreateZExt(builder_.CreateOr(an_l, an_h), builder_.getInt32Ty(), "an");

    auto* rs_l_u = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF));
    auto* rt_l_u = builder_.CreateAnd(rt, builder_.getInt32(0xFFFF));
    auto* rs_h_u = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF));
    auto* rt_h_u = builder_.CreateAnd(builder_.CreateLShr(rt, 16), builder_.getInt32(0xFFFF));

    // AC0 carry: add → (~rs_l & 0xFFFF) < rt_l;  sub → rt_l <= rs_l
    llvm::Value* ac0;
    if (lo_is_sub)
        ac0 = builder_.CreateZExt(builder_.CreateICmpULE(rt_l_u, rs_l_u), builder_.getInt32Ty(), "ac0");
    else
        ac0 = builder_.CreateZExt(builder_.CreateICmpULT(
            builder_.CreateAnd(builder_.CreateNot(rs_l_u), builder_.getInt32(0xFFFF)), rt_l_u),
            builder_.getInt32Ty(), "ac0");

    // AC1 carry: add → (~rs_h & 0xFFFF) < rt_h;  sub → rt_h <= rs_h
    llvm::Value* ac1;
    if (hi_is_sub)
        ac1 = builder_.CreateZExt(builder_.CreateICmpULE(rt_h_u, rs_h_u), builder_.getInt32Ty(), "ac1");
    else
        ac1 = builder_.CreateZExt(builder_.CreateICmpULT(
            builder_.CreateAnd(builder_.CreateNot(rs_h_u), builder_.getInt32(0xFFFF)), rt_h_u),
            builder_.getInt32Ty(), "ac1");

    auto* v_flag = builder_.CreateZExt(v_any, builder_.getInt32Ty(), "v");
    auto* vs_new = builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag);

    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_ac0(ac0);
    store_cpu_u32(offsetof(CpuState, ac1), ac1);
    store_v(v_flag);
    store_cpu_u32(offsetof(CpuState, vs), vs_new);
    return true;
}
bool LiftVisitor::decode_dsp32alu_ADDADD(uint32_t M, uint32_t H, uint32_t s, uint32_t x,
                                          uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_addsubpair(false, false, s, x, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_ADDSUB(uint32_t M, uint32_t H, uint32_t s, uint32_t x,
                                          uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_addsubpair(false, true, s, x, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_SUBADD(uint32_t M, uint32_t H, uint32_t s, uint32_t x,
                                          uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_addsubpair(true, false, s, x, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_SUBSUB(uint32_t M, uint32_t H, uint32_t s, uint32_t x,
                                          uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_addsubpair(true, true, s, x, dst0, src0, src1);
}
bool LiftVisitor::emit_quadadd(uint32_t HL, uint32_t aop, uint32_t s, uint32_t x,
                                uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    using Value = llvm::Value;
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");

    // Extract 4 sign-extended 16-bit halves
    auto* rs_h = emit_sext_half16(rs, true);
    auto* rs_l = emit_sext_half16(rs, false);
    auto* rt_h = emit_sext_half16(rt, true);
    auto* rt_l = emit_sext_half16(rt, false);

    // Scale helper: apply aop to a 32-bit result before saturation
    auto scale16 = [&](Value* v) -> Value* {
        if (aop == 2) return builder_.CreateAShr(v, 1);
        if (aop == 3) return builder_.CreateShl(v,  1);
        return v;
    };

    // Saturation helper (signed 16-bit)
    auto sat16 = [&](Value* v) -> Value* {
        if (!s) return v;
        auto* mx = builder_.getInt32(0x7FFF);
        auto* mn = builder_.getInt32(static_cast<uint32_t>(-32768));
        return emit_smin(emit_smax(v, mn), mx);
    };

    Value *d1h_raw, *d1l_raw, *d0h_raw, *d0l_raw;
    if (HL == 0) {
        d1h_raw = builder_.CreateAdd(rs_h, rt_h, "d1h");
        d1l_raw = builder_.CreateAdd(rs_l, rt_l, "d1l");
        d0h_raw = builder_.CreateSub(rs_h, rt_h, "d0h");
        d0l_raw = builder_.CreateSub(rs_l, rt_l, "d0l");
    } else {
        d1h_raw = builder_.CreateAdd(rs_h, rt_h, "d1h");
        d1l_raw = builder_.CreateSub(rs_l, rt_l, "d1l");
        d0h_raw = builder_.CreateSub(rs_h, rt_h, "d0h");
        d0l_raw = builder_.CreateAdd(rs_l, rt_l, "d0l");
    }

    auto* d1h = sat16(scale16(d1h_raw));
    auto* d1l = sat16(scale16(d1l_raw));
    auto* d0h = sat16(scale16(d0h_raw));
    auto* d0l = sat16(scale16(d0l_raw));

    auto* m16 = builder_.getInt32(0xFFFF);
    auto* d1h_m = builder_.CreateAnd(d1h, m16);
    auto* d1l_m = builder_.CreateAnd(d1l, m16);
    auto* d0h_m = builder_.CreateAnd(d0h, m16);
    auto* d0l_m = builder_.CreateAnd(d0l, m16);

    auto* rd1 = builder_.CreateOr(builder_.CreateShl(d1h_m, 16), d1l_m, "rd1");
    auto* rd0 = x
        ? builder_.CreateOr(builder_.CreateShl(d0l_m, 16), d0h_m, "rd0_swapped")
        : builder_.CreateOr(builder_.CreateShl(d0h_m, 16), d0l_m, "rd0");

    store_dreg(dst0, rd0);
    store_dreg(dst1, rd1);

    auto bit15 = [&](Value* v) -> Value* {
        return builder_.CreateICmpSGT(v, builder_.getInt32(0x7FFF));
    };
    auto* an_val = builder_.CreateZExt(
        builder_.CreateOr(builder_.CreateOr(builder_.CreateOr(bit15(d1h_m), bit15(d1l_m)),
                                             bit15(d0h_m)), bit15(d0l_m)),
        builder_.getInt32Ty(), "an");

    auto is_zero = [&](Value* v) -> Value* {
        return builder_.CreateICmpEQ(v, builder_.getInt32(0));
    };
    auto* az_val = builder_.CreateZExt(
        builder_.CreateOr(builder_.CreateOr(builder_.CreateOr(is_zero(d1h_m), is_zero(d1l_m)),
                                             is_zero(d0h_m)), is_zero(d0l_m)),
        builder_.getInt32Ty(), "az");

    auto ovf = [&](Value* raw_scaled, Value* /*sat_result*/) -> Value* {
        auto* too_big   = builder_.CreateICmpSGT(raw_scaled, builder_.getInt32(0x7FFF));
        auto* too_small = builder_.CreateICmpSLT(raw_scaled, builder_.getInt32(static_cast<uint32_t>(-32768)));
        return builder_.CreateOr(too_big, too_small);
    };
    auto* v_val = builder_.CreateZExt(
        builder_.CreateOr(builder_.CreateOr(builder_.CreateOr(
            ovf(scale16(d1h_raw), d1h), ovf(scale16(d1l_raw), d1l)),
            ovf(scale16(d0h_raw), d0h)), ovf(scale16(d0l_raw), d0l)),
        builder_.getInt32Ty(), "v");

    auto* vs_cur = load_cpu_u32(offsetof(CpuState, vs), "vs_cur");
    store_cpu_u32(offsetof(CpuState, az), az_val);
    store_cpu_u32(offsetof(CpuState, an), an_val);
    store_v(v_val);
    store_cpu_u32(offsetof(CpuState, vs), builder_.CreateOr(vs_cur, v_val));
    return true;
}
bool LiftVisitor::decode_dsp32alu_QUADADD_HL0(uint32_t M, uint32_t aop, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_quadadd(0, aop, s, x, dst0, dst1, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_QUADADD_HL1(uint32_t M, uint32_t aop, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_quadadd(1, aop, s, x, dst0, dst1, src0, src1);
}
// Shared kernel for dsp32alu ADD16/SUB16 (aopcde=2/3)
// is_sub: false=ADD, true=SUB
// dst_hi: false=write result to Rd.l, true=Rd.h
// src0_hi/src1_hi: false=use .l half, true=.h half
bool LiftVisitor::emit_add_sub16(bool is_sub, bool dst_hi,
                                  bool src0_hi, bool src1_hi,
                                  uint32_t s, uint32_t dst0,
                                  uint32_t src0, uint32_t src1) {
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");

    // Extract requested 16-bit half, sign-extended to 32 bits
    auto extract_half = [&](llvm::Value* r, bool hi) -> llvm::Value* {
        auto* shifted = hi ? builder_.CreateLShr(r, 16) : r;
        return builder_.CreateSExt(
            builder_.CreateTrunc(shifted, builder_.getInt16Ty()),
            builder_.getInt32Ty());
    };
    auto* a = extract_half(rs, src0_hi);
    auto* b = extract_half(rt, src1_hi);

    // Bit 15 of each operand (sign flag)
    auto* a_neg = builder_.CreateLShr(builder_.CreateAnd(a, builder_.getInt32(0x8000)), 15);
    auto* b_neg = builder_.CreateLShr(builder_.CreateAnd(b, builder_.getInt32(0x8000)), 15);

    llvm::Value* result = is_sub
        ? builder_.CreateSub(a, b, "diff16")
        : builder_.CreateAdd(a, b, "sum16");

    auto* res_neg = builder_.CreateLShr(
        builder_.CreateAnd(result, builder_.getInt32(0x8000)), 15);

    // Overflow detection (different formula for add vs sub)
    llvm::Value* overflow;
    if (!is_sub) {
        // ADD: (a_neg ^ res_neg) & (b_neg ^ res_neg)
        overflow = builder_.CreateAnd(builder_.CreateXor(a_neg, res_neg),
                                      builder_.CreateXor(b_neg, res_neg));
    } else {
        // SUB: (a_neg ^ b_neg) & (res_neg ^ a_neg)
        overflow = builder_.CreateAnd(builder_.CreateXor(a_neg, b_neg),
                                      builder_.CreateXor(res_neg, a_neg));
    }

    // Optional saturation (s is static at JIT compile time)
    llvm::Value* val = result;
    if (s) {
        auto* sat_max = builder_.getInt32(0x7FFF);
        auto* sat_min = builder_.getInt32(static_cast<uint32_t>(-32768));
        val = emit_smin(emit_smax(result, sat_min), sat_max);
    }

    // Mask to 16 bits and write to destination half
    auto* val16 = builder_.CreateAnd(val, builder_.getInt32(0xFFFF));
    if (dst_hi)
        store_dreg_hi(dst0, val16);
    else
        store_dreg_lo(dst0, val16);

    // ASTAT: AZ, AN from the 16-bit result
    auto* az = builder_.CreateZExt(
        builder_.CreateICmpEQ(val16, builder_.getInt32(0)),
        builder_.getInt32Ty(), "az");
    auto* an = builder_.CreateLShr(val16, 15, "an"); // bit 15

    // AC0: carry (add) or borrow (sub) on the unsigned 16-bit operands
    auto* a_u = builder_.CreateAnd(
        src0_hi ? builder_.CreateLShr(rs, 16) : rs,
        builder_.getInt32(0xFFFF));
    auto* b_u = builder_.CreateAnd(
        src1_hi ? builder_.CreateLShr(rt, 16) : rt,
        builder_.getInt32(0xFFFF));
    llvm::Value* ac0;
    if (!is_sub) {
        // carry: (~a & 0xFFFF) < b
        ac0 = builder_.CreateZExt(
            builder_.CreateICmpULT(
                builder_.CreateAnd(builder_.CreateNot(a_u), builder_.getInt32(0xFFFF)),
                b_u),
            builder_.getInt32Ty(), "ac0");
    } else {
        // borrow: b <= a
        ac0 = builder_.CreateZExt(
            builder_.CreateICmpULE(b_u, a_u),
            builder_.getInt32Ty(), "ac0");
    }

    auto* v_flag = builder_.CreateZExt(overflow, builder_.getInt32Ty(), "v");
    auto* vs_new = builder_.CreateOr(
        load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag);

    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_ac0(ac0);
    store_v(v_flag);
    store_cpu_u32(offsetof(CpuState, vs), vs_new);
    return true;
}

// ADD16 HLd0 — result written to Rd.l
#define ADD16_HLd0(sfx, s0hi, s1hi) \
bool LiftVisitor::decode_dsp32alu_ADD16_HLd0_##sfx( \
    uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, \
    uint32_t src0, uint32_t src1) \
{ return emit_add_sub16(false, false, s0hi, s1hi, s, dst0, src0, src1); }
ADD16_HLd0(LL, false, false)
ADD16_HLd0(LH, false, true)
ADD16_HLd0(HL, true,  false)
ADD16_HLd0(HH, true,  true)
#undef ADD16_HLd0

// ADD16 HLd1 — result written to Rd.h
#define ADD16_HLd1(sfx, s0hi, s1hi) \
bool LiftVisitor::decode_dsp32alu_ADD16_HLd1_##sfx( \
    uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, \
    uint32_t src0, uint32_t src1) \
{ return emit_add_sub16(false, true, s0hi, s1hi, s, dst0, src0, src1); }
ADD16_HLd1(LL, false, false)
ADD16_HLd1(LH, false, true)
ADD16_HLd1(HL, true,  false)
ADD16_HLd1(HH, true,  true)
#undef ADD16_HLd1

// SUB16 HLd0 — result written to Rd.l
#define SUB16_HLd0(sfx, s0hi, s1hi) \
bool LiftVisitor::decode_dsp32alu_SUB16_HLd0_##sfx( \
    uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, \
    uint32_t src0, uint32_t src1) \
{ return emit_add_sub16(true, false, s0hi, s1hi, s, dst0, src0, src1); }
SUB16_HLd0(LL, false, false)
SUB16_HLd0(LH, false, true)
SUB16_HLd0(HL, true,  false)
SUB16_HLd0(HH, true,  true)
#undef SUB16_HLd0

// SUB16 HLd1 — result written to Rd.h
#define SUB16_HLd1(sfx, s0hi, s1hi) \
bool LiftVisitor::decode_dsp32alu_SUB16_HLd1_##sfx( \
    uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, \
    uint32_t src0, uint32_t src1) \
{ return emit_add_sub16(true, true, s0hi, s1hi, s, dst0, src0, src1); }
SUB16_HLd1(LL, false, false)
SUB16_HLd1(LH, false, true)
SUB16_HLd1(HL, true,  false)
SUB16_HLd1(HH, true,  true)
#undef SUB16_HLd1
bool LiftVisitor::emit_addsub32(bool is_sub, uint32_t s,
                                uint32_t dst0, uint32_t src0, uint32_t src1) {
    auto* a = load_dreg(src0, "a");
    auto* b = load_dreg(src1, "b");
    auto* v = is_sub ? builder_.CreateSub(a, b, "diff")
                     : builder_.CreateAdd(a, b, "sum");
    auto* flgs = builder_.CreateICmpSLT(a, builder_.getInt32(0), "flgs");  // i1
    auto* flgo = builder_.CreateICmpSLT(b, builder_.getInt32(0), "flgo");  // i1
    auto* flgn = builder_.CreateICmpSLT(v, builder_.getInt32(0), "flgn");  // i1
    llvm::Value* overflow;
    if (is_sub)
        // sub overflow: different-sign inputs, result sign differs from minuend
        overflow = builder_.CreateAnd(
            builder_.CreateXor(flgs, flgo),
            builder_.CreateXor(flgn, flgs), "overflow");  // i1
    else
        // add overflow: same-sign inputs, result sign differs from operands
        overflow = builder_.CreateAnd(
            builder_.CreateXor(flgs, flgn),
            builder_.CreateXor(flgo, flgn), "overflow");  // i1
    if (s) {
        auto* neg_sat = builder_.getInt32(0x80000000u);
        auto* pos_sat = builder_.getInt32(0x7FFFFFFFu);
        // flgn=1 → result went negative (positive overflow) → clamp to pos_sat
        auto* sat_val = builder_.CreateSelect(flgn, pos_sat, neg_sat);
        v = builder_.CreateSelect(overflow, sat_val, v, "v_sat");
        flgn = builder_.CreateICmpSLT(v, builder_.getInt32(0), "flgn_sat");
    }
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(v, builder_.getInt32(0)),
                                   builder_.getInt32Ty(), "az");
    auto* an = builder_.CreateZExt(flgn, builder_.getInt32Ty(), "an");
    auto* vf = builder_.CreateZExt(overflow, builder_.getInt32Ty(), "v");
    llvm::Value* ac0;
    if (is_sub)
        ac0 = builder_.CreateZExt(builder_.CreateICmpULE(b, a), builder_.getInt32Ty(), "ac0");
    else
        ac0 = builder_.CreateZExt(builder_.CreateICmpULT(builder_.CreateNot(a), b),
                                  builder_.getInt32Ty(), "ac0");
    store_dreg(dst0, v);
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_v(vf);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), vf));
    store_ac0(ac0);
    return true;
}
bool LiftVisitor::decode_dsp32alu_ADD32(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_addsub32(false, s, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_SUB32(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_addsub32(true, s, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_ADDSUB32_dual(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                                uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // dst1 = src0 + src1, dst0 = src0 - src1
    auto* a = load_dreg(src0, "a");
    auto* b = load_dreg(src1, "b");

    // Add result for dst1
    auto* add_v  = builder_.CreateAdd(a, b, "add_v");
    auto* flgs_a = builder_.CreateICmpSLT(a, builder_.getInt32(0), "flgs_a");      // i1
    auto* flgo_a = builder_.CreateICmpSLT(b, builder_.getInt32(0), "flgo_a");      // i1
    auto* flgn_a = builder_.CreateICmpSLT(add_v, builder_.getInt32(0), "flgn_a");  // i1
    auto* ov_add = builder_.CreateAnd(
        builder_.CreateXor(flgs_a, flgn_a),
        builder_.CreateXor(flgo_a, flgn_a), "ov_add");  // i1
    llvm::Value* res_add = add_v;
    if (s) {
        auto* neg_sat = builder_.getInt32(0x80000000u);
        auto* pos_sat = builder_.getInt32(0x7FFFFFFFu);
        // flgn_a=1 means result went negative (positive overflow) → clamp to pos_sat
        auto* sat_a   = builder_.CreateSelect(flgn_a, pos_sat, neg_sat);
        res_add = builder_.CreateSelect(ov_add, sat_a, add_v, "res_add");
    }

    // Sub result for dst0
    auto* sub_v  = builder_.CreateSub(a, b, "sub_v");
    auto* flgs_s = builder_.CreateICmpSLT(a, builder_.getInt32(0), "flgs_s");      // i1
    auto* flgo_s = builder_.CreateICmpSLT(b, builder_.getInt32(0), "flgo_s");      // i1
    auto* flgn_s = builder_.CreateICmpSLT(sub_v, builder_.getInt32(0), "flgn_s");  // i1
    auto* ov_sub = builder_.CreateAnd(
        builder_.CreateXor(flgs_s, flgo_s),
        builder_.CreateXor(flgn_s, flgs_s), "ov_sub");  // i1
    llvm::Value* res_sub = sub_v;
    if (s) {
        auto* neg_sat = builder_.getInt32(0x80000000u);
        auto* pos_sat = builder_.getInt32(0x7FFFFFFFu);
        // flgn_s=1 means result went negative (positive overflow) → clamp to pos_sat
        auto* sat_s   = builder_.CreateSelect(flgn_s, pos_sat, neg_sat);
        res_sub = builder_.CreateSelect(ov_sub, sat_s, sub_v, "res_sub");
    }

    store_dreg(dst1, res_add);
    store_dreg(dst0, res_sub);

    // Parallel flag semantics (matching bfin-sim.c add32/sub32 with parallel=1):
    //   add32 always writes AN/V/AZ/AC0;
    //   sub32(parallel=1) only SETS (never clears): AN if flgn, V if overflow, AZ if zero.
    //   Net result: AN = OR of both sign bits; V = OR of both overflows;
    //               AZ = OR of both zero conditions; AC0 = from add (borrow sense).
    auto* flgn_add_f = builder_.CreateICmpSLT(res_add, builder_.getInt32(0), "flgn_add_f");  // i1
    auto* flgn_sub_f = builder_.CreateICmpSLT(res_sub, builder_.getInt32(0), "flgn_sub_f");  // i1
    auto* an_i1 = builder_.CreateOr(flgn_add_f, flgn_sub_f, "an_i1");
    auto* az_add = builder_.CreateICmpEQ(res_add, builder_.getInt32(0), "az_add");
    auto* az_sub = builder_.CreateICmpEQ(res_sub, builder_.getInt32(0), "az_sub");
    auto* az_i1 = builder_.CreateOr(az_add, az_sub, "az_i1");
    auto* an = builder_.CreateZExt(an_i1, builder_.getInt32Ty(), "an");
    auto* az = builder_.CreateZExt(az_i1, builder_.getInt32Ty(), "az");
    auto* v_any = builder_.CreateOr(ov_add, ov_sub, "v_any");  // i1 Or
    auto* vf  = builder_.CreateZExt(v_any, builder_.getInt32Ty(), "v");
    auto* ac0_add = builder_.CreateICmpULT(builder_.CreateNot(a), b, "ac0_add");  // add carry
    auto* ac0_sub = builder_.CreateICmpULE(b, a, "ac0_sub");                      // sub borrow
    auto* ac0_i1  = builder_.CreateOr(ac0_add, ac0_sub, "ac0_i1");
    auto* ac0 = builder_.CreateZExt(ac0_i1, builder_.getInt32Ty(), "ac0");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_v(vf);
    // VS is sticky: set if any overflow, never cleared by this instruction
    auto* vs_old = load_cpu_u32(offsetof(CpuState, vs), "vs_old");
    store_cpu_u32(offsetof(CpuState, vs), builder_.CreateOr(vs_old, vf, "vs_new"));
    store_ac0(ac0);
    return true;
}
bool LiftVisitor::emit_rnd12(bool is_sub, bool dst_hi,
                              uint32_t dst0, uint32_t src0, uint32_t src1) {
    // Rd.l/.h = Rs +/- Rt (RND12): add, round at bit 11, extract bits[27:12]
    // For sub: clamp-negate Rt (INT_MIN -> INT_MAX) first, then same as add.
    auto* i32  = builder_.getInt32Ty();
    auto* rs   = load_dreg(src0, "rs");
    auto* rt_raw = load_dreg(src1, "rt");
    llvm::Value* rt;
    if (is_sub) {
        auto* is_min = builder_.CreateICmpEQ(rt_raw, builder_.getInt32(0x80000000u));
        auto* neg_rt = builder_.CreateNeg(rt_raw, "neg_rt");
        rt = builder_.CreateSelect(is_min, builder_.getInt32(0x7FFFFFFFu), neg_rt, "rt_adj");
    } else {
        rt = rt_raw;
    }
    auto* sb1  = builder_.CreateLShr(rs, 31, "sb1");
    auto* sb2  = builder_.CreateLShr(rt, 31, "sb2");
    auto* sum  = builder_.CreateAdd(rs, rt, "sum");
    auto* sbR1 = builder_.CreateLShr(sum, 31, "sbR1");
    auto* rnd  = builder_.CreateAdd(sum, builder_.getInt32(0x0800u), "rnd");
    auto* sbR2 = builder_.CreateLShr(rnd, 31, "sbR2");
    auto* sgn  = builder_.CreateAShr(rnd, 27, "sgn");
    auto* same = builder_.CreateICmpEQ(sb1, sb2);
    auto* ov1  = builder_.CreateAnd(same, builder_.CreateICmpNE(sb1, sbR1));
    auto* bp   = builder_.CreateAnd(builder_.CreateICmpEQ(sb1, builder_.getInt32(0)),
                                    builder_.CreateICmpEQ(sb2, builder_.getInt32(0)));
    auto* ov2  = builder_.CreateAnd(bp, builder_.CreateICmpNE(sbR2, builder_.getInt32(0)));
    auto* ov3  = builder_.CreateAnd(builder_.CreateICmpNE(sgn, builder_.getInt32(0)),
                                    builder_.CreateICmpNE(sgn, builder_.getInt32(~0u)));
    auto* ov   = builder_.CreateOr(builder_.CreateOr(ov1, ov2), ov3, "ov");
    auto* no_ov = builder_.CreateAShr(builder_.CreateShl(rnd, 4), 16, "no_ov");
    auto* neg_s = builder_.getInt32(0x8000u);
    auto* pos_s = builder_.getInt32(0x7FFFu);
    auto* mx_s  = builder_.CreateSelect(builder_.CreateICmpNE(sbR1, builder_.getInt32(0)),
                                        neg_s, pos_s);
    auto* bn    = builder_.CreateAnd(builder_.CreateICmpNE(sb1, builder_.getInt32(0)),
                                     builder_.CreateICmpNE(sb2, builder_.getInt32(0)));
    auto* sat   = builder_.CreateSelect(bn, neg_s,
                    builder_.CreateSelect(bp, pos_s, mx_s));
    auto* res   = builder_.CreateSelect(ov, sat, no_ov, "res");
    auto* r16   = builder_.CreateAnd(res, builder_.getInt32(0xFFFF), "r16");
    if (dst_hi) store_dreg_hi(dst0, r16); else store_dreg_lo(dst0, r16);
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(r16, builder_.getInt32(0)), i32, "az");
    auto* an = builder_.CreateZExt(builder_.CreateICmpNE(
        builder_.CreateAnd(r16, builder_.getInt32(0x8000u)), builder_.getInt32(0)), i32, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    auto* vf = builder_.CreateZExt(ov, i32, "v");
    store_v(vf);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs"), vf));
    return true;
}
bool LiftVisitor::decode_dsp32alu_ADD_RND12_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd12(false, false, dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_SUB_RND12_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd12(true,  false, dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_ADD_RND12_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd12(false, true,  dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_SUB_RND12_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd12(true,  true,  dst0, src0, src1); }
bool LiftVisitor::emit_rnd20(bool is_sub, bool dst_hi,
                              uint32_t dst0, uint32_t src0, uint32_t src1) {
    // Rd.l/.h = Rs +/- Rt (RND20): carry-safe >>4 add, round at bit 15, unsigned >>16
    // For sub: plain negate Rt (no INT_MIN clamp, unlike RND12).
    auto* i32  = builder_.getInt32Ty();
    auto* rs   = load_dreg(src0, "rs");
    auto* rt   = is_sub ? builder_.CreateNeg(load_dreg(src1, "rt"), "neg_rt")
                        : load_dreg(src1, "rt");
    auto* rs4  = builder_.CreateAShr(rs, 4, "rs4");
    auto* rt4  = builder_.CreateAShr(rt, 4, "rt4");
    auto* rsL  = builder_.CreateAnd(rs, builder_.getInt32(0xFu));
    auto* rtL  = builder_.CreateAnd(rt, builder_.getInt32(0xFu));
    auto* c4   = builder_.CreateLShr(builder_.CreateAdd(rsL, rtL), 4, "c4");
    auto* sum4 = builder_.CreateAdd(builder_.CreateAdd(rs4, rt4), c4, "sum4");
    auto* rnd  = builder_.CreateAdd(sum4, builder_.getInt32(0x8000u), "rnd");
    auto* res  = builder_.CreateLShr(rnd, 16, "res");
    auto* r16  = builder_.CreateAnd(res, builder_.getInt32(0xFFFF), "r16");
    if (dst_hi) store_dreg_hi(dst0, r16); else store_dreg_lo(dst0, r16);
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(r16, builder_.getInt32(0)), i32, "az");
    auto* an = builder_.CreateZExt(builder_.CreateICmpNE(
        builder_.CreateAnd(r16, builder_.getInt32(0x8000u)), builder_.getInt32(0)), i32, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32alu_ADD_RND20_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd20(false, false, dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_SUB_RND20_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd20(true,  false, dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_ADD_RND20_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd20(false, true,  dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_SUB_RND20_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) { return emit_rnd20(true,  true,  dst0, src0, src1); }
bool LiftVisitor::decode_dsp32alu_VMAX(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                        uint32_t dst0, uint32_t dst1,
                                        uint32_t src0, uint32_t src1) {
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");
    auto* rs_l = emit_sext_half16(rs, false); auto* rt_l = emit_sext_half16(rt, false);
    auto* rs_h = emit_sext_half16(rs, true);  auto* rt_h = emit_sext_half16(rt, true);
    auto* res_l = emit_smax(rs_l, rt_l);
    auto* res_h = emit_smax(rs_h, rt_h);
    // Pack halves into result
    auto* m16 = builder_.getInt32(0xFFFF);
    auto* packed = builder_.CreateOr(
        builder_.CreateShl(builder_.CreateAnd(res_h, m16), 16),
        builder_.CreateAnd(res_l, m16));
    store_dreg(dst0, packed);
    emit_flags_nz_2x16(res_l, res_h);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32alu_VMIN(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                        uint32_t dst0, uint32_t dst1,
                                        uint32_t src0, uint32_t src1) {
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");
    auto* rs_l = emit_sext_half16(rs, false); auto* rt_l = emit_sext_half16(rt, false);
    auto* rs_h = emit_sext_half16(rs, true);  auto* rt_h = emit_sext_half16(rt, true);
    auto* res_l = emit_smin(rs_l, rt_l);
    auto* res_h = emit_smin(rs_h, rt_h);
    auto* m16 = builder_.getInt32(0xFFFF);
    auto* packed = builder_.CreateOr(
        builder_.CreateShl(builder_.CreateAnd(res_h, m16), 16),
        builder_.CreateAnd(res_l, m16));
    store_dreg(dst0, packed);
    emit_flags_nz_2x16(res_l, res_h);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32alu_VABS(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                        uint32_t dst0, uint32_t dst1,
                                        uint32_t src0, uint32_t src1) {
    auto* rs = load_dreg(src0, "rs");
    auto* rs_l = emit_sext_half16(rs, false);
    auto* rs_h = emit_sext_half16(rs, true);
    // ABS each half with saturation: -(0x8000) overflows to 0x8000, clamp to 0x7FFF
    auto abs_half = [&](llvm::Value* v, const char* tag)
        -> std::pair<llvm::Value*, llvm::Value*> {
        auto* av     = emit_abs(v);
        auto* is_min = builder_.CreateICmpEQ(av, builder_.getInt32(0x8000));
        auto* sat    = builder_.CreateSelect(
            is_min, builder_.getInt32(0x7FFF), av,
            (std::string(tag) + "_sat").c_str());
        return {sat, builder_.CreateZExt(is_min, builder_.getInt32Ty())};
    };
    auto [abs_l, v_l] = abs_half(rs_l, "vabs_l");
    auto [abs_h, v_h] = abs_half(rs_h, "vabs_h");
    auto* m16 = builder_.getInt32(0xFFFF);
    auto* packed = builder_.CreateOr(
        builder_.CreateShl(builder_.CreateAnd(abs_h, m16), 16),
        builder_.CreateAnd(abs_l, m16));
    store_dreg(dst0, packed);
    emit_flags_nz_2x16(abs_l, abs_h);
    auto* v_flag = builder_.CreateOr(v_l, v_h, "v");
    store_v(v_flag);
    // VS is sticky
    auto* vs_cur = load_cpu_u32(offsetof(CpuState, vs), "vs_cur");
    store_cpu_u32(offsetof(CpuState, vs), builder_.CreateOr(vs_cur, v_flag));
    return true;
}
bool LiftVisitor::decode_dsp32alu_MAX(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                       uint32_t dst0, uint32_t dst1,
                                       uint32_t src0, uint32_t src1) {
    auto* rs     = load_dreg(src0, "rs");
    auto* rt     = load_dreg(src1, "rt");
    auto* result = emit_smax(rs, rt);
    store_dreg(dst0, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32alu_MIN(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                       uint32_t dst0, uint32_t dst1,
                                       uint32_t src0, uint32_t src1) {
    auto* rs     = load_dreg(src0, "rs");
    auto* rt     = load_dreg(src1, "rt");
    auto* result = emit_smin(rs, rt);
    store_dreg(dst0, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32alu_ABS(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                       uint32_t dst0, uint32_t dst1,
                                       uint32_t src0, uint32_t src1) {
    auto* rs     = load_dreg(src0, "rs");
    // ABS with saturation: abs(INT_MIN) wraps, clamp to 0x7fffffff
    auto* abs_v  = emit_abs(rs);
    auto* is_min = builder_.CreateICmpEQ(abs_v, builder_.getInt32(0x80000000u), "is_min");
    auto* result = builder_.CreateSelect(is_min, builder_.getInt32(0x7fffffff), abs_v, "sat");
    store_dreg(dst0, result);
    auto* v_flag = builder_.CreateZExt(is_min, builder_.getInt32Ty(), "v");
    emit_flags_arith(result, v_flag, builder_.getInt32(0));
    return true;
}
bool LiftVisitor::emit_neg32(bool saturate,
                              uint32_t dst0, uint32_t src0, uint32_t src1) {
    // Rd = -Rs; saturate=false: wrap (0x80000000 stays 0x80000000, V=0)
    //           saturate=true:  clamp (0x80000000 -> 0x7FFFFFFF, V=1)
    auto* rs = load_dreg(src0, "rs");
    llvm::Value* result;
    if (saturate) {
        auto* is_min = builder_.CreateICmpEQ(rs, builder_.getInt32(0x80000000u), "is_min");
        auto* neg_rs = builder_.CreateNeg(rs, "neg_rs");
        result = builder_.CreateSelect(is_min, builder_.getInt32(0x7FFFFFFFu), neg_rs, "result");
        store_dreg(dst0, result);
        auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(result, builder_.getInt32(0)),
                                       builder_.getInt32Ty(), "az");
        auto* an = builder_.CreateLShr(result, 31, "an");
        auto* vf = builder_.CreateZExt(is_min, builder_.getInt32Ty(), "v");
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_v(vf);
        store_cpu_u32(offsetof(CpuState, vs),
            builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs"), vf));
    } else {
        result = builder_.CreateNeg(rs, "neg_rs");
        store_dreg(dst0, result);
        auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(result, builder_.getInt32(0)),
                                       builder_.getInt32Ty(), "az");
        auto* an = builder_.CreateLShr(result, 31, "an");
        store_cpu_u32(offsetof(CpuState, az), az);
        store_cpu_u32(offsetof(CpuState, an), an);
        store_v(builder_.getInt32(0));
    }
    return true;
}
bool LiftVisitor::decode_dsp32alu_NEG_NS(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_neg32(false, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_NEG_S(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    return emit_neg32(true, dst0, src0, src1);
}
bool LiftVisitor::decode_dsp32alu_ACC_A0_CLR(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* zero = builder_.getInt32(0);
    store_cpu_u32(offsetof(CpuState, aw[0]), zero);
    store_cpu_u32(offsetof(CpuState, ax[0]), zero);
    return true;
}
void LiftVisitor::emit_acc_sat(int n) {
    auto* i32ty = builder_.getInt32Ty();
    auto* min_s32 = builder_.getInt64(-0x80000000LL);
    auto* max_s32 = builder_.getInt64(0x7FFFFFFFLL);
    auto* acc = emit_load_acc(n);
    auto* ov_lo = builder_.CreateICmpSLT(acc, min_s32);
    auto* ov_hi = builder_.CreateICmpSGT(acc, max_s32);
    auto* ov    = builder_.CreateOr(ov_lo, ov_hi, "ov");
    auto* clamped = emit_smin(emit_smax(acc, min_s32), max_s32);
    // sign-extend bit 31 into upper byte
    auto* sign31 = builder_.CreateAnd(clamped, builder_.getInt64(0x80000000LL));
    auto* sext   = builder_.CreateNeg(sign31);
    auto* final_acc = builder_.CreateOr(clamped, sext, "final_acc");
    emit_store_acc(n, final_acc);
    auto* av = builder_.CreateZExt(ov, i32ty, "av");
    size_t av_off  = n == 0 ? offsetof(CpuState, av0)  : offsetof(CpuState, av1);
    size_t avs_off = n == 0 ? offsetof(CpuState, av0s) : offsetof(CpuState, av1s);
    store_cpu_u32(av_off, av);
    store_cpu_u32(avs_off, builder_.CreateOr(load_cpu_u32(avs_off, "avs"), av));
    store_cpu_u32(offsetof(CpuState, az),
        builder_.CreateZExt(builder_.CreateICmpEQ(final_acc, builder_.getInt64(0)), i32ty, "az"));
    store_cpu_u32(offsetof(CpuState, an),
        builder_.CreateZExt(builder_.CreateICmpSLT(final_acc, builder_.getInt64(0)), i32ty, "an"));
}
bool LiftVisitor::decode_dsp32alu_ACC_A0_SAT(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_sat(0);
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_A1_CLR(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* zero = builder_.getInt32(0);
    store_cpu_u32(offsetof(CpuState, aw[1]), zero);
    store_cpu_u32(offsetof(CpuState, ax[1]), zero);
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_A1_SAT(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_sat(1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_A1A0_CLR(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto* zero = builder_.getInt32(0);
    store_cpu_u32(offsetof(CpuState, aw[0]), zero);
    store_cpu_u32(offsetof(CpuState, ax[0]), zero);
    store_cpu_u32(offsetof(CpuState, aw[1]), zero);
    store_cpu_u32(offsetof(CpuState, ax[1]), zero);
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_A1A0_SAT(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_sat(0);
    emit_acc_sat(1);
    // Override AZ/AN per reference: az = either is zero, an = either is negative
    auto* i32ty = builder_.getInt32Ty();
    auto* final0 = emit_load_acc(0);
    auto* final1 = emit_load_acc(1);
    auto* either_zero = builder_.CreateOr(
        builder_.CreateICmpEQ(final0, builder_.getInt64(0)),
        builder_.CreateICmpEQ(final1, builder_.getInt64(0)));
    auto* either_neg = builder_.CreateOr(
        builder_.CreateICmpSLT(final0, builder_.getInt64(0)),
        builder_.CreateICmpSLT(final1, builder_.getInt64(0)));
    store_cpu_u32(offsetof(CpuState, az),
        builder_.CreateZExt(either_zero, i32ty, "az"));
    store_cpu_u32(offsetof(CpuState, an),
        builder_.CreateZExt(either_neg, i32ty, "an"));
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_COPY_A0_A1(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A0 = A1: copy AX[1]/AW[1] into AX[0]/AW[0]
    store_cpu_u32(offsetof(CpuState, aw[0]), load_cpu_u32(offsetof(CpuState, aw[1]), "aw1"));
    store_cpu_u32(offsetof(CpuState, ax[0]), load_cpu_u32(offsetof(CpuState, ax[1]), "ax1"));
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_COPY_A1_A0(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A1 = A0: copy AX[0]/AW[0] into AX[1]/AW[1]
    store_cpu_u32(offsetof(CpuState, aw[1]), load_cpu_u32(offsetof(CpuState, aw[0]), "aw0"));
    store_cpu_u32(offsetof(CpuState, ax[1]), load_cpu_u32(offsetof(CpuState, ax[0]), "ax0"));
    return true;
}
bool LiftVisitor::decode_dsp32alu_A0X_READ(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // Rd.L = (int8_t)A0.X — sign-extend 8-bit ax[0] to 16 bits, write to Rd low half
    auto* ax8   = builder_.CreateTrunc(
        load_cpu_u32(offsetof(CpuState, ax[0]), "a0x"), builder_.getInt8Ty(), "ax8");
    auto* val16 = builder_.CreateSExt(ax8, builder_.getInt16Ty(), "ax_sext16");
    auto* val32 = builder_.CreateSExt(val16, builder_.getInt32Ty(), "ax_sext32");
    store_dreg_lo(dst0, val32);
    return true;
}
bool LiftVisitor::decode_dsp32alu_A1X_READ(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // Rd.L = (int8_t)A1.X — sign-extend 8-bit ax[1] to 16 bits, write to Rd low half
    auto* ax8   = builder_.CreateTrunc(
        load_cpu_u32(offsetof(CpuState, ax[1]), "a1x"), builder_.getInt8Ty(), "ax8");
    auto* val16 = builder_.CreateSExt(ax8, builder_.getInt16Ty(), "ax_sext16");
    auto* val32 = builder_.CreateSExt(val16, builder_.getInt32Ty(), "ax_sext32");
    store_dreg_lo(dst0, val32);
    return true;
}
bool LiftVisitor::decode_dsp32alu_A0_PLUS_A1(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A0 += A1, then Dreg(dst0) = saturate_s32(A0); az/an from dreg; ac0=carry; v/vs=sat_s32
    auto* i32ty = builder_.getInt32Ty();
    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    // Carry: (uint40)(~acc1) < (uint40)(acc0) for addition
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* not_acc1 = builder_.CreateAnd(builder_.CreateXor(acc1, mask40), mask40, "not_acc1_40");
    auto* carry = builder_.CreateZExt(
        builder_.CreateICmpULT(not_acc1, builder_.CreateAnd(acc0, mask40)), i32ty, "carry");
    auto* sum = builder_.CreateAdd(acc0, acc1, "sum");
    // Saturate s40
    auto* min40 = builder_.getInt64(-0x8000000000LL);
    auto* max40 = builder_.getInt64(0x7FFFFFFFFFLL);
    // A0 += A1 via saturate_s40_astat uses > boundary (not >=)
    auto* ov40_lo = builder_.CreateICmpSLT(sum, min40);
    auto* ov40_hi = builder_.CreateICmpSGT(sum, max40);
    auto* v40     = builder_.CreateOr(ov40_lo, ov40_hi, "v40");
    auto* result = emit_smin(emit_smax(sum, min40), max40);
    // Store A0
    store_cpu_u32(offsetof(CpuState, ax[0]),
        builder_.CreateTrunc(builder_.CreateLShr(result, 32), i32ty));
    store_cpu_u32(offsetof(CpuState, aw[0]),
        builder_.CreateTrunc(result, i32ty));
    // Saturate result to s32 for Dreg output
    auto* min_s32 = builder_.getInt64(-0x80000000LL);
    auto* max_s32 = builder_.getInt64(0x7FFFFFFFLL);
    auto* ov_lo32 = builder_.CreateICmpSLT(result, min_s32);
    auto* ov_hi32 = builder_.CreateICmpSGT(result, max_s32);
    auto* sat_s32 = builder_.CreateOr(ov_lo32, ov_hi32, "sat_s32");
    auto* dreg_i64 = emit_smin(emit_smax(result, min_s32), max_s32);
    auto* dreg = builder_.CreateTrunc(dreg_i64, i32ty, "dreg");
    store_dreg(dst0, dreg);
    store_cpu_u32(offsetof(CpuState, az),
        builder_.CreateZExt(builder_.CreateICmpEQ(dreg, builder_.getInt32(0)), i32ty, "az"));
    store_cpu_u32(offsetof(CpuState, an),
        builder_.CreateLShr(dreg, builder_.getInt32(31)));
    store_ac0(carry);
    // av0 = v40 && (acc1 != 0) (refs/bfin-sim.c:4737: SET_ASTATREG(av0, v && acc1))
    auto* acc1_nz = builder_.CreateICmpNE(acc1, builder_.getInt64(0), "acc1_nz");
    auto* av0_v   = builder_.CreateZExt(builder_.CreateAnd(v40, acc1_nz), i32ty, "av0");
    store_cpu_u32(offsetof(CpuState, av0), av0_v);
    store_cpu_u32(offsetof(CpuState, av0s),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, av0s), "avs"), av0_v));
    auto* v = builder_.CreateZExt(sat_s32, i32ty, "v");
    store_v(v);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs"), v));
    return true;
}
void LiftVisitor::emit_a0_plus_a1_hl(bool hi, uint32_t dst0) {
    auto* i32ty = builder_.getInt32Ty();
    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* not_acc1 = builder_.CreateAnd(builder_.CreateXor(acc1, mask40), mask40, "not_acc1_40");
    auto* carry = builder_.CreateZExt(
        builder_.CreateICmpULT(not_acc1, builder_.CreateAnd(acc0, mask40)), i32ty, "carry");
    auto* sum = builder_.CreateAdd(acc0, acc1, "sum");
    auto* min40 = builder_.getInt64(-0x8000000000LL);
    auto* max40 = builder_.getInt64(0x7FFFFFFFFFLL);
    auto* ov40_lo = builder_.CreateICmpSLT(sum, min40);
    auto* ov40_hi = builder_.CreateICmpSGT(sum, max40);
    auto* v40     = builder_.CreateOr(ov40_lo, ov40_hi, "v40");
    auto* result = emit_smin(emit_smax(sum, min40), max40);
    emit_store_acc(0, result);
    // rnd16: (result + 0x8000) >> 16, saturate to s16
    auto* rnd = builder_.CreateAShr(
        builder_.CreateAdd(result, builder_.getInt64(0x8000LL)), 16, "rnd16");
    auto* min16 = builder_.getInt64(-0x8000LL);
    auto* max16 = builder_.getInt64(0x7FFFLL);
    auto* sat_lo = builder_.CreateICmpSLT(rnd, min16);
    auto* sat_hi = builder_.CreateICmpSGT(rnd, max16);
    auto* sat = builder_.CreateOr(sat_lo, sat_hi, "sat16");
    auto* dreg32 = builder_.CreateTrunc(emit_smin(emit_smax(rnd, min16), max16), i32ty, "dreg32");
    if (hi) store_dreg_hi(dst0, dreg32);
    else    store_dreg_lo(dst0, dreg32);
    store_ac0(carry);
    // av0 = v40 && (acc1 != 0) (refs/bfin-sim.c:4737)
    auto* acc1_nz = builder_.CreateICmpNE(acc1, builder_.getInt64(0), "acc1_nz");
    auto* av0_v   = builder_.CreateZExt(builder_.CreateAnd(v40, acc1_nz), i32ty, "av0");
    store_cpu_u32(offsetof(CpuState, av0), av0_v);
    store_cpu_u32(offsetof(CpuState, av0s),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, av0s), "avs"), av0_v));
    auto* v = builder_.CreateZExt(sat, i32ty, "v");
    store_v(v);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs"), v));
}
bool LiftVisitor::decode_dsp32alu_A0_PLUS_A1_HL_lo(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_a0_plus_a1_hl(false, dst0);
    return true;
}
bool LiftVisitor::decode_dsp32alu_A0_PLUS_A1_HL_hi(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_a0_plus_a1_hl(true, dst0);
    return true;
}
void LiftVisitor::emit_acc_inc_dec(bool is_sub, bool w32) {
    auto* i32ty = builder_.getInt32Ty();
    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);

    llvm::Value* carry;
    llvm::Value* combined;
    if (!is_sub) {
        auto* not_acc1 = builder_.CreateAnd(builder_.CreateXor(acc1, mask40), mask40, "not_acc1_40");
        carry = builder_.CreateZExt(
            builder_.CreateICmpULT(not_acc1, builder_.CreateAnd(acc0, mask40)), i32ty, "carry");
        combined = builder_.CreateAdd(acc0, acc1, "sum");
    } else {
        carry = builder_.CreateZExt(
            builder_.CreateICmpULT(
                builder_.CreateAnd(acc1, mask40),
                builder_.CreateAnd(acc0, mask40)), i32ty, "carry");
        combined = builder_.CreateSub(acc0, acc1, "diff");
    }

    auto* min40 = builder_.getInt64(-0x8000000000LL);
    auto* max40 = builder_.getInt64(0x7FFFFFFFFFLL);
    auto* ov_lo = builder_.CreateICmpSLT(combined, min40);
    // A0 -= A1 uses >= boundary (refs/bfin-sim.c:4260); A0 += A1 uses > (saturate_s40_astat:1419)
    auto* ov_hi = is_sub ? builder_.CreateICmpSGE(combined, max40)
                         : builder_.CreateICmpSGT(combined, max40);
    auto* ov = builder_.CreateOr(ov_lo, ov_hi, "ov");
    auto* sat_result = emit_smin(emit_smax(combined, min40), max40);

    llvm::Value* result = sat_result;
    if (w32) {
        auto* sign39 = builder_.CreateAnd(sat_result, builder_.getInt64(0x8000000000LL));
        auto* is_neg = builder_.CreateICmpNE(sign39, builder_.getInt64(0));
        result = builder_.CreateSelect(is_neg,
            builder_.CreateAnd(sat_result, builder_.getInt64(0x80FFFFFFFFLL)),
            builder_.CreateAnd(sat_result, builder_.getInt64(0xFFFFFFFFLL)));
    }

    emit_store_acc(0, result);
    llvm::Value* av;
    if (!is_sub) {
        // A0 += A1: av0 = v && acc1 (refs/bfin-sim.c:4737)
        auto* acc1_nz = builder_.CreateICmpNE(acc1, builder_.getInt64(0), "acc1_nz");
        av = builder_.CreateZExt(builder_.CreateAnd(ov, acc1_nz), i32ty, "av");
    } else {
        av = builder_.CreateZExt(ov, i32ty, "av");
    }
    store_cpu_u32(offsetof(CpuState, av0), av);
    store_cpu_u32(offsetof(CpuState, av0s),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, av0s), "avs"), av));
    store_cpu_u32(offsetof(CpuState, az),
        builder_.CreateZExt(builder_.CreateICmpEQ(result, builder_.getInt64(0)), i32ty, "az"));
    store_cpu_u32(offsetof(CpuState, an),
        builder_.CreateZExt(builder_.CreateICmpNE(
            builder_.CreateAnd(result, builder_.getInt64(0x8000000000LL)),
            builder_.getInt64(0)), i32ty, "an"));
    store_ac0(carry);
}
bool LiftVisitor::decode_dsp32alu_A0_INC_A1(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_inc_dec(false, false);
    return true;
}
bool LiftVisitor::decode_dsp32alu_A0_INC_A1_W32(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_inc_dec(false, true);
    return true;
}
bool LiftVisitor::decode_dsp32alu_A0_DEC_A1(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_inc_dec(true, false);
    return true;
}
bool LiftVisitor::decode_dsp32alu_A0_DEC_A1_W32(
        uint32_t M, uint32_t HL, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_inc_dec(true, true);
    return true;
}
bool LiftVisitor::decode_dsp32alu_SIGN_MULT(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // Rd.H = Rd.L = SIGN(Rs.H)*Rt.H + SIGN(Rs.L)*Rt.L
    // Rs = src0 (sign source), Rt = src1 (value to conditionally negate)
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");

    // Extract signed 16-bit halves via arithmetic shifts
    auto* rs_hi = builder_.CreateAShr(rs, builder_.getInt32(16), "rs_hi");
    auto* rs_lo = builder_.CreateAShr(builder_.CreateShl(rs, builder_.getInt32(16)),
                                      builder_.getInt32(16), "rs_lo");
    auto* rt_hi = builder_.CreateAShr(rt, builder_.getInt32(16), "rt_hi");
    auto* rt_lo = builder_.CreateAShr(builder_.CreateShl(rt, builder_.getInt32(16)),
                                      builder_.getInt32(16), "rt_lo");

    // SIGN(Rs.H)*Rt.H: negate Rt.H if Rs.H < 0
    auto* rs_hi_neg = builder_.CreateICmpSLT(rs_hi, builder_.getInt32(0), "rs_hi_neg");
    auto* neg_rt_hi = builder_.CreateAdd(builder_.CreateNot(rt_hi), builder_.getInt32(1), "neg_rt_hi");
    auto* val_hi = builder_.CreateSelect(rs_hi_neg, neg_rt_hi, rt_hi, "val_hi");

    // SIGN(Rs.L)*Rt.L: negate Rt.L if Rs.L < 0
    auto* rs_lo_neg = builder_.CreateICmpSLT(rs_lo, builder_.getInt32(0), "rs_lo_neg");
    auto* neg_rt_lo = builder_.CreateAdd(builder_.CreateNot(rt_lo), builder_.getInt32(1), "neg_rt_lo");
    auto* val_lo = builder_.CreateSelect(rs_lo_neg, neg_rt_lo, rt_lo, "val_lo");

    // Sum and mask to 16 bits
    auto* sum = builder_.CreateAnd(
        builder_.CreateAdd(val_hi, val_lo, "sum"), builder_.getInt32(0xFFFF), "sum16");

    // Pack into both Rd.H and Rd.L
    store_dreg(dst0, builder_.CreateOr(builder_.CreateShl(sum, builder_.getInt32(16)), sum, "packed"));
    return true;
}
bool LiftVisitor::decode_dsp32alu_ACC_ACCUM_SUM(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1,
                                                 uint32_t src0, uint32_t src1) {
    // Rd0 = A0.L + A0.H (signed), Rd1 = A1.L + A1.H  (no ASTAT update)
    auto* i32 = builder_.getInt32Ty();

    // A0: aw[0] holds the 32-bit word; sign-extend each 16-bit half, add
    auto* aw0   = load_cpu_u32(offsetof(CpuState, aw[0]), "aw0");
    auto* a0_l  = builder_.CreateSExt(
        builder_.CreateTrunc(aw0, builder_.getInt16Ty()), i32, "a0_l");
    auto* a0_h  = builder_.CreateSExt(
        builder_.CreateTrunc(builder_.CreateLShr(aw0, 16), builder_.getInt16Ty()), i32, "a0_h");
    auto* val0  = builder_.CreateAdd(a0_l, a0_h, "val0");
    store_dreg(dst0, val0);

    // A1: aw[1]
    auto* aw1   = load_cpu_u32(offsetof(CpuState, aw[1]), "aw1");
    auto* a1_l  = builder_.CreateSExt(
        builder_.CreateTrunc(aw1, builder_.getInt16Ty()), i32, "a1_l");
    auto* a1_h  = builder_.CreateSExt(
        builder_.CreateTrunc(builder_.CreateLShr(aw1, 16), builder_.getInt16Ty()), i32, "a1_h");
    auto* val1  = builder_.CreateAdd(a1_l, a1_h, "val1");
    store_dreg(dst1, val1);

    return true;
}
bool LiftVisitor::decode_dsp32alu_RND_HL_lo(uint32_t M, uint32_t s, uint32_t x,
                                             uint32_t dst0, uint32_t dst1,
                                             uint32_t src0, uint32_t src1) {
    // Rd.l = Rs (RND): round 32-bit Dreg to 16-bit, write to Rd.l
    auto* i32   = builder_.getInt32Ty();
    auto* rs    = load_dreg(src0, "rs");
    auto* sb_b  = builder_.CreateLShr(rs, 31, "sb_b");          // sign before round
    auto* rnd   = builder_.CreateAdd(rs, builder_.getInt32(0x8000u), "rnd");
    auto* sb_a  = builder_.CreateLShr(rnd, 31, "sb_a");         // sign after round
    // Overflow: sign changed AND upper 16 bits of rnd are non-zero
    auto* upper = builder_.CreateLShr(rnd, 16, "upper");
    auto* ov    = builder_.CreateAnd(
        builder_.CreateICmpNE(upper, builder_.getInt32(0)),
        builder_.CreateICmpNE(sb_b, sb_a), "ov");
    // Saturation: was positive -> 0x7FFF, was negative -> 0x8000
    auto* was_neg = builder_.CreateICmpNE(sb_b, builder_.getInt32(0));
    auto* sat   = builder_.CreateSelect(was_neg,
                    builder_.getInt32(0x8000u), builder_.getInt32(0x7FFFu), "sat");
    auto* no_ov = builder_.CreateAShr(rnd, 16, "no_ov");
    auto* res   = builder_.CreateSelect(ov, sat, no_ov, "res");
    auto* r16   = builder_.CreateAnd(res, builder_.getInt32(0xFFFF), "r16");
    store_dreg_lo(dst0, r16);
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(r16, builder_.getInt32(0)), i32, "az");
    auto* an = builder_.CreateZExt(builder_.CreateICmpNE(
        builder_.CreateAnd(r16, builder_.getInt32(0x8000u)), builder_.getInt32(0)), i32, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    auto* vf = builder_.CreateZExt(ov, i32, "v");
    store_v(vf);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs"), vf));
    return true;
}
bool LiftVisitor::decode_dsp32alu_RND_HL_hi(uint32_t M, uint32_t s, uint32_t x,
                                             uint32_t dst0, uint32_t dst1,
                                             uint32_t src0, uint32_t src1) {
    // Rd.h = Rs (RND): round 32-bit Dreg to 16-bit, write to Rd.h
    auto* i32   = builder_.getInt32Ty();
    auto* rs    = load_dreg(src0, "rs");
    auto* sb_b  = builder_.CreateLShr(rs, 31, "sb_b");
    auto* rnd   = builder_.CreateAdd(rs, builder_.getInt32(0x8000u), "rnd");
    auto* sb_a  = builder_.CreateLShr(rnd, 31, "sb_a");
    auto* upper = builder_.CreateLShr(rnd, 16, "upper");
    auto* ov    = builder_.CreateAnd(
        builder_.CreateICmpNE(upper, builder_.getInt32(0)),
        builder_.CreateICmpNE(sb_b, sb_a), "ov");
    auto* was_neg = builder_.CreateICmpNE(sb_b, builder_.getInt32(0));
    auto* sat   = builder_.CreateSelect(was_neg,
                    builder_.getInt32(0x8000u), builder_.getInt32(0x7FFFu), "sat");
    auto* no_ov = builder_.CreateAShr(rnd, 16, "no_ov");
    auto* res   = builder_.CreateSelect(ov, sat, no_ov, "res");
    auto* r16   = builder_.CreateAnd(res, builder_.getInt32(0xFFFF), "r16");
    store_dreg_hi(dst0, r16);
    auto* az = builder_.CreateZExt(builder_.CreateICmpEQ(r16, builder_.getInt32(0)), i32, "az");
    auto* an = builder_.CreateZExt(builder_.CreateICmpNE(
        builder_.CreateAnd(r16, builder_.getInt32(0x8000u)), builder_.getInt32(0)), i32, "an");
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    auto* vf = builder_.CreateZExt(ov, i32, "v");
    store_v(vf);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs"), vf));
    return true;
}
bool LiftVisitor::decode_dsp32alu_SEARCH(
        uint32_t M, uint32_t HL, uint32_t aop, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // (Rd1, Rd0) = SEARCH Rs (GT/GE/LT/LE)
    // Compare signed 16-bit halves of Rs against lower 16 bits of A0/A1;
    // on match: update accumulator and write P0 into dest Dreg.
    auto* i16ty = builder_.getInt16Ty();
    auto* i32ty = builder_.getInt32Ty();

    auto* rs = load_dreg(src0, "rs");
    // Sign-extend 16-bit halves of Rs
    auto* src_l = builder_.CreateSExt(
        builder_.CreateTrunc(rs, i16ty), i32ty, "src_l");
    auto* src_h = builder_.CreateSExt(
        builder_.CreateTrunc(builder_.CreateLShr(rs, 16), i16ty), i32ty, "src_h");
    // Sign-extend lower 16 bits of each accumulator word
    auto* a0_lo = builder_.CreateSExt(
        builder_.CreateTrunc(load_cpu_u32(offsetof(CpuState, aw[0]), "aw0"), i16ty), i32ty, "a0_lo");
    auto* a1_lo = builder_.CreateSExt(
        builder_.CreateTrunc(load_cpu_u32(offsetof(CpuState, aw[1]), "aw1"), i16ty), i32ty, "a1_lo");

    llvm::Value* up_lo = nullptr;
    llvm::Value* up_hi = nullptr;
    switch (aop) {
    case 0: // GT
        up_lo = builder_.CreateICmpSGT(src_l, a0_lo, "up_lo");
        up_hi = builder_.CreateICmpSGT(src_h, a1_lo, "up_hi");
        break;
    case 1: // GE
        up_lo = builder_.CreateICmpSGE(src_l, a0_lo, "up_lo");
        up_hi = builder_.CreateICmpSGE(src_h, a1_lo, "up_hi");
        break;
    case 2: // LT
        up_lo = builder_.CreateICmpSLT(src_l, a0_lo, "up_lo");
        up_hi = builder_.CreateICmpSLT(src_h, a1_lo, "up_hi");
        break;
    case 3: // LE
    default:
        up_lo = builder_.CreateICmpSLE(src_l, a0_lo, "up_lo");
        up_hi = builder_.CreateICmpSLE(src_h, a1_lo, "up_hi");
        break;
    }

    auto* p0 = load_preg(0, "p0");

    // Update A0: new value is src_l when up_lo, else a0_lo (sign-extended to 32-bit)
    // SET_AREG32: ax = -(aw >> 31), i.e. 0xff if aw is negative, 0x00 if non-negative
    auto* new_aw0 = builder_.CreateSelect(up_lo, src_l, a0_lo, "new_aw0");
    store_cpu_u32(offsetof(CpuState, aw[0]), new_aw0);
    store_cpu_u32(offsetof(CpuState, ax[0]),
        builder_.CreateNeg(builder_.CreateLShr(new_aw0, builder_.getInt32(31), "sign0"), "new_ax0"));
    // Update Dreg(dst0) with P0 if up_lo, else unchanged
    store_dreg(dst0, builder_.CreateSelect(up_lo, p0, load_dreg(dst0, "old_dst0"), "dst0_val"));

    // Update A1 and Dreg(dst1) similarly
    auto* new_aw1 = builder_.CreateSelect(up_hi, src_h, a1_lo, "new_aw1");
    store_cpu_u32(offsetof(CpuState, aw[1]), new_aw1);
    store_cpu_u32(offsetof(CpuState, ax[1]),
        builder_.CreateNeg(builder_.CreateLShr(new_aw1, builder_.getInt32(31), "sign1"), "new_ax1"));
    store_dreg(dst1, builder_.CreateSelect(up_hi, p0, load_dreg(dst1, "old_dst1"), "dst1_val"));

    return true;
}
void LiftVisitor::emit_acc_neg(int src, int dst) {
    auto* i32ty = builder_.getInt32Ty();
    auto* acc = emit_load_acc(src);

    // AC = 1 if source was zero (negating 0 -> 0 with carry)
    auto* is_zero = builder_.CreateICmpEQ(acc, builder_.getInt64(0), "is_zero");
    auto* ac_val = builder_.CreateZExt(is_zero, i32ty, "ac");

    // Negate
    auto* neg_acc = builder_.CreateNeg(acc, "neg_acc");

    // Overflow: -(2^39) negated = 2^39, clamp to 2^39-1
    auto* min_val = builder_.getInt64(1LL << 39);
    auto* max_val = builder_.getInt64((1LL << 39) - 1);
    auto* av_i1 = builder_.CreateICmpEQ(neg_acc, min_val, "av_i1");
    auto* result = builder_.CreateSelect(av_i1, max_val, neg_acc, "result");

    emit_store_acc(dst, result);
    auto* ax_new = builder_.CreateTrunc(builder_.CreateLShr(result, 32), i32ty, "ax_new");

    // ASTAT flags
    auto* az = builder_.CreateZExt(
        builder_.CreateICmpEQ(result, builder_.getInt64(0)), i32ty, "az");
    auto* an = builder_.CreateZExt(
        builder_.CreateICmpNE(builder_.CreateAnd(ax_new, builder_.getInt32(0x80)),
                              builder_.getInt32(0)), i32ty, "an");
    auto* av = builder_.CreateZExt(av_i1, i32ty, "av");

    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    if (dst == 0) {
        store_ac0(ac_val);
        store_cpu_u32(offsetof(CpuState, av0), av);
        store_cpu_u32(offsetof(CpuState, av0s),
            builder_.CreateOr(load_cpu_u32(offsetof(CpuState, av0s), "av0s_cur"), av));
    } else {
        store_cpu_u32(offsetof(CpuState, ac1), ac_val);
        store_cpu_u32(offsetof(CpuState, av1), av);
        store_cpu_u32(offsetof(CpuState, av1s),
            builder_.CreateOr(load_cpu_u32(offsetof(CpuState, av1s), "av1s_cur"), av));
    }
}
bool LiftVisitor::decode_dsp32alu_A_NEG_HL0_AOP0(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_neg(0, 0); // A0 = -A0
    return true;
}
bool LiftVisitor::decode_dsp32alu_A_NEG_HL0_AOP1(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_neg(1, 0); // A0 = -A1
    return true;
}
bool LiftVisitor::decode_dsp32alu_A_NEG_HL1_AOP0(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_neg(0, 1); // A1 = -A0
    return true;
}
bool LiftVisitor::decode_dsp32alu_A_NEG_HL1_AOP1(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_neg(1, 1); // A1 = -A1
    return true;
}
bool LiftVisitor::decode_dsp32alu_A_NEG_BOTH(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A1 = -A1, A0 = -A0 (no ASTAT flags updated per reference sim)
    auto* i32ty = builder_.getInt32Ty();
    auto* max_val = builder_.getInt64((1LL << 39) - 1);
    auto* min_val = builder_.getInt64(1LL << 39);
    for (int i = 0; i < 2; ++i) {
        size_t ax_off = i == 0 ? offsetof(CpuState, ax[0]) : offsetof(CpuState, ax[1]);
        size_t aw_off = i == 0 ? offsetof(CpuState, aw[0]) : offsetof(CpuState, aw[1]);
        auto* ax = load_cpu_u32(ax_off, "ax");
        auto* aw = load_cpu_u32(aw_off, "aw");
        auto* acc = build_acc_i64(ax, aw);
        auto* neg_acc = builder_.CreateNeg(acc, "neg_acc");
        // Saturate -(2^39) to 2^39-1
        auto* result = builder_.CreateSelect(
            builder_.CreateICmpEQ(neg_acc, min_val), max_val, neg_acc, "sat");
        store_cpu_u32(aw_off, builder_.CreateTrunc(result, i32ty, "aw_new"));
        store_cpu_u32(ax_off, builder_.CreateTrunc(
            builder_.CreateLShr(result, 32), i32ty, "ax_new"));
    }
    return true;
}
bool LiftVisitor::decode_dsp32alu_NEG_V(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // Rd = -Rs (V): negate each 16-bit half independently with saturation
    auto* i32ty = builder_.getInt32Ty();
    auto* rs = load_dreg(src0, "rs");

    // Sign-extend halves to i32 for negation
    auto* rs_h = builder_.CreateSExt(
        builder_.CreateTrunc(builder_.CreateLShr(rs, 16), builder_.getInt16Ty()),
        i32ty, "rs_h");
    auto* rs_l = builder_.CreateSExt(
        builder_.CreateTrunc(rs, builder_.getInt16Ty()), i32ty, "rs_l");

    // AC0/AC1: source half was zero (negating 0 -> 0, carry-out = 1)
    auto* ac1 = builder_.CreateZExt(
        builder_.CreateICmpEQ(rs_h, builder_.getInt32(0)), i32ty, "ac1");
    auto* ac0 = builder_.CreateZExt(
        builder_.CreateICmpEQ(rs_l, builder_.getInt32(0)), i32ty, "ac0");

    // Negate
    auto* neg_h = builder_.CreateNeg(rs_h, "neg_h");
    auto* neg_l = builder_.CreateNeg(rs_l, "neg_l");

    // Overflow: -(-32768) = 32768 which overflows i16; such input negated in i32 gives +0x8000
    auto* ov_h = builder_.CreateICmpEQ(neg_h, builder_.getInt32(0x8000), "ov_h");
    auto* ov_l = builder_.CreateICmpEQ(neg_l, builder_.getInt32(0x8000), "ov_l");
    auto* v_i1 = builder_.CreateOr(ov_h, ov_l);

    // Saturate: clamp 0x8000 -> 0x7FFF
    auto* sat = builder_.getInt32(0x7FFF);
    auto* res_h = builder_.CreateSelect(ov_h, sat,
        builder_.CreateAnd(neg_h, builder_.getInt32(0xFFFF)), "res_h");
    auto* res_l = builder_.CreateSelect(ov_l, sat,
        builder_.CreateAnd(neg_l, builder_.getInt32(0xFFFF)), "res_l");

    auto* result = builder_.CreateOr(builder_.CreateShl(res_h, 16), res_l);
    store_dreg(dst0, result);

    // ASTAT: AZ = either half is 0, AN = either half has bit 15 set
    auto* az = builder_.CreateZExt(
        builder_.CreateOr(
            builder_.CreateICmpEQ(res_h, builder_.getInt32(0)),
            builder_.CreateICmpEQ(res_l, builder_.getInt32(0))),
        i32ty, "az");
    auto* an = builder_.CreateZExt(
        builder_.CreateOr(
            builder_.CreateICmpNE(builder_.CreateAnd(res_h, builder_.getInt32(0x8000)), builder_.getInt32(0)),
            builder_.CreateICmpNE(builder_.CreateAnd(res_l, builder_.getInt32(0x8000)), builder_.getInt32(0))),
        i32ty, "an");
    auto* v_flag = builder_.CreateZExt(v_i1, i32ty, "v");
    auto* vs_new = builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag);

    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), an);
    store_ac0(ac0);
    store_cpu_u32(offsetof(CpuState, ac1), ac1);
    store_v(v_flag);
    store_cpu_u32(offsetof(CpuState, vs), vs_new);
    return true;
}
// dsp32alu A_ABS variants — accumulator absolute value
// Semantics (refs/bfin-sim.c:4379-4461): negate if negative, saturate -(2^39) to 0x7FFFFFFFFF
// Flags: av[HL], avs[HL] (sticky), az (result==0), an (always 0)
void LiftVisitor::emit_acc_abs_flags(int dst_acc, llvm::Value* av) {
    auto* acc = emit_load_acc(dst_acc);
    auto* az = builder_.CreateZExt(
        builder_.CreateICmpEQ(acc, builder_.getInt64(0)), builder_.getInt32Ty(), "az");
    size_t av_off  = dst_acc == 0 ? offsetof(CpuState, av0)  : offsetof(CpuState, av1);
    size_t avs_off = dst_acc == 0 ? offsetof(CpuState, av0s) : offsetof(CpuState, av1s);
    store_cpu_u32(av_off, av);
    store_cpu_u32(avs_off, builder_.CreateOr(load_cpu_u32(avs_off, "avs"), av));
    store_cpu_u32(offsetof(CpuState, az), az);
    store_cpu_u32(offsetof(CpuState, an), builder_.getInt32(0));
}

bool LiftVisitor::decode_dsp32alu_A_ABS_HL0_AOP0(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_abs_flags(0, emit_acc_abs(0, 0));
    return true;
}

bool LiftVisitor::decode_dsp32alu_A_ABS_HL0_AOP1(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_abs_flags(0, emit_acc_abs(1, 0));
    return true;
}

bool LiftVisitor::decode_dsp32alu_A_ABS_HL1_AOP0(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_abs_flags(1, emit_acc_abs(0, 1));
    return true;
}

bool LiftVisitor::decode_dsp32alu_A_ABS_HL1_AOP1(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_acc_abs_flags(1, emit_acc_abs(1, 1));
    return true;
}

bool LiftVisitor::decode_dsp32alu_A_ABS_BOTH(
        uint32_t M, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // A1 = ABS A1, A0 = ABS A0 (dual parallel)
    auto* av0 = emit_acc_abs(0, 0);
    auto* av1 = emit_acc_abs(1, 1);
    // Per-accumulator av/avs flags
    emit_acc_abs_flags(0, av0);
    emit_acc_abs_flags(1, av1);
    // Override AZ: either zero (OR of the two)
    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);
    auto* zero64 = builder_.getInt64(0);
    auto* az = builder_.CreateZExt(
        builder_.CreateOr(
            builder_.CreateICmpEQ(acc0, zero64, "az0"),
            builder_.CreateICmpEQ(acc1, zero64, "az1")),
        builder_.getInt32Ty(), "az");
    store_cpu_u32(offsetof(CpuState, az), az);
    return true;
}
void LiftVisitor::emit_a_sum_diff(bool a1_minus_a0, uint32_t s, uint32_t dst0, uint32_t dst1) {
    auto* acc0 = emit_load_acc(0);
    auto* acc1 = emit_load_acc(1);

    auto* sum  = builder_.CreateAdd(acc0, acc1, "sum");
    auto* diff = a1_minus_a0 ? builder_.CreateSub(acc1, acc0, "diff")
                             : builder_.CreateSub(acc0, acc1, "diff");

    auto* i32ty   = builder_.getInt32Ty();
    auto* min_s32 = builder_.getInt64(-0x80000000LL);
    auto* max_s32 = builder_.getInt64(0x7FFFFFFFLL);

    auto sat_s32 = [&](llvm::Value* v64, const char* tag)
            -> std::pair<llvm::Value*, llvm::Value*> {
        auto* ov_lo   = builder_.CreateICmpSLT(v64, min_s32);
        auto* ov_hi   = builder_.CreateICmpSGT(v64, max_s32);
        auto* ov      = builder_.CreateOr(ov_lo, ov_hi, tag);
        auto* clamped = emit_smin(emit_smax(v64, min_s32), max_s32);
        return { builder_.CreateTrunc(clamped, i32ty),
                 builder_.CreateZExt(ov, i32ty) };
    };

    auto [sum_sat,  sum_ov]  = sat_s32(sum,  "sum_ov");
    auto [diff_sat, diff_ov] = sat_s32(diff, "diff_ov");

    auto* val1 = s ? sum_sat  : builder_.CreateTrunc(sum,  i32ty);
    auto* val0 = s ? diff_sat : builder_.CreateTrunc(diff, i32ty);

    store_dreg(dst1, val1);
    store_dreg(dst0, val0);

    auto* v_flag = builder_.CreateOr(sum_ov, diff_ov, "v_flag");
    store_v(v_flag);
    store_cpu_u32(offsetof(CpuState, vs),
        builder_.CreateOr(load_cpu_u32(offsetof(CpuState, vs), "vs_cur"), v_flag));

    auto* an = builder_.CreateOr(
        builder_.CreateLShr(val0, builder_.getInt32(31)),
        builder_.CreateLShr(val1, builder_.getInt32(31)), "an");
    store_cpu_u32(offsetof(CpuState, an), an);

    auto* az = builder_.CreateZExt(builder_.CreateOr(
        builder_.CreateICmpEQ(val0, builder_.getInt32(0)),
        builder_.CreateICmpEQ(val1, builder_.getInt32(0))), i32ty, "az");
    store_cpu_u32(offsetof(CpuState, az), az);

    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* u0     = builder_.CreateAnd(acc0, mask40, "u0");
    auto* u1     = builder_.CreateAnd(acc1, mask40, "u1");
    auto* not_u0 = builder_.CreateXor(u0, mask40, "not_u0");
    store_cpu_u32(offsetof(CpuState, ac1),
        builder_.CreateZExt(builder_.CreateICmpULT(not_u0, u1), i32ty, "ac1"));
    // AC0 direction depends on which operand is subtracted
    if (a1_minus_a0)
        store_ac0(builder_.CreateZExt(builder_.CreateICmpULE(u0, u1), i32ty, "ac0"));
    else
        store_ac0(builder_.CreateZExt(builder_.CreateICmpULE(u1, u0), i32ty, "ac0"));
}
bool LiftVisitor::decode_dsp32alu_A1pA0_A1mA0(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    if (x != 0 || HL != 0) return false;
    emit_a_sum_diff(true, s, dst0, dst1);
    return true;
}

bool LiftVisitor::decode_dsp32alu_A0pA1_A0mA1(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    if (x != 0 || HL != 0) return false;
    emit_a_sum_diff(false, s, dst0, dst1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_SAA(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* i32ty = B.getInt32Ty();
    auto* i64ty = B.getInt64Ty();

    // Load accumulators as 64-bit unsigned: acc = zext(aw) | ((ax & 0xff) << 32)
    auto make_acc64 = [&](Value* aw, Value* ax) -> Value* {
        auto* aw64 = B.CreateZExt(aw, i64ty, "aw64");
        auto* ax64 = B.CreateAnd(B.CreateZExt(ax, i64ty), B.getInt64(0xFF), "ax64");
        return B.CreateOr(aw64, B.CreateShl(ax64, 32), "acc64");
    };
    auto* acc0 = make_acc64(
        load_cpu_u32(offsetof(CpuState, aw[0]), "aw0"),
        load_cpu_u32(offsetof(CpuState, ax[0]), "ax0"));
    auto* acc1 = make_acc64(
        load_cpu_u32(offsetof(CpuState, aw[1]), "aw1"),
        load_cpu_u32(offsetof(CpuState, ax[1]), "ax1"));

    // Load register pairs
    auto* s0L = load_dreg(src0,     "s0L");
    auto* s0H = load_dreg(src0 + 1, "s0H");
    auto* s1L = load_dreg(src1,     "s1L");
    auto* s1H = load_dreg(src1 + 1, "s1H");

    // algn(l, h, aln): aln=0 → l; else (l >> 8*aln) | (h << (32 - 8*aln))

    // Load I0 & I1 alignment nibbles (bits [1:0])
    auto* i0_raw = load_cpu_u32(offsetof(CpuState, iregs) + 0 * 4, "i0");
    auto* i1_raw = load_cpu_u32(offsetof(CpuState, iregs) + 1 * 4, "i1");
    auto* i0_aln = B.CreateAnd(i0_raw, B.getInt32(3), "i0_aln");
    auto* i1_aln = B.CreateAnd(i1_raw, B.getInt32(3), "i1_aln");

    Value* sv0;
    Value* sv1;
    if (s) {
        sv0 = emit_byte_align(s0H, s0L, i0_aln);
        sv1 = emit_byte_align(s1H, s1L, i1_aln);
    } else {
        sv0 = emit_byte_align(s0L, s0H, i0_aln);
        sv1 = emit_byte_align(s1L, s1H, i1_aln);
    }

    // Extract unsigned byte k (k=0 is LSB) as i32 value [0..255]
    auto extract_ub = [&](Value* v, unsigned k) -> Value* {
        auto* shifted = B.CreateLShr(v, B.getInt32(8 * k));
        return B.CreateAnd(shifted, B.getInt32(0xff), "ub");
    };

    // abs(a - b) for signed i32 values
    auto abs_diff = [&](Value* a, Value* b) -> Value* {
        return emit_abs(B.CreateSub(a, b, "d"));
    };

    // saturate_u16: clamp i32 to [0, 0xffff]
    auto sat_u16 = [&](Value* v) -> Value* {
        return emit_umin(v, B.getInt32(0xffff));
    };

    // Extract 16-bit half from 64-bit acc
    auto acc_half = [&](Value* acc64, int shift) -> Value* {
        auto* half = B.CreateTrunc(B.CreateLShr(acc64, B.getInt64(shift)), i32ty);
        return B.CreateAnd(half, B.getInt32(0xffff));
    };

    // Compute 4 abs-diffs
    auto* tmp0 = abs_diff(extract_ub(sv0, 0), extract_ub(sv1, 0));
    auto* tmp1 = abs_diff(extract_ub(sv0, 1), extract_ub(sv1, 1));
    auto* tmp2 = abs_diff(extract_ub(sv0, 2), extract_ub(sv1, 2));
    auto* tmp3 = abs_diff(extract_ub(sv0, 3), extract_ub(sv1, 3));

    // Saturate-accumulate into 16-bit halves
    auto* s0L_new = sat_u16(B.CreateAdd(tmp0, acc_half(acc0, 0)));
    auto* s0H_new = sat_u16(B.CreateAdd(tmp1, acc_half(acc0, 16)));
    auto* s1L_new = sat_u16(B.CreateAdd(tmp2, acc_half(acc1, 0)));
    auto* s1H_new = sat_u16(B.CreateAdd(tmp3, acc_half(acc1, 16)));

    // Store back: aw[n] = (sH_new << 16) | sL_new, ax[n] = 0
    auto* aw0_new = B.CreateOr(B.CreateShl(s0H_new, B.getInt32(16)), s0L_new);
    auto* aw1_new = B.CreateOr(B.CreateShl(s1H_new, B.getInt32(16)), s1L_new);
    auto* zero = B.getInt32(0);
    store_cpu_u32(offsetof(CpuState, aw[0]), aw0_new);
    store_cpu_u32(offsetof(CpuState, ax[0]), zero);
    store_cpu_u32(offsetof(CpuState, aw[1]), aw1_new);
    store_cpu_u32(offsetof(CpuState, ax[1]), zero);
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}
bool LiftVisitor::decode_dsp32alu_DISALGNEXCPT(
        uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
        uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    // Sets DIS_ALGN_EXPT: parallel dspLDST loads will mask address to &~3.
    dis_algn_expt_ = true;
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP1P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                            uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* sv0 = emit_byteop_load_align(src0, 0, s != 0, "s0");
    auto* sv1 = emit_byteop_load_align(src1, 1, s != 0, "s1");
    auto avg = [&](Value* a, Value* b) {
        return B.CreateLShr(B.CreateAdd(B.CreateAdd(a, b), B.getInt32(1)), B.getInt32(1), "avg");
    };
    auto* b0 = avg(emit_byteop_extract_ub(sv0, 0), emit_byteop_extract_ub(sv1, 0));
    auto* b1 = avg(emit_byteop_extract_ub(sv0, 1), emit_byteop_extract_ub(sv1, 1));
    auto* b2 = avg(emit_byteop_extract_ub(sv0, 2), emit_byteop_extract_ub(sv1, 2));
    auto* b3 = avg(emit_byteop_extract_ub(sv0, 3), emit_byteop_extract_ub(sv1, 3));
    auto* res = B.CreateOr(B.CreateShl(b3, B.getInt32(24)),
                   B.CreateOr(B.CreateShl(b2, B.getInt32(16)),
                   B.CreateOr(B.CreateShl(b1, B.getInt32(8)), b0)));
    store_dreg(dst0, res);
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}

bool LiftVisitor::decode_dsp32alu_BYTEOP1P_T(uint32_t M, uint32_t HL, uint32_t x,
                                              uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* sv0 = emit_byteop_load_align(src0, 0, false, "s0");
    auto* sv1 = emit_byteop_load_align(src1, 1, false, "s1");
    auto avg = [&](Value* a, Value* b) {
        return B.CreateLShr(B.CreateAdd(a, b), B.getInt32(1), "avg");
    };
    auto* b0 = avg(emit_byteop_extract_ub(sv0, 0), emit_byteop_extract_ub(sv1, 0));
    auto* b1 = avg(emit_byteop_extract_ub(sv0, 1), emit_byteop_extract_ub(sv1, 1));
    auto* b2 = avg(emit_byteop_extract_ub(sv0, 2), emit_byteop_extract_ub(sv1, 2));
    auto* b3 = avg(emit_byteop_extract_ub(sv0, 3), emit_byteop_extract_ub(sv1, 3));
    auto* res = B.CreateOr(B.CreateShl(b3, B.getInt32(24)),
                   B.CreateOr(B.CreateShl(b2, B.getInt32(16)),
                   B.CreateOr(B.CreateShl(b1, B.getInt32(8)), b0)));
    store_dreg(dst0, res);
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}

bool LiftVisitor::decode_dsp32alu_BYTEOP1P_T_R(uint32_t M, uint32_t HL, uint32_t x,
                                                uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* sv0 = emit_byteop_load_align(src0, 0, true, "s0");
    auto* sv1 = emit_byteop_load_align(src1, 1, true, "s1");
    auto avg = [&](Value* a, Value* b) {
        return B.CreateLShr(B.CreateAdd(a, b), B.getInt32(1), "avg");
    };
    auto* b0 = avg(emit_byteop_extract_ub(sv0, 0), emit_byteop_extract_ub(sv1, 0));
    auto* b1 = avg(emit_byteop_extract_ub(sv0, 1), emit_byteop_extract_ub(sv1, 1));
    auto* b2 = avg(emit_byteop_extract_ub(sv0, 2), emit_byteop_extract_ub(sv1, 2));
    auto* b3 = avg(emit_byteop_extract_ub(sv0, 3), emit_byteop_extract_ub(sv1, 3));
    auto* res = B.CreateOr(B.CreateShl(b3, B.getInt32(24)),
                   B.CreateOr(B.CreateShl(b2, B.getInt32(16)),
                   B.CreateOr(B.CreateShl(b1, B.getInt32(8)), b0)));
    store_dreg(dst0, res);
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}

bool LiftVisitor::decode_dsp32alu_BYTEOP16P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                             uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* sv0 = emit_byteop_load_align(src0, 0, s != 0, "s0");
    auto* sv1 = emit_byteop_load_align(src1, 1, s != 0, "s1");
    auto add4 = [&](int k) {
        return B.CreateAdd(emit_byteop_extract_ub(sv0, k), emit_byteop_extract_ub(sv1, k), "sum");
    };
    store_dreg(dst0, emit_byteop_pack2(add4(0), add4(1), false));
    store_dreg(dst1, emit_byteop_pack2(add4(2), add4(3), false));
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}

bool LiftVisitor::decode_dsp32alu_BYTEOP16M(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                             uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* sv0 = emit_byteop_load_align(src0, 0, s != 0, "s0");
    auto* sv1 = emit_byteop_load_align(src1, 1, s != 0, "s1");
    auto sub4 = [&](int k) {
        auto* diff = B.CreateSub(emit_byteop_extract_ub(sv0, k), emit_byteop_extract_ub(sv1, k), "diff");
        return B.CreateAnd(diff, B.getInt32(0xffff), "masked");
    };
    store_dreg(dst0, emit_byteop_pack2(sub4(0), sub4(1), false));
    store_dreg(dst1, emit_byteop_pack2(sub4(2), sub4(3), false));
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}

void LiftVisitor::emit_byteop2p(bool reversed, bool hi_slot, bool rounding,
                                uint32_t dst0, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    // Both sources use the same I0 alignment — load it once.
    auto* i_raw = load_cpu_u32(offsetof(CpuState, iregs) + 0 * 4, "i");
    auto* i_aln = B.CreateAnd(i_raw, B.getInt32(3), "aln");
    auto* sv0 = emit_byteop_load_align_v(src0, i_aln, reversed, "s0");
    auto* sv1 = emit_byteop_load_align_v(src1, i_aln, reversed, "s1");
    auto quad_avg = [&](int k0, int k1) -> Value* {
        auto* s = B.CreateAdd(
            B.CreateAdd(emit_byteop_extract_ub(sv0, k0), emit_byteop_extract_ub(sv1, k0)),
            B.CreateAdd(emit_byteop_extract_ub(sv0, k1), emit_byteop_extract_ub(sv1, k1)));
        if (rounding) s = B.CreateAdd(s, B.getInt32(2));
        return B.CreateAnd(B.CreateLShr(s, B.getInt32(2)), B.getInt32(0xff));
    };
    store_dreg(dst0, emit_byteop_pack2(quad_avg(0, 1), quad_avg(2, 3), hi_slot));
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_RNDL(uint32_t M, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(false, false, true, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_RNDL_R(uint32_t M, uint32_t x,
                                                   uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(true, false, true, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_RNDH(uint32_t M, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(false, true, true, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_RNDH_R(uint32_t M, uint32_t x,
                                                   uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(true, true, true, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_TL(uint32_t M, uint32_t x,
                                               uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(false, false, false, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_TL_R(uint32_t M, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(true, false, false, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_TH(uint32_t M, uint32_t x,
                                               uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(false, true, false, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP2P_TH_R(uint32_t M, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop2p(true, true, false, dst0, src0, src1);
    return true;
}

void LiftVisitor::emit_byteop3p(bool reversed, bool hi_slot,
                                uint32_t dst0, uint32_t src0, uint32_t src1) {
    auto& B = builder_;
    auto* i32ty = B.getInt32Ty();
    auto* sv0 = emit_byteop_load_align(src0, 0, reversed, "s0");
    auto* sv1 = emit_byteop_load_align(src1, 1, reversed, "s1");
    auto* s0_lo = B.CreateSExt(B.CreateTrunc(sv0, B.getInt16Ty()), i32ty, "s0_lo");
    auto* s0_hi = B.CreateSExt(B.CreateTrunc(B.CreateLShr(sv0, B.getInt32(16)), B.getInt16Ty()), i32ty, "s0_hi");
    // HI uses even byte lanes (0,2), LO uses odd byte lanes (1,3)
    int bk0 = hi_slot ? 0 : 1;
    int bk1 = hi_slot ? 2 : 3;
    auto* b0 = B.CreateZExt(emit_byteop_extract_ub(sv1, bk0), i32ty);
    auto* b1 = B.CreateZExt(emit_byteop_extract_ub(sv1, bk1), i32ty);
    store_dreg(dst0, emit_byteop_pack2(
        emit_clamp_u8(B.CreateAdd(s0_lo, b0)),
        emit_clamp_u8(B.CreateAdd(s0_hi, b1)), hi_slot));
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
}
bool LiftVisitor::decode_dsp32alu_BYTEOP3P_LO(uint32_t M, uint32_t x,
                                               uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop3p(false, false, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP3P_LO_R(uint32_t M, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop3p(true, false, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP3P_HI(uint32_t M, uint32_t x,
                                               uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop3p(false, true, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEOP3P_HI_R(uint32_t M, uint32_t x,
                                                 uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    emit_byteop3p(true, true, dst0, src0, src1);
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEPACK(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                            uint32_t dst0, uint32_t dst1,
                                            uint32_t src0, uint32_t src1) {
    // Rd = BYTEPACK(Rs, Rt):
    //   dst[7:0]   = rs[7:0]   (low byte of Rs low half)
    //   dst[15:8]  = rs[23:16] (low byte of Rs high half)
    //   dst[23:16] = rt[7:0]   (low byte of Rt low half)
    //   dst[31:24] = rt[23:16] (low byte of Rt high half)
    auto& B = builder_;
    auto* rs = load_dreg(src0, "rs");
    auto* rt = load_dreg(src1, "rt");
    auto* b0 = B.CreateAnd(rs, B.getInt32(0xFF));           // rs[7:0]
    auto* b1 = B.CreateAnd(B.CreateLShr(rs, 16), B.getInt32(0xFF)); // rs[23:16]
    auto* b2 = B.CreateAnd(rt, B.getInt32(0xFF));           // rt[7:0]
    auto* b3 = B.CreateAnd(B.CreateLShr(rt, 16), B.getInt32(0xFF)); // rt[23:16]
    auto* res = B.CreateOr(B.CreateOr(b0, B.CreateShl(b1, 8)),
                   B.CreateOr(B.CreateShl(b2, 16), B.CreateShl(b3, 24)));
    store_dreg(dst0, res);
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}
bool LiftVisitor::decode_dsp32alu_BYTEUNPACK(uint32_t M, uint32_t HL, uint32_t s, uint32_t x,
                                              uint32_t dst0, uint32_t dst1,
                                              uint32_t src0, uint32_t src1) {
    // (Rd1, Rd0) = BYTEUNPACK Rs+1:Rs  [or reversed with s==1]
    // Uses I0[1:0] as byte rotation offset.
    // Extracts 4 consecutive bytes from 64-bit combined source, places each as
    // low byte of a 16-bit half-word slot: dst0 = byteb<<16|bytea, dst1 = byted<<16|bytec
    auto& B = builder_;
    auto* i64ty = B.getInt64Ty();
    auto* i32ty = B.getInt32Ty();

    // Load source register pair; s==1 reverses hi/lo
    auto* src_lo = B.CreateZExt(load_dreg(s ? src0 + 1 : src0,     "src_lo"), i64ty);
    auto* src_hi = B.CreateZExt(load_dreg(s ? src0     : src0 + 1, "src_hi"), i64ty);
    // Combined 64-bit source: hi in bits[63:32], lo in bits[31:0]
    auto* comb = B.CreateOr(B.CreateShl(src_hi, B.getInt64(32)), src_lo, "comb");

    // Read I0 byte alignment offset
    auto* i0_raw = load_cpu_u32(offsetof(CpuState, iregs) + 0 * 4, "i0");
    auto* order  = B.CreateAnd(i0_raw, B.getInt32(3), "order");
    auto* order64 = B.CreateZExt(order, i64ty, "order64");
    // Byte bit offset = 8 * order
    auto* bit_off = B.CreateMul(order64, B.getInt64(8), "bit_off");

    // Extract 4 consecutive bytes from comb starting at bit_off
    auto extract_byte = [&](uint64_t extra_off) -> llvm::Value* {
        auto* shifted = B.CreateLShr(comb,
            B.CreateAdd(bit_off, B.getInt64(extra_off)), "sh");
        return B.CreateTrunc(B.CreateAnd(shifted, B.getInt64(0xFF)), i32ty, "byt");
    };
    auto* bytea = extract_byte(0);
    auto* byteb = extract_byte(8);
    auto* bytec = extract_byte(16);
    auto* byted = extract_byte(24);

    // dst0 = bytea | (byteb << 16), dst1 = bytec | (byted << 16)
    store_dreg(dst0, B.CreateOr(bytea, B.CreateShl(byteb, 16)));
    store_dreg(dst1, B.CreateOr(bytec, B.CreateShl(byted, 16)));
    dis_algn_expt_ = true;  // implicit DISALGNEXCPT
    return true;
}

// dsp32shift register-shift implementations

// ASHIFT16 (sop=0, sopcde=0): Rd.h/l = ASHIFT Rs.h/l BY Rt.l
bool LiftVisitor::decode_dsp32shift_ASHIFT16(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs    = load_dreg(src1, "rs");
    auto* in16  = emit_extract_half16(rs, HLs);
    // Sign-extend in16 to i32 for arithmetic right shift
    auto* in_se  = builder_.CreateAShr(
        builder_.CreateShl(in16, builder_.getInt32(16)), builder_.getInt32(16), "in_se");
    auto* in_sign = builder_.CreateLShr(in16, builder_.getInt32(15), "in_sign");
    auto* is_pos  = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    // Right: arithmetic shift of sign-extended value, clamp shift to 16
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(16)),
        builder_.getInt32(16), neg_shft, "rcnt");
    auto* right_result = builder_.CreateAnd(
        builder_.CreateAShr(in_se, rcnt), builder_.getInt32(0xFFFF), "right_result");
    // Left: logical left shift in i64 to preserve overflow bits
    auto* in16_64      = builder_.CreateZExt(in16, builder_.getInt64Ty(), "in16_64");
    auto* shft64       = builder_.CreateZExt(shft, builder_.getInt64Ty(), "shft64");
    auto* left_shifted = builder_.CreateShl(in16_64, shft64, "left_shifted");
    auto* left_result  = builder_.CreateAnd(
        builder_.CreateTrunc(left_shifted, builder_.getInt32Ty()),
        builder_.getInt32(0xFFFF), "left_result");
    // Overflow detection
    auto* overflow = emit_shift16_overflow(left_shifted, left_result, shft, in_sign);
    // Select based on shift direction
    auto* result16 = builder_.CreateSelect(is_pos, left_result, right_result, "result16");
    emit_merge_half16(dst, result16, HLs);
    // V/VS on left overflow only; AZ, AN always
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateSelect(is_pos, overflow, builder_.getFalse()), i32ty, "v_flag");
    emit_v_vs_update(v_flag);
    emit_flags_az_an_16(result16);
    return true;
}

// ASHIFT16S (sop=1, sopcde=0): Rd.h/l = ASHIFT Rs.h/l BY Rt.l (S) — with saturation
bool LiftVisitor::decode_dsp32shift_ASHIFT16S(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs    = load_dreg(src1, "rs");
    auto* in16  = emit_extract_half16(rs, HLs);
    auto* in_se   = builder_.CreateAShr(
        builder_.CreateShl(in16, builder_.getInt32(16)), builder_.getInt32(16), "in_se");
    auto* in_sign = builder_.CreateLShr(in16, builder_.getInt32(15), "in_sign");
    auto* is_pos  = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    // Right: arithmetic right, clamped to 16
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(16)),
        builder_.getInt32(16), neg_shft, "rcnt");
    auto* right_result = builder_.CreateAnd(
        builder_.CreateAShr(in_se, rcnt), builder_.getInt32(0xFFFF), "right_result");
    // Left: logical left shift in i64 to preserve overflow bits
    auto* in16_64      = builder_.CreateZExt(in16, builder_.getInt64Ty(), "in16_64");
    auto* shft64       = builder_.CreateZExt(shft, builder_.getInt64Ty(), "shft64");
    auto* left_shifted = builder_.CreateShl(in16_64, shft64, "left_shifted");
    auto* left_raw     = builder_.CreateAnd(
        builder_.CreateTrunc(left_shifted, builder_.getInt32Ty()),
        builder_.getInt32(0xFFFF), "left_raw");
    // Overflow detection
    auto* overflow = emit_shift16_overflow(left_shifted, left_raw, shft, in_sign);
    // Saturate based on original sign
    auto* saturated    = builder_.CreateSelect(
        builder_.CreateICmpNE(in_sign, builder_.getInt32(0)),
        builder_.getInt32(0x8000), builder_.getInt32(0x7FFF), "saturated");
    auto* left_result  = builder_.CreateSelect(overflow, saturated, left_raw, "left_result");
    auto* result16 = builder_.CreateSelect(is_pos, left_result, right_result, "result16");
    emit_merge_half16(dst, result16, HLs);
    // V/VS on left overflow; AZ, AN
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateSelect(is_pos, overflow, builder_.getFalse()), i32ty, "v_flag");
    emit_v_vs_update(v_flag);
    emit_flags_az_an_16(result16);
    return true;
}

// LSHIFT16 (sop=2, sopcde=0): Rd.h/l = LSHIFT Rs.h/l BY Rt.l
bool LiftVisitor::decode_dsp32shift_LSHIFT16(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* shft  = emit_extract_shift6(src0);
    auto* rs    = load_dreg(src1, "rs");
    auto* in16  = emit_extract_half16(rs, HLs);
    auto* is_pos  = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    // Right: logical (zero-fill), clamp to 16
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(16)),
        builder_.getInt32(16), neg_shft, "rcnt");
    auto* right_result = builder_.CreateLShr(in16, rcnt, "right_result");
    // Left: logical left shift
    auto* left_result  = builder_.CreateAnd(
        builder_.CreateShl(in16, shft), builder_.getInt32(0xFFFF), "left_result");
    auto* result16 = builder_.CreateSelect(is_pos, left_result, right_result, "result16");
    emit_merge_half16(dst, result16, HLs);
    // V = 0; AZ, AN
    store_v(builder_.getInt32(0));
    emit_flags_az_an_16(result16);
    return true;
}

// VASHIFT (sop=0, sopcde=1): Rd = ASHIFT Rs BY Rt.l (V)
bool LiftVisitor::decode_dsp32shift_VASHIFT(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs     = load_dreg(src1, "rs");
    auto* val0   = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1   = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    // Sign-extend each half
    auto* se0    = builder_.CreateAShr(builder_.CreateShl(val0, 16), 16, "se0");
    auto* se1    = builder_.CreateAShr(builder_.CreateShl(val1, 16), 16, "se1");
    auto* is_pos = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(16)),
        builder_.getInt32(16), neg_shft, "rcnt");
    // Arithmetic right
    auto* r0 = builder_.CreateAnd(builder_.CreateAShr(se0, rcnt), builder_.getInt32(0xFFFF), "r0");
    auto* r1 = builder_.CreateAnd(builder_.CreateAShr(se1, rcnt), builder_.getInt32(0xFFFF), "r1");
    // Logical left in i64 to preserve overflow bits
    auto* shft64     = builder_.CreateZExt(shft, builder_.getInt64Ty(), "shft64");
    auto* l0_shifted = builder_.CreateShl(builder_.CreateZExt(val0, builder_.getInt64Ty()), shft64, "l0_shifted");
    auto* l1_shifted = builder_.CreateShl(builder_.CreateZExt(val1, builder_.getInt64Ty()), shft64, "l1_shifted");
    auto* l0     = builder_.CreateAnd(builder_.CreateTrunc(l0_shifted, builder_.getInt32Ty()), builder_.getInt32(0xFFFF), "l0");
    auto* l1     = builder_.CreateAnd(builder_.CreateTrunc(l1_shifted, builder_.getInt32Ty()), builder_.getInt32(0xFFFF), "l1");
    // Overflow per half
    auto* is0 = builder_.CreateLShr(val0, 15, "is0");
    auto* is1 = builder_.CreateLShr(val1, 15, "is1");
    auto* ov0 = emit_shift16_overflow(l0_shifted, l0, shft, is0);
    auto* ov1 = emit_shift16_overflow(l1_shifted, l1, shft, is1);
    // Select direction
    auto* out0   = builder_.CreateSelect(is_pos, l0, r0, "out0");
    auto* out1   = builder_.CreateSelect(is_pos, l1, r1, "out1");
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    // V/VS: overflow in either half on left shift
    auto* any_ov = builder_.CreateOr(ov0, ov1, "any_ov");
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateSelect(is_pos, any_ov, builder_.getFalse()), i32ty, "v_flag");
    emit_v_vs_update(v_flag);
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}

// VASHIFTS (sop=1, sopcde=1): Rd = ASHIFT Rs BY Rt.l (V, S) — with saturation
bool LiftVisitor::decode_dsp32shift_VASHIFTS(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs     = load_dreg(src1, "rs");
    auto* val0   = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1   = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    auto* se0    = builder_.CreateAShr(builder_.CreateShl(val0, 16), 16, "se0");
    auto* se1    = builder_.CreateAShr(builder_.CreateShl(val1, 16), 16, "se1");
    auto* sign0  = builder_.CreateLShr(val0, 15, "sign0");
    auto* sign1  = builder_.CreateLShr(val1, 15, "sign1");
    auto* is_pos = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(16)),
        builder_.getInt32(16), neg_shft, "rcnt");
    // Arithmetic right
    auto* r0 = builder_.CreateAnd(builder_.CreateAShr(se0, rcnt), builder_.getInt32(0xFFFF), "r0");
    auto* r1 = builder_.CreateAnd(builder_.CreateAShr(se1, rcnt), builder_.getInt32(0xFFFF), "r1");
    // Logical left in i64 to preserve overflow bits
    auto* shft64     = builder_.CreateZExt(shft, builder_.getInt64Ty(), "shft64");
    auto* l0_shifted = builder_.CreateShl(builder_.CreateZExt(val0, builder_.getInt64Ty()), shft64, "l0_shifted");
    auto* l1_shifted = builder_.CreateShl(builder_.CreateZExt(val1, builder_.getInt64Ty()), shft64, "l1_shifted");
    auto* l0_raw = builder_.CreateAnd(builder_.CreateTrunc(l0_shifted, builder_.getInt32Ty()), builder_.getInt32(0xFFFF), "l0_raw");
    auto* l1_raw = builder_.CreateAnd(builder_.CreateTrunc(l1_shifted, builder_.getInt32Ty()), builder_.getInt32(0xFFFF), "l1_raw");
    // Overflow per half
    auto* ov0 = emit_shift16_overflow(l0_shifted, l0_raw, shft, sign0);
    auto* ov1 = emit_shift16_overflow(l1_shifted, l1_raw, shft, sign1);
    // Saturate based on original sign
    auto* sat0   = builder_.CreateSelect(builder_.CreateICmpNE(sign0, builder_.getInt32(0)),
                                         builder_.getInt32(0x8000), builder_.getInt32(0x7FFF), "sat0");
    auto* sat1   = builder_.CreateSelect(builder_.CreateICmpNE(sign1, builder_.getInt32(0)),
                                         builder_.getInt32(0x8000), builder_.getInt32(0x7FFF), "sat1");
    auto* l0     = builder_.CreateSelect(ov0, sat0, l0_raw, "l0");
    auto* l1     = builder_.CreateSelect(ov1, sat1, l1_raw, "l1");
    auto* out0   = builder_.CreateSelect(is_pos, l0, r0, "out0");
    auto* out1   = builder_.CreateSelect(is_pos, l1, r1, "out1");
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    auto* any_ov = builder_.CreateOr(ov0, ov1, "any_ov");
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateSelect(is_pos, any_ov, builder_.getFalse()), i32ty, "v_flag");
    emit_v_vs_update(v_flag);
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}

// VLSHIFT (sop=2, sopcde=1): Rd = LSHIFT Rs BY Rt.l (V)
bool LiftVisitor::decode_dsp32shift_VLSHIFT(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* shft  = emit_extract_shift6(src0);
    auto* rs     = load_dreg(src1, "rs");
    auto* val0   = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1   = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    auto* is_pos = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(16)),
        builder_.getInt32(16), neg_shft, "rcnt");
    // Logical right
    auto* r0 = builder_.CreateLShr(val0, rcnt, "r0");
    auto* r1 = builder_.CreateLShr(val1, rcnt, "r1");
    // Logical left
    auto* l0 = builder_.CreateAnd(builder_.CreateShl(val0, shft), builder_.getInt32(0xFFFF), "l0");
    auto* l1 = builder_.CreateAnd(builder_.CreateShl(val1, shft), builder_.getInt32(0xFFFF), "l1");
    auto* out0   = builder_.CreateSelect(is_pos, l0, r0, "out0");
    auto* out1   = builder_.CreateSelect(is_pos, l1, r1, "out1");
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    store_v(builder_.getInt32(0));
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}

// ASHIFT32 (sop=0, sopcde=2): Rd = ASHIFT Rs BY Rt.l
bool LiftVisitor::decode_dsp32shift_ASHIFT32(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* i64ty = builder_.getInt64Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs      = load_dreg(src1, "rs");
    auto* in_sign = builder_.CreateLShr(rs, builder_.getInt32(31), "in_sign");
    auto* is_pos  = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    // Clamp to 31 (not 32): AShr(i32,32) is UB; clamping to 31 gives same sign-fill result
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(31)),
        builder_.getInt32(31), neg_shft, "rcnt");
    auto* right_result = builder_.CreateAShr(rs, rcnt, "right_result");
    // Left shift in i64 to avoid UB for large shifts.
    // Clamp shift to [0,31] before extending: SHL by a negative or huge amount is UB/poison
    // in LLVM IR even when guarded by a select, since poison propagates through arithmetic.
    auto* shft_clamp = builder_.CreateSelect(is_pos, shft, builder_.getInt32(0), "shft_clamp");
    auto* rs64 = builder_.CreateZExt(rs, i64ty, "rs64");
    auto* shft64 = builder_.CreateZExt(shft_clamp, i64ty, "shft64");
    auto* left64 = builder_.CreateShl(rs64, shft64, "left64");
    auto* left_shifted = builder_.CreateTrunc(left64, i32ty, "left_shifted");
    // Overflow detection: two conditions from refs/bfin-sim.c (sopcde==2, sop==0):
    // 1) lshift() v_i logic: high_part != 0 AND NOT (high_part==all_ones AND result_sign==1)
    //    handles cases where significant bits are shifted out above the 32-bit window
    auto* high_part = builder_.CreateTrunc(
        builder_.CreateLShr(left64, builder_.getInt64(32)), i32ty, "high_part");
    auto* shft64_ov = builder_.CreateZExt(shft_clamp, i64ty, "shft64_ov");
    auto* all_ones  = builder_.CreateTrunc(
        builder_.CreateSub(
            builder_.CreateShl(builder_.getInt64(1), shft64_ov),
            builder_.getInt64(1)),
        i32ty, "all_ones");
    auto* result_sign = builder_.CreateAnd(
        builder_.CreateLShr(left_shifted, builder_.getInt32(31)),
        builder_.getInt32(1), "result_sign");
    auto* hb_zero = builder_.CreateICmpEQ(high_part, builder_.getInt32(0), "hb_zero");
    auto* hb_ones = builder_.CreateICmpEQ(high_part, all_ones, "hb_ones");
    auto* sign_neg = builder_.CreateICmpNE(result_sign, builder_.getInt32(0), "sign_neg");
    auto* no_v_i = builder_.CreateOr(hb_zero,
        builder_.CreateAnd(hb_ones, sign_neg), "no_v_i");
    auto* v_i_overflow = builder_.CreateNot(no_v_i, "v_i_overflow");
    // 2) Call-site sign-flip check (refs line ~5422): ((v>>31) != (val>>31))
    //    catches shifts like 0x40000000<<1 where no bits exit window but MSB flips
    auto* out_sign  = builder_.CreateAnd(
        result_sign, builder_.getInt32(1), "out_sign");
    auto* sign_flip = builder_.CreateICmpNE(in_sign, out_sign, "sign_flip");
    auto* overflow  = builder_.CreateOr(v_i_overflow, sign_flip, "overflow");
    auto* result = builder_.CreateSelect(is_pos, left_shifted, right_result, "result");
    store_dreg(dst, result);
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateSelect(is_pos, overflow, builder_.getFalse()), i32ty, "v_flag");
    emit_v_vs_update(v_flag);
    emit_flags_az_an(result);
    return true;
}

// ASHIFT32S (sop=1, sopcde=2): Rd = ASHIFT Rs BY Rt.l (S) — with saturation
bool LiftVisitor::decode_dsp32shift_ASHIFT32S(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* i64ty = builder_.getInt64Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs      = load_dreg(src1, "rs");
    auto* in_sign = builder_.CreateLShr(rs, builder_.getInt32(31), "in_sign");
    auto* is_pos  = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    // Clamp to 31: AShr(i32,32) is UB
    auto* rcnt   = builder_.CreateSelect(
        builder_.CreateICmpSGT(neg_shft, builder_.getInt32(31)),
        builder_.getInt32(31), neg_shft, "rcnt");
    auto* right_result = builder_.CreateAShr(rs, rcnt, "right_result");
    // Clamp shift to [0,31] before extending: SHL by a negative or huge amount is UB/poison
    // in LLVM IR even when guarded by a select, since poison propagates through arithmetic.
    auto* shft_clamp = builder_.CreateSelect(is_pos, shft, builder_.getInt32(0), "shft_clamp");
    auto* rs64 = builder_.CreateSExt(rs, i64ty, "rs64");
    auto* shft64 = builder_.CreateZExt(shft_clamp, i64ty, "shft64");
    auto* left64 = builder_.CreateShl(rs64, shft64, "left64");
    auto* left_shifted = builder_.CreateTrunc(left64, i32ty, "left_shifted");
    auto* left_sext64 = builder_.CreateSExt(left_shifted, i64ty, "left_sext64");
    auto* overflow     = builder_.CreateICmpNE(left64, left_sext64, "overflow");
    auto* saturated    = builder_.CreateSelect(
        builder_.CreateICmpNE(in_sign, builder_.getInt32(0)),
        builder_.getInt32(0x80000000), builder_.getInt32(0x7FFFFFFF), "saturated");
    auto* left_result  = builder_.CreateSelect(overflow, saturated, left_shifted, "left_result");
    auto* result = builder_.CreateSelect(is_pos, left_result, right_result, "result");
    store_dreg(dst, result);
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateSelect(is_pos, overflow, builder_.getFalse()), i32ty, "v_flag");
    emit_v_vs_update(v_flag);
    emit_flags_az_an(result);
    return true;
}

// LSHIFT32 (sop=2, sopcde=2): Rd = LSHIFT Rs BY Rt.l
bool LiftVisitor::decode_dsp32shift_LSHIFT32(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* i32ty = builder_.getInt32Ty();
    auto* i64ty = builder_.getInt64Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs      = load_dreg(src1, "rs");
    auto* is_pos  = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* neg_shft = builder_.CreateNeg(shft, "neg_shft");
    // Use i64 for both directions to avoid UB when shift >= 32 (result becomes 0)
    auto* rs64     = builder_.CreateZExt(rs, i64ty, "rs64");
    auto* shft64   = builder_.CreateZExt(shft, i64ty, "shft64");       // left: non-neg i64
    auto* neg64    = builder_.CreateZExt(neg_shft, i64ty, "neg64");    // right: non-neg i64
    auto* right64  = builder_.CreateLShr(rs64, neg64, "right64");
    auto* left64   = builder_.CreateShl(rs64, shft64, "left64");
    auto* right_result = builder_.CreateTrunc(right64, i32ty, "right_result");
    auto* left_result  = builder_.CreateTrunc(left64, i32ty, "left_result");
    auto* result = builder_.CreateSelect(is_pos, left_result, right_result, "result");
    store_dreg(dst, result);
    store_v(builder_.getInt32(0));
    emit_flags_az_an(result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_ROT32(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = ROT Rs BY Rt.L: 33-bit rotate through CC; shift is signed 6-bit from Rt.L
    auto* i32ty = builder_.getInt32Ty();
    auto* i64ty = builder_.getInt64Ty();
    auto* shft  = emit_extract_shift6(src0);
    auto* rs     = load_dreg(src1, "rs");
    auto* cc_old = load_cpu_u32(offsetof(CpuState, cc), "cc_old");
    auto* rs64   = builder_.CreateZExt(rs, i64ty, "rs64");
    auto* cc64   = builder_.CreateZExt(cc_old, i64ty, "cc64");
    auto* shft64 = builder_.CreateSExt(shft, i64ty, "shft64");
    // Normalize: negative shift → left by adding 33
    auto* is_neg   = builder_.CreateICmpSLT(shft64, builder_.getInt64(0), "is_neg");
    auto* shft_pos = builder_.CreateAdd(shft64, builder_.getInt64(33), "shft_pos");
    auto* shft_n   = builder_.CreateSelect(is_neg, shft_pos, shft64, "shft_norm");
    // left_part = (shft_n == 32) ? 0 : rs64 << shft_n
    auto* is_32    = builder_.CreateICmpEQ(shft_n, builder_.getInt64(32), "is_32");
    auto* left_raw  = builder_.CreateShl(rs64, shft_n, "left_raw");
    auto* left_part = builder_.CreateSelect(is_32, builder_.getInt64(0), left_raw, "left_part");
    // right_part = (shft_n == 1) ? 0 : rs64 >> (33 - shft_n)
    auto* is_1       = builder_.CreateICmpEQ(shft_n, builder_.getInt64(1), "is_1");
    auto* shft_r     = builder_.CreateSub(builder_.getInt64(33), shft_n, "shft_r");
    auto* right_raw  = builder_.CreateLShr(rs64, shft_r, "right_raw");
    auto* right_part = builder_.CreateSelect(is_1, builder_.getInt64(0), right_raw, "right_part");
    // cc_in = cc64 << (shft_n - 1); guard against shft_n == 0
    auto* cc_in_raw = builder_.CreateShl(
        cc64, builder_.CreateSub(shft_n, builder_.getInt64(1)), "cc_in_raw");
    auto* is_zero_n = builder_.CreateICmpEQ(shft_n, builder_.getInt64(0), "is_zero_n");
    auto* cc_in     = builder_.CreateSelect(is_zero_n, builder_.getInt64(0), cc_in_raw, "cc_in");
    // Combine and mask to 32 bits; preserve original value when shift == 0
    auto* new_rs_raw = builder_.CreateAnd(
        builder_.CreateOr(builder_.CreateOr(left_part, right_part), cc_in),
        builder_.getInt64(0xFFFFFFFFULL), "new_rs_raw");
    auto* new_rs = builder_.CreateSelect(is_zero_n, rs64, new_rs_raw, "new_rs");
    store_dreg(dst, builder_.CreateTrunc(new_rs, i32ty, "rd_new"));
    // new_cc = (rs >> (32 - shft_n)) & 1; only update CC if shift != 0
    auto* new_cc64 = builder_.CreateAnd(
        builder_.CreateLShr(rs64, builder_.CreateSub(builder_.getInt64(32), shft_n)),
        builder_.getInt64(1), "new_cc64");
    auto* new_cc = builder_.CreateTrunc(new_cc64, i32ty, "new_cc");
    auto* is_zero_shift = builder_.CreateICmpEQ(shft, builder_.getInt32(0), "is_zero");
    store_cc(builder_.CreateSelect(is_zero_shift, cc_old, new_cc, "cc_out"));
    return true;
}
bool LiftVisitor::emit_acc_ashift(int n, uint32_t src0) {
    // An = ASHIFT An BY Rt.l — arithmetic shift 40-bit accumulator by signed 6-bit shift
    auto* i64ty = builder_.getInt64Ty();
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* shft = emit_extract_shift6(src0);
    auto* acc = emit_load_acc(n);
    auto* shft64 = builder_.CreateSExt(shft, i64ty, "shft64");
    auto* is_pos = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* left_result = builder_.CreateAnd(builder_.CreateShl(acc, shft64), mask40, "left_result");
    auto* neg_shft = builder_.CreateNeg(shft64, "neg_shft");
    auto* right_result = builder_.CreateAShr(acc, neg_shft, "right_result");
    auto* result = builder_.CreateAnd(
        builder_.CreateSelect(is_pos, left_result, right_result, "result"), mask40, "result_masked");
    emit_store_acc(n, result);
    emit_flags_az_an_acc(result);
    size_t av_off = n == 0 ? offsetof(CpuState, av0) : offsetof(CpuState, av1);
    store_cpu_u32(av_off, builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32shift_ACC_ASHIFT_A0(
        uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    return emit_acc_ashift(0, src0);
}
bool LiftVisitor::decode_dsp32shift_ACC_ASHIFT_A1(
        uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    return emit_acc_ashift(1, src0);
}
bool LiftVisitor::emit_acc_lshift(int n, uint32_t src0) {
    // An = LSHIFT An BY Rt.l — logical (zero-fill) 40-bit shift
    auto* i64ty  = builder_.getInt64Ty();
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* shft   = emit_extract_shift6(src0);
    auto* acc    = emit_load_acc_unsigned(n);
    auto* shft64      = builder_.CreateSExt(shft, i64ty, "shft64");
    auto* is_pos      = builder_.CreateICmpSGT(shft, builder_.getInt32(0), "is_pos");
    auto* left_result = builder_.CreateAnd(builder_.CreateShl(acc, shft64), mask40, "left_result");
    auto* neg_shft    = builder_.CreateNeg(shft64, "neg_shft");
    auto* right_result = builder_.CreateLShr(acc, neg_shft, "right_result");
    auto* result = builder_.CreateAnd(
        builder_.CreateSelect(is_pos, left_result, right_result, "result"), mask40, "result_masked");
    emit_store_acc(n, result);
    emit_flags_az_an_acc(result);
    size_t av_off = n == 0 ? offsetof(CpuState, av0) : offsetof(CpuState, av1);
    store_cpu_u32(av_off, builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32shift_ACC_LSHIFT_A0(
        uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    return emit_acc_lshift(0, src0);
}
bool LiftVisitor::decode_dsp32shift_ACC_LSHIFT_A1(
        uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    return emit_acc_lshift(1, src0);
}
bool LiftVisitor::emit_acc_rot(int n, uint32_t src0) {
    // An = ROT An BY Rt.l — 41-bit rotate through CC
    auto* i64ty  = builder_.getInt64Ty();
    auto* i32ty  = builder_.getInt32Ty();
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);

    auto* shft   = emit_extract_shift6(src0);
    auto* acc    = emit_load_acc_unsigned(n);
    auto* cc_old = load_cpu_u32(offsetof(CpuState, cc), "cc_old");
    auto* cc64   = builder_.CreateZExt(cc_old, i64ty, "cc64");

    // Normalize: shft_n = (shft < 0) ? shft + 41 : shft  (as i64)
    auto* shft64   = builder_.CreateSExt(shft, i64ty, "shft64");
    auto* is_neg   = builder_.CreateICmpSLT(shft64, builder_.getInt64(0), "is_neg");
    auto* shft_pos = builder_.CreateAdd(shft64, builder_.getInt64(41), "shft_pos");
    auto* shft_n   = builder_.CreateSelect(is_neg, shft_pos, shft64, "shft_norm");

    // left_part = (shft_n == 40) ? 0 : acc << shft_n
    auto* is_40     = builder_.CreateICmpEQ(shft_n, builder_.getInt64(40), "is_40");
    auto* left_raw  = builder_.CreateShl(acc, shft_n, "left_raw");
    auto* left_part = builder_.CreateSelect(is_40, builder_.getInt64(0), left_raw, "left_part");

    // right_part = (shft_n == 1) ? 0 : acc >> (41 - shft_n)
    auto* is_1       = builder_.CreateICmpEQ(shft_n, builder_.getInt64(1), "is_1");
    auto* shft_r     = builder_.CreateSub(builder_.getInt64(41), shft_n, "shft_r");
    auto* right_raw  = builder_.CreateLShr(acc, shft_r, "right_raw");
    auto* right_part = builder_.CreateSelect(is_1, builder_.getInt64(0), right_raw, "right_part");

    // cc_in = cc64 << (shft_n - 1); guarded to 0 when shft_n == 0
    auto* cc_in_raw = builder_.CreateShl(
        cc64, builder_.CreateSub(shft_n, builder_.getInt64(1)), "cc_in_raw");
    auto* is_zero_n = builder_.CreateICmpEQ(shft_n, builder_.getInt64(0), "is_zero_n");
    auto* cc_in     = builder_.CreateSelect(is_zero_n, builder_.getInt64(0), cc_in_raw, "cc_in");

    auto* new_acc_raw = builder_.CreateAnd(
        builder_.CreateOr(builder_.CreateOr(left_part, right_part), cc_in), mask40, "new_acc_raw");

    // When shft_n == 0, acc is unchanged
    auto* new_acc = builder_.CreateSelect(is_zero_n, acc, new_acc_raw, "new_acc");

    // new_cc = (acc >> (40 - shft_n)) & 1
    auto* new_cc64 = builder_.CreateAnd(
        builder_.CreateLShr(acc, builder_.CreateSub(builder_.getInt64(40), shft_n)),
        builder_.getInt64(1), "new_cc64");
    auto* new_cc = builder_.CreateTrunc(new_cc64, i32ty, "new_cc");

    emit_store_acc(n, new_acc);

    // Update CC only if shift != 0
    auto* is_zero_shift = builder_.CreateICmpEQ(shft, builder_.getInt32(0), "is_zero");
    store_cc(builder_.CreateSelect(is_zero_shift, cc_old, new_cc, "cc_out"));
    return true;
}
bool LiftVisitor::decode_dsp32shift_ACC_ROT_A0(
        uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    return emit_acc_rot(0, src0);
}
bool LiftVisitor::decode_dsp32shift_ACC_ROT_A1(
        uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    return emit_acc_rot(1, src0);
}
bool LiftVisitor::decode_dsp32shift_ROT32_dreg(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = ROT Rs BY Rt.L (alternate encoding sopcde=3) — same as ROT32
    return decode_dsp32shift_ROT32(M, HLs, dst, src0, src1);
}
bool LiftVisitor::decode_dsp32shift_PACK_LL(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = PACK(Rs.l, Rt.l): src1=Rs (low→high), src0=Rt (low→low)
    auto* rs  = load_dreg(src1, "rs");
    auto* rt  = load_dreg(src0, "rt");
    auto* rs_l = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "rs_l");
    auto* rt_l = builder_.CreateAnd(rt, builder_.getInt32(0xFFFF), "rt_l");
    store_dreg(dst, builder_.CreateOr(builder_.CreateShl(rs_l, 16), rt_l));
    return true;
}
bool LiftVisitor::decode_dsp32shift_PACK_LH(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = PACK(Rs.l, Rt.h): src1=Rs (low→high), src0=Rt (high→low)
    auto* rs  = load_dreg(src1, "rs");
    auto* rt  = load_dreg(src0, "rt");
    auto* rs_l = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "rs_l");
    auto* rt_h = builder_.CreateLShr(rt, 16, "rt_h");
    store_dreg(dst, builder_.CreateOr(builder_.CreateShl(rs_l, 16),
                                      builder_.CreateAnd(rt_h, builder_.getInt32(0xFFFF))));
    return true;
}
bool LiftVisitor::decode_dsp32shift_PACK_HL(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = PACK(Rs.h, Rt.l): src1=Rs (high half), src0=Rt (low half)
    auto* rs  = load_dreg(src1, "rs");
    auto* rt  = load_dreg(src0, "rt");
    auto* rs_h = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF0000u), "rs_h");
    auto* rt_l = builder_.CreateAnd(rt, builder_.getInt32(0xFFFF), "rt_l");
    store_dreg(dst, builder_.CreateOr(rs_h, rt_l));
    return true;
}
bool LiftVisitor::decode_dsp32shift_PACK_HH(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = PACK(Rs.h, Rt.h): src1=Rs (high→high), src0=Rt (high→low)
    auto* rs  = load_dreg(src1, "rs");
    auto* rt  = load_dreg(src0, "rt");
    auto* rs_h = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF0000u), "rs_h");
    auto* rt_h = builder_.CreateLShr(rt, 16, "rt_h");
    store_dreg(dst, builder_.CreateOr(rs_h, builder_.CreateAnd(rt_h, builder_.getInt32(0xFFFF))));
    return true;
}
bool LiftVisitor::decode_dsp32shift_SIGNBITS_32(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.l = SIGNBITS Rs: count redundant sign bits of 32-bit Rs (result range [0,30])
    // Algorithm: norm = rs ^ (rs>>31); result = min(ctlz(norm)-1, 30)
    auto* rs      = load_dreg(src1, "rs");
    auto* sign32  = builder_.CreateAShr(rs, 31, "sign32");
    auto* norm32  = builder_.CreateXor(rs, sign32, "norm32");
    auto* ctlz_fn = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    auto* clz     = builder_.CreateCall(ctlz_fn, {norm32, builder_.getFalse()}, "clz");
    auto* result  = builder_.CreateSub(clz, builder_.getInt32(1), "signbits");
    // ctlz(norm) range: norm=0 (all-same bits) → ctlz=32 → result=31 (max, correct)
    //                   norm nonzero           → ctlz<32 → result in [0,30]
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_SIGNBITS_16L(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.l = SIGNBITS Rs.l: count redundant sign bits of low 16-bit half (result range [0,15])
    // Sign-extend Rs.L to i32, then norm = sext ^ (sext>>31); result = ctlz(norm) - 17
    auto* rs      = load_dreg(src1, "rs");
    auto* trunc16 = builder_.CreateTrunc(rs, builder_.getInt16Ty(), "trunc16");
    auto* sext32  = builder_.CreateSExt(trunc16, builder_.getInt32Ty(), "sext32");
    auto* sign32  = builder_.CreateAShr(sext32, 31, "sign32");
    auto* norm32  = builder_.CreateXor(sext32, sign32, "norm32");
    auto* ctlz_fn = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    auto* clz     = builder_.CreateCall(ctlz_fn, {norm32, builder_.getFalse()}, "clz");
    auto* result  = builder_.CreateSub(clz, builder_.getInt32(17), "signbits");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_SIGNBITS_16H(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.l = SIGNBITS Rs.h: count redundant sign bits of high 16-bit half (result range [0,15])
    // Sign-extend Rs.H to i32, then norm = sext ^ (sext>>31); result = ctlz(norm) - 17
    auto* rs      = load_dreg(src1, "rs");
    auto* hi16    = builder_.CreateTrunc(builder_.CreateLShr(rs, 16), builder_.getInt16Ty(), "hi16");
    auto* sext32  = builder_.CreateSExt(hi16, builder_.getInt32Ty(), "sext32");
    auto* sign32  = builder_.CreateAShr(sext32, 31, "sign32");
    auto* norm32  = builder_.CreateXor(sext32, sign32, "norm32");
    auto* ctlz_fn = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    auto* clz     = builder_.CreateCall(ctlz_fn, {norm32, builder_.getFalse()}, "clz");
    auto* result  = builder_.CreateSub(clz, builder_.getInt32(17), "signbits");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_SIGNBITS_A0(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* ax  = load_cpu_u32(offsetof(CpuState, ax[0]), "a0x");
    auto* aw  = load_cpu_u32(offsetof(CpuState, aw[0]), "a0w");
    auto* res = emit_signbits_acc(ax, aw);
    store_dreg_lo(dst, res);
    return true;
}
bool LiftVisitor::decode_dsp32shift_SIGNBITS_A1(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    auto* ax  = load_cpu_u32(offsetof(CpuState, ax[1]), "a1x");
    auto* aw  = load_cpu_u32(offsetof(CpuState, aw[1]), "a1w");
    auto* res = emit_signbits_acc(ax, aw);
    store_dreg_lo(dst, res);
    return true;
}
bool LiftVisitor::decode_dsp32shift_ONES(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = ONES Rs: count of set bits (popcount) in Rs
    auto* rs       = load_dreg(src1, "rs");
    auto* ctpop_fn = Intrinsic::getDeclaration(module_, Intrinsic::ctpop, {builder_.getInt32Ty()});
    auto* result   = builder_.CreateCall(ctpop_fn, {rs}, "ones");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_EXPADJ_32(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = EXPADJ(Rs, Rt.L): min of signbits(Rs,32) and Rt.L (compare lower 5 bits)
    auto* rs       = load_dreg(src1, "rs");
    auto* rt       = load_dreg(src0, "rt");
    // signbits(rs, 32): norm = rs ^ (rs>>31); sv1 = clz(norm)-1, clamped to [0..30]
    auto* sign     = builder_.CreateAShr(rs, builder_.getInt32(31), "sign");
    auto* norm     = builder_.CreateXor(rs, sign, "norm");
    auto* ctlz_fn  = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    auto* clz      = builder_.CreateCall(ctlz_fn, {norm, builder_.getFalse()}, "clz");
    auto* sv1_raw  = builder_.CreateSub(clz, builder_.getInt32(1), "sv1_raw");
    auto* sv1      = builder_.CreateSelect(
        builder_.CreateICmpUGT(sv1_raw, builder_.getInt32(30)),
        builder_.getInt32(30), sv1_raw, "sv1");
    // Compare lower 5 bits: if sv1[4:0] < Rt[4:0] → sv1 wins, else Rt
    auto* sv1_5    = builder_.CreateAnd(sv1, builder_.getInt32(0x1F), "sv1_5");
    auto* rt_5     = builder_.CreateAnd(rt, builder_.getInt32(0x1F), "rt_5");
    auto* result   = builder_.CreateSelect(
        builder_.CreateICmpULT(sv1_5, rt_5), sv1, rt, "expadj32");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_EXPADJ_V(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = EXPADJ(Rs, Rt.L) (V): three-way min of signbits(Rs.H,16), signbits(Rs.L,16) vs Rt.L
    // Comparison uses lower 4 bits only
    auto* rs       = load_dreg(src1, "rs");
    auto* rt       = load_dreg(src0, "rt");
    auto* ctlz_fn  = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    // signbits(Rs.L, 16): sign-extend Rs.L to i32, norm = sext ^ (sext>>31), tmp = clz-17
    auto* rs_lo_16 = builder_.CreateTrunc(rs, builder_.getInt16Ty(), "rs_lo16");
    auto* rs_lo_se = builder_.CreateSExt(rs_lo_16, builder_.getInt32Ty(), "rs_lo_se");
    auto* sign_lo  = builder_.CreateAShr(rs_lo_se, builder_.getInt32(31), "sign_lo");
    auto* norm_lo  = builder_.CreateXor(rs_lo_se, sign_lo, "norm_lo");
    auto* tmp_lo   = builder_.CreateSub(
        builder_.CreateCall(ctlz_fn, {norm_lo, builder_.getFalse()}, "clz_lo"),
        builder_.getInt32(17), "tmp_lo");
    // signbits(Rs.H, 16): sign-extend Rs.H to i32, norm = sext ^ (sext>>31), tmp = clz-17
    auto* rs_hi_16 = builder_.CreateTrunc(
        builder_.CreateLShr(rs, builder_.getInt32(16)), builder_.getInt16Ty(), "rs_hi16");
    auto* rs_hi_se = builder_.CreateSExt(rs_hi_16, builder_.getInt32Ty(), "rs_hi_se");
    auto* sign_hi  = builder_.CreateAShr(rs_hi_se, builder_.getInt32(31), "sign_hi");
    auto* norm_hi  = builder_.CreateXor(rs_hi_se, sign_hi, "norm_hi");
    auto* tmp_hi   = builder_.CreateSub(
        builder_.CreateCall(ctlz_fn, {norm_hi, builder_.getFalse()}, "clz_hi"),
        builder_.getInt32(17), "tmp_hi");
    // Three-way min comparing lower 4 bits: min(tmp_hi, tmp_lo, Rt.L)
    auto* tmp_lo_4 = builder_.CreateAnd(tmp_lo, builder_.getInt32(0xF), "tmp_lo4");
    auto* tmp_hi_4 = builder_.CreateAnd(tmp_hi, builder_.getInt32(0xF), "tmp_hi4");
    auto* rt_4     = builder_.CreateAnd(rt, builder_.getInt32(0xF), "rt4");
    auto* min_hl   = builder_.CreateSelect(
        builder_.CreateICmpULT(tmp_hi_4, tmp_lo_4), tmp_hi, tmp_lo, "min_hl");
    auto* min_hl_4 = builder_.CreateAnd(min_hl, builder_.getInt32(0xF), "min_hl4");
    auto* result   = builder_.CreateSelect(
        builder_.CreateICmpULT(min_hl_4, rt_4), min_hl, rt, "expadj_v");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_EXPADJ_16L(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = EXPADJ(Rs.L, Rt.L): min of signbits(Rs.L,16) and Rt.L (compare lower 4 bits)
    auto* rs       = load_dreg(src1, "rs");
    auto* rt       = load_dreg(src0, "rt");
    auto* ctlz_fn  = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    auto* rs_lo_16 = builder_.CreateTrunc(rs, builder_.getInt16Ty(), "rs_lo16");
    auto* rs_lo_se = builder_.CreateSExt(rs_lo_16, builder_.getInt32Ty(), "rs_lo_se");
    auto* sign     = builder_.CreateAShr(rs_lo_se, builder_.getInt32(31), "sign");
    auto* norm     = builder_.CreateXor(rs_lo_se, sign, "norm");
    auto* tmp      = builder_.CreateSub(
        builder_.CreateCall(ctlz_fn, {norm, builder_.getFalse()}, "clz"),
        builder_.getInt32(17), "tmp");
    auto* tmp_4    = builder_.CreateAnd(tmp, builder_.getInt32(0xF), "tmp4");
    auto* rt_4     = builder_.CreateAnd(rt, builder_.getInt32(0xF), "rt4");
    auto* result   = builder_.CreateSelect(
        builder_.CreateICmpULT(tmp_4, rt_4), tmp, rt, "expadj_16l");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_EXPADJ_16H(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = EXPADJ(Rs.H, Rt.L): min of signbits(Rs.H,16) and Rt.L (compare lower 4 bits)
    auto* rs       = load_dreg(src1, "rs");
    auto* rt       = load_dreg(src0, "rt");
    auto* ctlz_fn  = Intrinsic::getDeclaration(module_, Intrinsic::ctlz, {builder_.getInt32Ty()});
    auto* rs_hi_16 = builder_.CreateTrunc(
        builder_.CreateLShr(rs, builder_.getInt32(16)), builder_.getInt16Ty(), "rs_hi16");
    auto* rs_hi_se = builder_.CreateSExt(rs_hi_16, builder_.getInt32Ty(), "rs_hi_se");
    auto* sign     = builder_.CreateAShr(rs_hi_se, builder_.getInt32(31), "sign");
    auto* norm     = builder_.CreateXor(rs_hi_se, sign, "norm");
    auto* tmp      = builder_.CreateSub(
        builder_.CreateCall(ctlz_fn, {norm, builder_.getFalse()}, "clz"),
        builder_.getInt32(17), "tmp");
    auto* tmp_4    = builder_.CreateAnd(tmp, builder_.getInt32(0xF), "tmp4");
    auto* rt_4     = builder_.CreateAnd(rt, builder_.getInt32(0xF), "rt4");
    auto* result   = builder_.CreateSelect(
        builder_.CreateICmpULT(tmp_4, rt_4), tmp, rt, "expadj_16h");
    store_dreg_lo(dst, result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_BITMUX_ASR(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // BITMUX(Rs, Rt, A0) (ASR): shift A0 right 2, insert LSBs of Rs/Rt at bits 38/39 of A0
    auto* i64ty   = builder_.getInt64Ty();
    auto* acc     = emit_load_acc_unsigned(0);
    auto* s0      = load_dreg(src0, "s0");
    auto* s1      = load_dreg(src1, "s1");
    // acc = (acc >> 2) | ((s0 & 1) << 38) | ((s1 & 1) << 39)
    auto* acc_sh  = builder_.CreateLShr(acc, builder_.getInt64(2), "acc_sh");
    auto* bit0    = builder_.CreateAnd(builder_.CreateZExt(s0, i64ty), builder_.getInt64(1), "bit0");
    auto* bit1    = builder_.CreateAnd(builder_.CreateZExt(s1, i64ty), builder_.getInt64(1), "bit1");
    auto* acc_new = builder_.CreateOr(
        builder_.CreateOr(acc_sh, builder_.CreateShl(bit0, builder_.getInt64(38))),
        builder_.CreateShl(bit1, builder_.getInt64(39)), "acc_new");
    emit_store_acc(0, acc_new);
    // Rs >>= 1 (logical), Rt >>= 1 (logical)
    store_dreg(src0, builder_.CreateLShr(s0, builder_.getInt32(1), "s0_sh"));
    store_dreg(src1, builder_.CreateLShr(s1, builder_.getInt32(1), "s1_sh"));
    return true;
}
bool LiftVisitor::decode_dsp32shift_BITMUX_ASL(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // BITMUX(Rs, Rt, A0) (ASL): shift A0 left 2, insert MSBs of Rs/Rt at bits 0/1 of A0
    auto* i64ty   = builder_.getInt64Ty();
    auto* acc     = emit_load_acc_unsigned(0);
    auto* s0      = load_dreg(src0, "s0");
    auto* s1      = load_dreg(src1, "s1");
    // acc = (acc << 2) | ((s0 >> 31) & 1) | ((s1 >> 30) & 2)
    auto* acc_sh  = builder_.CreateShl(acc, builder_.getInt64(2), "acc_sh");
    auto* msb0    = builder_.CreateAnd(
        builder_.CreateZExt(builder_.CreateLShr(s0, builder_.getInt32(31)), i64ty),
        builder_.getInt64(1), "msb0");
    auto* msb1    = builder_.CreateAnd(
        builder_.CreateZExt(builder_.CreateLShr(s1, builder_.getInt32(30)), i64ty),
        builder_.getInt64(2), "msb1");
    auto* acc_new = builder_.CreateOr(
        builder_.CreateOr(acc_sh, msb0), msb1, "acc_new");
    emit_store_acc(0, acc_new);
    // Rs <<= 1 (logical), Rt <<= 1 (logical)
    store_dreg(src0, builder_.CreateShl(s0, builder_.getInt32(1), "s0_sh"));
    store_dreg(src1, builder_.CreateShl(s1, builder_.getInt32(1), "s1_sh"));
    return true;
}
bool LiftVisitor::decode_dsp32shift_VITMAX_ASL(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = VIT_MAX(Rs) (ASL): single Viterbi butterfly, A0 shift left 1, decision bit at bit 0
    auto* acc0    = emit_load_acc_unsigned(0);
    // Shift A0 left 1
    auto* acc0_sl = builder_.CreateShl(acc0, builder_.getInt64(1), "acc0_sl");
    // Extract signed 16-bit halves of Rs (src1)
    auto* rs      = load_dreg(src1, "rs");
    auto* rs_hi   = builder_.CreateAShr(rs, builder_.getInt32(16), "rs_hi");
    auto* rs_lo   = builder_.CreateAShr(
        builder_.CreateShl(rs, builder_.getInt32(16)), builder_.getInt32(16), "rs_lo");
    // Decision: (sH - sL) & 0x8000 == 0  →  sH >= sL
    auto* rs_gte  = builder_.CreateICmpEQ(
        builder_.CreateAnd(builder_.CreateSub(rs_hi, rs_lo), builder_.getInt32(0x8000)),
        builder_.getInt32(0), "rs_gte");
    auto* out     = builder_.CreateSelect(rs_gte, rs_hi, rs_lo, "out");
    // If sH >= sL: insert decision bit 1 at bit 0 of A0 (ASL)
    auto* acc0_new = builder_.CreateSelect(rs_gte,
        builder_.CreateOr(acc0_sl, builder_.getInt64(1)), acc0_sl, "acc0_new");
    emit_store_acc(0, acc0_new);
    store_dreg_lo(dst, out);
    return true;
}
bool LiftVisitor::decode_dsp32shift_VITMAX_ASR(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = VIT_MAX(Rs) (ASR): single Viterbi butterfly, A0 logical shift right 1, decision at bit 31
    auto* acc0    = emit_load_acc_unsigned(0);
    // Clear bit 32 (bit 0 of ax in 40-bit), then logical shift right 1: (acc0 & 0xFEFFFFFFFFULL) >> 1
    auto* acc0_cl = builder_.CreateAnd(acc0, builder_.getInt64(0xFEFFFFFFFFULL), "acc0_cl");
    auto* acc0_sr = builder_.CreateLShr(acc0_cl, builder_.getInt64(1), "acc0_sr");
    // Extract signed 16-bit halves of Rs (src1)
    auto* rs      = load_dreg(src1, "rs");
    auto* rs_hi   = builder_.CreateAShr(rs, builder_.getInt32(16), "rs_hi");
    auto* rs_lo   = builder_.CreateAShr(
        builder_.CreateShl(rs, builder_.getInt32(16)), builder_.getInt32(16), "rs_lo");
    // Decision: (sH - sL) & 0x8000 == 0  →  sH >= sL
    auto* rs_gte  = builder_.CreateICmpEQ(
        builder_.CreateAnd(builder_.CreateSub(rs_hi, rs_lo), builder_.getInt32(0x8000)),
        builder_.getInt32(0), "rs_gte");
    auto* out     = builder_.CreateSelect(rs_gte, rs_hi, rs_lo, "out");
    // If sH >= sL: insert decision bit 1 at bit 31 of A0 (ASR)
    auto* acc0_new = builder_.CreateSelect(rs_gte,
        builder_.CreateOr(acc0_sr, builder_.getInt64(0x80000000ULL)), acc0_sr, "acc0_new");
    emit_store_acc(0, acc0_new);
    store_dreg_lo(dst, out);
    return true;
}
bool LiftVisitor::decode_dsp32shift_VITMAX2_ASL(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = VIT_MAX(Rs, Rt) (ASL) — dual Viterbi butterfly, shift A0 left by 2
    auto* i64ty = builder_.getInt64Ty();
    auto* i32ty = builder_.getInt32Ty();

    // Load A0 as sign-extended 40-bit i64, shift left by 2
    auto* acc0 = builder_.CreateShl(emit_load_acc(0), builder_.getInt64(2), "acc0_asl2");

    // Extract signed 16-bit halves of Rt (src0) and Rs (src1)
    auto* rt = load_dreg(src0, "rt");
    auto* rs = load_dreg(src1, "rs");
    auto* rt_hi = builder_.CreateAShr(rt, builder_.getInt32(16), "rt_hi");
    auto* rt_lo = builder_.CreateAShr(builder_.CreateShl(rt, builder_.getInt32(16)),
                                      builder_.getInt32(16), "rt_lo");
    auto* rs_hi = builder_.CreateAShr(rs, builder_.getInt32(16), "rs_hi");
    auto* rs_lo = builder_.CreateAShr(builder_.CreateShl(rs, builder_.getInt32(16)),
                                      builder_.getInt32(16), "rs_lo");

    // Butterfly 0 (Rt): (Rt.H - Rt.L) & 0x8000 == 0 means Rt.H >= Rt.L
    auto* rt_gte  = builder_.CreateICmpEQ(
        builder_.CreateAnd(builder_.CreateSub(rt_hi, rt_lo), builder_.getInt32(0x8000)),
        builder_.getInt32(0), "rt_gte");
    auto* out0    = builder_.CreateSelect(rt_gte, rt_hi, rt_lo, "out0");
    // If Rt.H >= Rt.L, set bit 1 in A0 (ASL variant)
    auto* acc0_b0 = builder_.CreateSelect(rt_gte,
        builder_.CreateOr(acc0, builder_.CreateZExt(builder_.getInt32(2), i64ty)),
        acc0, "acc0_b0");

    // Butterfly 1 (Rs): (Rs.H - Rs.L) & 0x8000 == 0 means Rs.H >= Rs.L
    auto* rs_gte  = builder_.CreateICmpEQ(
        builder_.CreateAnd(builder_.CreateSub(rs_hi, rs_lo), builder_.getInt32(0x8000)),
        builder_.getInt32(0), "rs_gte");
    auto* out1    = builder_.CreateSelect(rs_gte, rs_hi, rs_lo, "out1");
    // If Rs.H >= Rs.L, set bit 0 in A0 (ASL variant)
    auto* acc0_b1 = builder_.CreateSelect(rs_gte,
        builder_.CreateOr(acc0_b0, builder_.CreateZExt(builder_.getInt32(1), i64ty)),
        acc0_b0, "acc0_b1");

    // Write back A0
    store_cpu_u32(offsetof(CpuState, aw[0]), builder_.CreateTrunc(acc0_b1, i32ty, "aw_new"));
    store_cpu_u32(offsetof(CpuState, ax[0]),
        builder_.CreateTrunc(builder_.CreateLShr(acc0_b1, builder_.getInt64(32)), i32ty, "ax_new"));

    // Rd = (out1 & 0xFFFF) << 16 | (out0 & 0xFFFF)
    auto* out0_16 = builder_.CreateAnd(out0, builder_.getInt32(0xFFFF), "out0_16");
    auto* out1_16 = builder_.CreateShl(
        builder_.CreateAnd(out1, builder_.getInt32(0xFFFF)), builder_.getInt32(16), "out1_16");
    store_dreg(dst, builder_.CreateOr(out1_16, out0_16, "rd_out"));
    return true;
}
bool LiftVisitor::decode_dsp32shift_VITMAX2_ASR(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = VIT_MAX(Rs, Rt) (ASR)  — dual Viterbi butterfly, shift A0 right by 2
    // src1 = Rs, src0 = Rt
    auto* i64ty = builder_.getInt64Ty();
    auto* i32ty = builder_.getInt32Ty();

    // Load A0 as sign-extended 40-bit i64, shift right by 2 (arithmetic)
    auto* acc0 = builder_.CreateAShr(emit_load_acc(0), builder_.getInt64(2), "acc0_asr2");

    // Extract signed 16-bit halves of Rt (src0) and Rs (src1)
    auto* rt = load_dreg(src0, "rt");
    auto* rs = load_dreg(src1, "rs");
    auto* rt_hi = builder_.CreateAShr(rt, builder_.getInt32(16), "rt_hi");
    auto* rt_lo = builder_.CreateAShr(builder_.CreateShl(rt, builder_.getInt32(16)),
                                      builder_.getInt32(16), "rt_lo");
    auto* rs_hi = builder_.CreateAShr(rs, builder_.getInt32(16), "rs_hi");
    auto* rs_lo = builder_.CreateAShr(builder_.CreateShl(rs, builder_.getInt32(16)),
                                      builder_.getInt32(16), "rs_lo");

    // Butterfly 0 (Rt): (Rt.H - Rt.L) & 0x8000 == 0 means Rt.H >= Rt.L
    auto* rt_gte  = builder_.CreateICmpEQ(
        builder_.CreateAnd(builder_.CreateSub(rt_hi, rt_lo), builder_.getInt32(0x8000)),
        builder_.getInt32(0), "rt_gte");
    auto* out0    = builder_.CreateSelect(rt_gte, rt_hi, rt_lo, "out0");
    auto* acc0_b0 = builder_.CreateSelect(rt_gte,
        builder_.CreateOr(acc0, builder_.CreateZExt(builder_.getInt32(0x40000000), i64ty)),
        acc0, "acc0_b0");

    // Butterfly 1 (Rs): (Rs.H - Rs.L) & 0x8000 == 0 means Rs.H >= Rs.L
    auto* rs_gte  = builder_.CreateICmpEQ(
        builder_.CreateAnd(builder_.CreateSub(rs_hi, rs_lo), builder_.getInt32(0x8000)),
        builder_.getInt32(0), "rs_gte");
    auto* out1    = builder_.CreateSelect(rs_gte, rs_hi, rs_lo, "out1");
    auto* acc0_b1 = builder_.CreateSelect(rs_gte,
        builder_.CreateOr(acc0_b0, builder_.CreateZExt(builder_.getInt32(0x80000000), i64ty)),
        acc0_b0, "acc0_b1");

    // Write back A0
    store_cpu_u32(offsetof(CpuState, aw[0]), builder_.CreateTrunc(acc0_b1, i32ty, "aw_new"));
    store_cpu_u32(offsetof(CpuState, ax[0]),
        builder_.CreateTrunc(builder_.CreateLShr(acc0_b1, builder_.getInt64(32)), i32ty, "ax_new"));

    // Rd = (out1 & 0xFFFF) << 16 | (out0 & 0xFFFF)  — Rd.H = Rs result, Rd.L = Rt result
    auto* out0_16 = builder_.CreateAnd(out0, builder_.getInt32(0xFFFF), "out0_16");
    auto* out1_16 = builder_.CreateShl(
        builder_.CreateAnd(out1, builder_.getInt32(0xFFFF)), builder_.getInt32(16), "out1_16");
    store_dreg(dst, builder_.CreateOr(out1_16, out0_16, "rd_out"));
    return true;
}
bool LiftVisitor::decode_dsp32shift_EXTRACT_Z(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = EXTRACT(Rs, Rt.L) (Z): zero-extend bit-field from Rs
    // src0=control (Rt): bits[4:0]=len, bits[12:8]=shift; src1=Rs (source value)
    auto* v    = load_dreg(src0, "v");
    auto* x    = load_dreg(src1, "x");
    auto* len  = builder_.CreateAnd(v, builder_.getInt32(0x1F), "len");
    auto* shft = builder_.CreateAnd(builder_.CreateLShr(v, 8), builder_.getInt32(0x1F), "shft");
    auto* mask = builder_.CreateSub(builder_.CreateShl(builder_.getInt32(1), len), builder_.getInt32(1), "mask");
    auto* x_sh = builder_.CreateLShr(x, shft, "x_sh");
    auto* result = builder_.CreateAnd(x_sh, mask, "extract");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_EXTRACT_X(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = EXTRACT(Rs, Rt.L) (X): sign-extend bit-field from Rs
    auto* v    = load_dreg(src0, "v");
    auto* x    = load_dreg(src1, "x");
    auto* len  = builder_.CreateAnd(v, builder_.getInt32(0x1F), "len");
    auto* shft = builder_.CreateAnd(builder_.CreateLShr(v, 8), builder_.getInt32(0x1F), "shft");
    auto* mask = builder_.CreateSub(builder_.CreateShl(builder_.getInt32(1), len), builder_.getInt32(1), "mask");
    auto* x_sh = builder_.CreateLShr(x, shft, "x_sh");
    auto* extracted = builder_.CreateAnd(x_sh, mask, "extracted");
    // Sign-extend: sgn = (1 << len) >> 1 = top bit of the extracted field
    // Note: this differs from mask>>1 when len=1 (mask=0 but sgn=1)
    auto* sgn    = builder_.CreateLShr(builder_.CreateShl(builder_.getInt32(1), len), builder_.getInt32(1), "sgn");
    auto* is_neg = builder_.CreateICmpNE(
        builder_.CreateAnd(extracted, sgn), builder_.getInt32(0), "is_neg");
    auto* sext   = builder_.CreateOr(extracted, builder_.CreateNot(mask), "sext");
    auto* result = builder_.CreateSelect(is_neg, sext, extracted, "extract_x");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_DEPOSIT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = DEPOSIT(Rs, Rt): insert fg bit-field into bg
    // src0 = fg (control): fg[4:0]=len, fg[12:8]=shft, fg[31:16]=bits to deposit
    // src1 = bg (background value to deposit into)
    auto* fg  = load_dreg(src0, "fg");
    auto* bg  = load_dreg(src1, "bg");
    auto* len  = builder_.CreateAnd(fg, builder_.getInt32(0x1f), "len");
    auto* shft = builder_.CreateAnd(
                     builder_.CreateLShr(fg, builder_.getInt32(8)), builder_.getInt32(0x1f), "shft");
    auto* one  = builder_.getInt32(1);
    // Cap len at 16 to match reference: mask = (1 << min(16, len)) - 1
    auto* len16 = builder_.CreateSelect(
        builder_.CreateICmpULT(len, builder_.getInt32(16)), len, builder_.getInt32(16), "len16");
    auto* mask = builder_.CreateSub(builder_.CreateShl(one, len16), one, "mask");
    auto* fgnd = builder_.CreateAnd(
                     builder_.CreateLShr(fg, builder_.getInt32(16)), mask, "fgnd");
    auto* fgnd_s = builder_.CreateShl(fgnd, shft, "fgnd_s");
    auto* mask_s = builder_.CreateShl(mask, shft, "mask_s");
    auto* bg_clr = builder_.CreateAnd(bg, builder_.CreateNot(mask_s), "bg_clr");
    auto* result = builder_.CreateOr(bg_clr, fgnd_s, "deposit");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_DEPOSIT_X(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = DEPOSIT(Rs, Rt) (X): sign-extend fg bit-field before depositing
    auto* fg  = load_dreg(src0, "fg");
    auto* bg  = load_dreg(src1, "bg");
    auto* len  = builder_.CreateAnd(fg, builder_.getInt32(0x1F), "len");
    auto* shft = builder_.CreateAnd(builder_.CreateLShr(fg, 8), builder_.getInt32(0x1F), "shft");
    // Extract fg bits from fg[31:16], capped at 16 bits
    auto* len16 = builder_.CreateSelect(
        builder_.CreateICmpULT(len, builder_.getInt32(16)), len, builder_.getInt32(16), "len16");
    auto* mask16 = builder_.CreateSub(
        builder_.CreateShl(builder_.getInt32(1), len16), builder_.getInt32(1), "mask16");
    auto* fgnd = builder_.CreateAnd(builder_.CreateLShr(fg, 16), mask16, "fgnd");
    // Sign-extend fgnd within 16 bits: (bs32)(bs16)(fgnd << (16-len)) >> (16-len)
    auto* shift_amt = builder_.CreateSub(builder_.getInt32(16), len, "shift_amt");
    auto* shifted_l = builder_.CreateShl(fgnd, shift_amt, "shifted_l");
    // Trunc to i16 then SExt to i32 = sign extends from bit 15
    auto* as_i16  = builder_.CreateTrunc(shifted_l, builder_.getInt16Ty(), "as_i16");
    auto* sext32  = builder_.CreateSExt(as_i16, builder_.getInt32Ty(), "sext32");
    auto* fgnd_se = builder_.CreateAShr(sext32, shift_amt, "fgnd_se");
    // mask = -1 (all ones); shift fg bits into position
    auto* fgnd_s = builder_.CreateShl(fgnd_se, shft, "fgnd_s");
    auto* mask_s = builder_.CreateShl(builder_.getInt32(-1), shft, "mask_s");
    auto* bg_clr = builder_.CreateAnd(bg, builder_.CreateNot(mask_s), "bg_clr");
    auto* result = builder_.CreateOr(bg_clr, fgnd_s, "deposit_x");
    store_dreg(dst, result);
    emit_flags_logic(result);
    return true;
}
bool LiftVisitor::decode_dsp32shift_BXORSHIFT(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = CC = BXORSHIFT(A0, Rs): shift A0 left 1, CC = xor_reduce(new_A0, Rs), store back
    auto* i64ty  = builder_.getInt64Ty();
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);

    auto* acc0 = emit_load_acc_unsigned(0);

    // Shift A0 left 1, masked to 40 bits
    auto* acc0_sh = builder_.CreateAnd(
        builder_.CreateShl(acc0, builder_.getInt64(1)), mask40, "acc0_sh");

    // xor_reduce(acc0_sh, Rs): parity of bitwise AND, 40 bits
    auto* rs64 = builder_.CreateZExt(load_dreg(src0, "rs"), i64ty, "rs64");
    auto* xr = emit_xor_reduce_parity(acc0_sh, rs64);

    store_cc(xr);
    store_dreg_lo(dst, xr);

    // Write shifted A0 back
    emit_store_acc(0, acc0_sh);
    return true;
}
bool LiftVisitor::decode_dsp32shift_BXOR(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = CC = BXOR(A0, Rs): CC = xor_reduce(A0, Rs), A0 unchanged
    auto* i64ty  = builder_.getInt64Ty();

    auto* acc0 = emit_load_acc_unsigned(0);

    auto* rs64 = builder_.CreateZExt(load_dreg(src0, "rs"), i64ty, "rs64");
    auto* xr = emit_xor_reduce_parity(acc0, rs64);

    store_cc(xr);
    store_dreg_lo(dst, xr);
    return true;
}
bool LiftVisitor::decode_dsp32shift_BXORSHIFT3(
        uint32_t M, uint32_t hh, uint32_t ddd, uint32_t aaa, uint32_t bbb) {
    // A0 = BXORSHIFT(A0, A1, CC): LFSR left-shift with parity feedback
    auto* i64ty  = builder_.getInt64Ty();
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);

    auto* acc0 = emit_load_acc_unsigned(0);
    auto* acc1 = emit_load_acc_unsigned(1);

    // xor_reduce(acc0, acc1): parity of (acc0 & acc1) across 40 bits
    auto* xr = builder_.CreateZExt(
        emit_xor_reduce_parity(builder_.CreateAnd(acc0, mask40), builder_.CreateAnd(acc1, mask40)),
        i64ty, "xr64");

    // Load CC
    auto* cc = builder_.CreateZExt(
        load_cpu_u32(offsetof(CpuState, cc), "cc"), i64ty, "cc64");

    // New A0 = ((acc0 << 1) | (CC ^ xr)) & mask40
    auto* shifted  = builder_.CreateShl(acc0, builder_.getInt64(1), "acc0_sh");
    auto* feedback = builder_.CreateXor(cc, xr, "feedback");
    auto* new_acc0 = builder_.CreateAnd(
        builder_.CreateOr(shifted, feedback), mask40, "new_acc0");

    emit_store_acc(0, new_acc0);
    return true;
}
bool LiftVisitor::decode_dsp32shift_BXOR3(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd.L = CC = BXOR(A0, A1, CC): new_CC = old_CC ^ xor_reduce(A0,A1)
    // A0 is NOT written back
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);

    auto* acc0 = emit_load_acc_unsigned(0);
    auto* acc1 = emit_load_acc_unsigned(1);

    auto* xr = emit_xor_reduce_parity(builder_.CreateAnd(acc0, mask40), builder_.CreateAnd(acc1, mask40));

    // new_CC = old_CC ^ xr
    auto* old_cc = load_cpu_u32(offsetof(CpuState, cc), "cc");
    auto* new_cc = builder_.CreateXor(old_cc, xr, "new_cc");

    store_cc(new_cc);
    store_dreg_lo(dst, new_cc);
    return true;
}
bool LiftVisitor::decode_dsp32shift_ALIGN8(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = ALIGN8(Rs, Rt): (Rs << 24) | (Rt >> 8)
    auto* rs = load_dreg(src1, "rs");
    auto* rt = load_dreg(src0, "rt");
    store_dreg(dst, emit_fshr(rs, rt, builder_.getInt32(8)));
    return true;
}
bool LiftVisitor::decode_dsp32shift_ALIGN16(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = ALIGN16(Rs, Rt): (Rs << 16) | (Rt >> 16)
    auto* rs = load_dreg(src1, "rs");
    auto* rt = load_dreg(src0, "rt");
    store_dreg(dst, emit_fshr(rs, rt, builder_.getInt32(16)));
    return true;
}
bool LiftVisitor::decode_dsp32shift_ALIGN24(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    // Rd = ALIGN24(Rs, Rt): (Rs << 8) | (Rt >> 24)
    auto* rs = load_dreg(src1, "rs");
    auto* rt = load_dreg(src0, "rt");
    store_dreg(dst, emit_fshr(rs, rt, builder_.getInt32(24)));
    return true;
}

// dsp32shiftimm stubs
bool LiftVisitor::decode_dsp32shiftimm_ASHIFT16_arith(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // dReg_h/l = dReg_h/l >>> newimmag — 16-bit arithmetic right shift by immediate
    // newimmag is 6-bit two's-complement negation of field (b<<5 | imm5)
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>((b << 5) | imm5))) & 0x3fu;
    auto* rs = load_dreg(src1, "rs");
    auto* in16 = emit_extract_half16(rs, HLs);
    auto* in16_sext = builder_.CreateSExt(
        builder_.CreateTrunc(in16, builder_.getInt16Ty()), builder_.getInt32Ty(), "in_sext");
    llvm::Value* result16;
    llvm::Value* v_flag;
    if (newimmag > 16) {
        uint32_t lamt = 16u - (newimmag & 0xFu);
        auto* shifted = builder_.CreateShl(in16, builder_.getInt32(lamt), "wrap_left");
        result16 = builder_.CreateAnd(shifted, builder_.getInt32(0xFFFF), "result16");
        auto* bits_lost = builder_.CreateLShr(shifted, builder_.getInt32(16), "bits_lost");
        uint32_t all_ones_bits = (1u << lamt) - 1u;  // lamt in [1,15], safe
        auto* v_i = emit_lshift16_vi_imm(bits_lost, result16, all_ones_bits);
        // call-site sign-change check (refs/bfin-sim.c:5857-5861)
        auto* in_sign    = builder_.CreateLShr(in16, builder_.getInt32(15), "in_sign");
        auto* out_sign   = builder_.CreateLShr(result16, builder_.getInt32(15), "out_sign");
        auto* sign_chg   = builder_.CreateICmpNE(in_sign, out_sign, "sign_chg");
        v_flag = builder_.CreateZExt(
            builder_.CreateOr(v_i, sign_chg), builder_.getInt32Ty(), "v_flag");
    } else {
        auto* shifted = builder_.CreateAShr(in16_sext, builder_.getInt32(newimmag), "ashr16");
        result16 = builder_.CreateAnd(shifted, builder_.getInt32(0xFFFF), "result16");
        v_flag = builder_.getInt32(0);
    }
    emit_merge_half16(dst, result16, HLs);
    emit_v_vs_update(v_flag);
    emit_flags_az_an_16(result16);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_ASHIFT16S_left(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // dReg_h/l = dReg_h/l << imm5 (S) — 16-bit left shift with saturation
    // HLs: bit0 selects source half (0=lo, 1=hi), bit1 selects dest half (0=lo, 1=hi)
    auto* rs = load_dreg(src1, "rs");
    auto* in16 = emit_extract_half16(rs, HLs);
    auto* in_sign = builder_.CreateLShr(in16, 15, "in_sign");
    // Sign-extend the 16-bit value to 32 bits before shifting to detect overflow correctly.
    // Simply checking shifted bits > bit 15 is wrong for negative inputs: a negative value
    // can have its top bits spill into the 32-bit container without actually overflowing 16-bit.
    auto* in16_sext = builder_.CreateSExt(
        builder_.CreateTrunc(in16, builder_.getInt16Ty()), builder_.getInt32Ty(), "in_sext");
    auto* shifted32 = builder_.CreateShl(in16_sext, builder_.getInt32(imm5), "shifted32");
    // Overflow iff the 32-bit shifted value doesn't round-trip through 16-bit sign extension
    auto* sext_back = builder_.CreateSExt(
        builder_.CreateTrunc(shifted32, builder_.getInt16Ty()), builder_.getInt32Ty(), "sext_back");
    auto* overflow = builder_.CreateICmpNE(shifted32, sext_back, "overflow");
    auto* sat_pos = builder_.getInt32(0x7FFF);
    auto* sat_neg = builder_.getInt32(0x8000);
    auto* saturated = builder_.CreateSelect(
        builder_.CreateICmpNE(in_sign, builder_.getInt32(0)), sat_neg, sat_pos, "saturated");
    auto* result16 = builder_.CreateSelect(overflow, saturated, shifted32, "result16");
    result16 = builder_.CreateAnd(result16, builder_.getInt32(0xFFFF), "result16_mask");
    emit_merge_half16(dst, result16, HLs);
    auto* v_flag = builder_.CreateZExt(overflow, builder_.getInt32Ty(), "v_flag");
    emit_v_vs_update(v_flag);
    emit_flags_az_an_16(result16);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_ASHIFT16S_arith(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // dReg_h/l = dReg_h/l >>> newimmag (S) — 16-bit arithmetic right shift with saturation
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>(0x20u | imm5))) & 0x3fu;
    auto* rs = load_dreg(src1, "rs");
    auto* in16 = emit_extract_half16(rs, HLs);
    auto* in16_sext = builder_.CreateSExt(
        builder_.CreateTrunc(in16, builder_.getInt16Ty()), builder_.getInt32Ty(), "in_sext");
    llvm::Value* result16;
    llvm::Value* v_flag;
    if (newimmag > 16) {
        uint32_t shift_left = 32u - newimmag;
        auto* shifted = builder_.CreateShl(in16, builder_.getInt32(shift_left), "wrap_left");
        auto* in_sign = builder_.CreateLShr(in16, 15, "in_sign");
        auto* out_sign = builder_.CreateAnd(builder_.CreateLShr(shifted, 15), builder_.getInt32(1), "out_sign");
        // Reference truncates to bu16 before overflow check — only sign change matters
        auto* overflow = builder_.CreateICmpNE(in_sign, out_sign, "overflow");
        auto* sat_neg = builder_.getInt32(0x8000);
        auto* sat_pos = builder_.getInt32(0x7FFF);
        auto* saturated = builder_.CreateSelect(
            builder_.CreateICmpNE(in_sign, builder_.getInt32(0)), sat_neg, sat_pos, "saturated");
        result16 = builder_.CreateSelect(overflow, saturated,
            builder_.CreateAnd(shifted, builder_.getInt32(0xFFFF)), "result16");
        v_flag = builder_.CreateZExt(overflow, builder_.getInt32Ty(), "v_flag");
    } else {
        auto* shifted = builder_.CreateAShr(in16_sext, builder_.getInt32(newimmag), "ashr16");
        result16 = builder_.CreateAnd(shifted, builder_.getInt32(0xFFFF), "result16");
        v_flag = builder_.getInt32(0);
    }
    emit_merge_half16(dst, result16, HLs);
    emit_v_vs_update(v_flag);
    emit_flags_az_an_16(result16);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_LSHIFT16_left(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd.H/L = Rs.H/L << imm5  — 16-bit logical left shift by immediate
    auto* rs = load_dreg(src1, "rs");
    auto* in16 = emit_extract_half16(rs, HLs);
    auto* shifted = builder_.CreateShl(in16, builder_.getInt32(imm5), "shifted");
    auto* result16 = builder_.CreateAnd(shifted, builder_.getInt32(0xFFFF), "result16");
    emit_merge_half16(dst, result16, HLs);
    // V: lshift v_i = NOT(bits_lost==0 OR (bits_lost==all_ones_mask && result_neg))
    // refs/bfin-sim.c:5921 calls lshift(in, immag, 16, sat=0, ov=1) — V can be set
    auto* bits_lost = builder_.CreateLShr(shifted, builder_.getInt32(16), "bits_lost");
    uint32_t all_ones_bits = (imm5 == 0) ? 0u : ((1u << imm5) - 1u);
    auto* v_flag = builder_.CreateZExt(
        emit_lshift16_vi_imm(bits_lost, result16, all_ones_bits),
        builder_.getInt32Ty(), "v_flag");
    emit_v_vs_update(v_flag);
    emit_flags_az_an_16(result16);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_LSHIFT16_right(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd.H/L = Rs.H/L >> newimmag  — 16-bit logical right shift by immediate
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>(0x20u | imm5))) & 0x1fu;
    if (newimmag > 16) newimmag = 16;  // clamp: shifting >=16 zeros the 16-bit value
    auto* rs = load_dreg(src1, "rs");
    auto* in16 = emit_extract_half16(rs, HLs);
    auto* result16 = builder_.CreateLShr(in16, builder_.getInt32(newimmag), "result16");
    emit_merge_half16(dst, result16, HLs);
    store_v(builder_.getInt32(0));
    emit_flags_az_an_16(result16);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_VASHIFT_arith(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // Rd = Rs >>> newimmag (V) — vector 16x2 arithmetic right shift by immediate
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>((b << 5) | imm5))) & 0x1fu;
    auto* rs   = load_dreg(src1, "rs");
    auto* val0 = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1 = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    // Sign-extend each half to i32 for arithmetic shift
    auto* se0 = builder_.CreateAShr(builder_.CreateShl(val0, 16), 16, "se0");
    auto* se1 = builder_.CreateAShr(builder_.CreateShl(val1, 16), 16, "se1");
    llvm::Value* out0, *out1, *v_flag;
    if (newimmag > 16) {
        // Reference wraps to left-shift by (16 - (newimmag & 0xF)), no saturation
        // V = v_i0 || v_i1 || sign_chg0 || sign_chg1  (refs/bfin-sim.c lines 6070-6078)
        // v_i uses full lshift formula: NOT(bits_lost==0 OR (bits_lost==all_ones && result_neg))
        uint32_t lamt = 16u - (newimmag & 0xFu);
        std::tie(out0, out1, v_flag) = emit_vashift_wrap(val0, val1, lamt);
    } else {
        out0 = builder_.CreateAnd(
            builder_.CreateAShr(se0, builder_.getInt32(newimmag)), builder_.getInt32(0xFFFF), "out0");
        out1 = builder_.CreateAnd(
            builder_.CreateAShr(se1, builder_.getInt32(newimmag)), builder_.getInt32(0xFFFF), "out1");
        v_flag = builder_.getInt32(0);
    }
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    emit_v_vs_update(v_flag);
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_VASHIFTS_left(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd = Rs << imm5 (V,S) — vector 16x2 saturating shift by immediate (signed 5-bit count)
    // imm5 is a 5-bit signed field: 0-15 = left shift, 16-31 = right shift (count - 32)
    // Reference: bfin-sim.c line 6001: count = imm5(immag); if (count>=0) lshift else ashiftrt
    auto* i32ty = builder_.getInt32Ty();
    auto* rs   = load_dreg(src1, "rs");
    auto* val0 = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1 = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");

    // Sign-extend the 5-bit immediate at JIT-generation time
    int count = (imm5 & 0x10) ? (int)imm5 - 32 : (int)imm5;

    llvm::Value* r0, *r1, *v_flag;
    if (count < 0) {
        // Arithmetic right shift by -count; no saturation, V not set by this op
        int ramt = -count;
        auto* se0 = builder_.CreateAShr(builder_.CreateShl(val0, 16), 16, "se0");
        auto* se1 = builder_.CreateAShr(builder_.CreateShl(val1, 16), 16, "se1");
        r0 = builder_.CreateAnd(
            builder_.CreateAShr(se0, builder_.getInt32(ramt)), builder_.getInt32(0xFFFF), "r0");
        r1 = builder_.CreateAnd(
            builder_.CreateAShr(se1, builder_.getInt32(ramt)), builder_.getInt32(0xFFFF), "r1");
        auto* result = builder_.CreateOr(builder_.CreateShl(r1, 16), r0, "result");
        store_dreg(dst, result);
        emit_v_vs_update(builder_.getInt32(0));
        // AZ/AN: per-half, OR'd across both halves (mirrors SET_ASTAT(ASTAT|astat) in reference)
        auto* az0 = builder_.CreateICmpEQ(r0, builder_.getInt32(0), "az0");
        auto* az1 = builder_.CreateICmpEQ(r1, builder_.getInt32(0), "az1");
        store_cpu_u32(offsetof(CpuState, az),
            builder_.CreateZExt(builder_.CreateOr(az0, az1), i32ty, "az"));
        auto* an_lo = builder_.CreateLShr(r0, builder_.getInt32(15), "an_lo");
        auto* an_hi = builder_.CreateLShr(r1, builder_.getInt32(15), "an_hi");
        store_cpu_u32(offsetof(CpuState, an), builder_.CreateOr(an_lo, an_hi, "an"));
        return true;
    } else {
        // Per-half saturating left shift — use sign-extend round-trip to detect overflow correctly.
        // Checking overflow_bits > bit15 is wrong for negative halves: a negative 16-bit value
        // shifted left can spill into the 32-bit container without actually overflowing 16 bits.
        auto sat_half = [&](llvm::Value* in16, const char* tag) -> std::pair<llvm::Value*, llvm::Value*> {
            std::string pfx(tag);
            auto* i16ty    = builder_.getInt16Ty();
            auto* in_sign  = builder_.CreateLShr(in16, 15, (pfx + "_ins").c_str());
            auto* in_sext  = builder_.CreateSExt(builder_.CreateTrunc(in16, i16ty), i32ty,
                                                  (pfx + "_sext").c_str());
            auto* shifted32 = builder_.CreateShl(in_sext, builder_.getInt32(count), (pfx + "_sh").c_str());
            auto* sext_back = builder_.CreateSExt(
                builder_.CreateTrunc(shifted32, i16ty), i32ty, (pfx + "_sb").c_str());
            auto* ov       = builder_.CreateICmpNE(shifted32, sext_back, (pfx + "_ov").c_str());
            auto* sat = builder_.CreateSelect(
                builder_.CreateICmpNE(in_sign, builder_.getInt32(0)),
                builder_.getInt32(0x8000), builder_.getInt32(0x7FFF), (pfx + "_sat").c_str());
            auto* res = builder_.CreateAnd(
                builder_.CreateSelect(ov, sat, shifted32), builder_.getInt32(0xFFFF),
                (pfx + "_res").c_str());
            return {res, builder_.CreateZExt(ov, i32ty, (pfx + "_ovf").c_str())};
        };
        auto [res0, ov0] = sat_half(val0, "h0");
        auto [res1, ov1] = sat_half(val1, "h1");
        r0 = res0; r1 = res1;
        v_flag = builder_.CreateOr(ov0, ov1, "v_flag");
    }

    auto* result = builder_.CreateOr(builder_.CreateShl(r1, 16), r0, "result");
    store_dreg(dst, result);
    emit_v_vs_update(v_flag);
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(r0, r1);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_VASHIFTS_arith(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd = Rs >>> newimmag (V,S) — vector 16x2 saturating arithmetic right shift by immediate
    // Reference uses same code path as VASHIFT_arith for the arithmetic right case
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>(0x20u | imm5))) & 0x1fu;
    auto* rs   = load_dreg(src1, "rs");
    auto* val0 = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1 = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    auto* se0  = builder_.CreateAShr(builder_.CreateShl(val0, 16), 16, "se0");
    auto* se1  = builder_.CreateAShr(builder_.CreateShl(val1, 16), 16, "se1");
    llvm::Value* out0, *out1, *v_flag;
    if (newimmag > 16) {
        // Reference (refs/bfin-sim.c:6055) handles VASHIFT and VASHIFTS in the SAME code path:
        // calls lshift(val, 16-(count&0xF), 16, sat=0, ov=1) — NO saturation even for S variant.
        // Then additionally sets V on sign change. lamt = 16-(newimmag&0xF) = 32-newimmag.
        uint32_t lamt = 32u - newimmag;  // == 16 - (newimmag & 0xF) for newimmag in [17,31]
        std::tie(out0, out1, v_flag) = emit_vashift_wrap(val0, val1, lamt);
    } else {
        out0 = builder_.CreateAnd(
            builder_.CreateAShr(se0, builder_.getInt32(newimmag)), builder_.getInt32(0xFFFF), "out0");
        out1 = builder_.CreateAnd(
            builder_.CreateAShr(se1, builder_.getInt32(newimmag)), builder_.getInt32(0xFFFF), "out1");
        v_flag = builder_.getInt32(0);
    }
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    emit_v_vs_update(v_flag);
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_VLSHIFT_left(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd = Rs << imm5 (V) — vector 16x2 logical left shift by immediate
    auto* rs   = load_dreg(src1, "rs");
    auto* val0 = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1 = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    auto* shifted0 = builder_.CreateShl(val0, builder_.getInt32(imm5), "sh0");
    auto* shifted1 = builder_.CreateShl(val1, builder_.getInt32(imm5), "sh1");
    auto* out0 = builder_.CreateAnd(shifted0, builder_.getInt32(0xFFFF), "out0");
    auto* out1 = builder_.CreateAnd(shifted1, builder_.getInt32(0xFFFF), "out1");
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    // V: lshift v_i per half ORed (refs/bfin-sim.c:6048 calls lshift(., ov=1))
    uint32_t all_ones_bits = (imm5 == 0) ? 0u : ((1u << imm5) - 1u);
    auto* bl0 = builder_.CreateLShr(shifted0, builder_.getInt32(16), "bl0");
    auto* bl1 = builder_.CreateLShr(shifted1, builder_.getInt32(16), "bl1");
    auto* vi0 = emit_lshift16_vi_imm(bl0, out0, all_ones_bits);
    auto* vi1 = emit_lshift16_vi_imm(bl1, out1, all_ones_bits);
    auto* v_flag = builder_.CreateZExt(
        builder_.CreateOr(vi0, vi1), builder_.getInt32Ty(), "v_flag");
    emit_v_vs_update(v_flag);
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_VLSHIFT_right(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd = Rs >> newimmag (V) — vector 16x2 logical right shift by immediate
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>(0x20u | imm5))) & 0x1fu;
    if (newimmag > 16) newimmag = 16;
    auto* rs   = load_dreg(src1, "rs");
    auto* val0 = builder_.CreateAnd(rs, builder_.getInt32(0xFFFF), "val0");
    auto* val1 = builder_.CreateAnd(builder_.CreateLShr(rs, 16), builder_.getInt32(0xFFFF), "val1");
    auto* out0 = builder_.CreateLShr(val0, builder_.getInt32(newimmag), "out0");
    auto* out1 = builder_.CreateLShr(val1, builder_.getInt32(newimmag), "out1");
    auto* result = builder_.CreateOr(builder_.CreateShl(out1, 16), out0, "result");
    store_dreg(dst, result);
    store_v(builder_.getInt32(0));
    // AZ = either half zero, AN = either half negative (reference ORs flags from both halves)
    emit_flags_az_an_v2x16(out0, out1);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_ASHIFT32_arith(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // Rd = Rs >>> newimmag — 32-bit arithmetic shift by immediate
    // The reference uses a signed imm6 interpretation: negative count means left shift
    uint32_t raw_newimmag = static_cast<uint32_t>(-(static_cast<int32_t>((b << 5) | imm5))) & 0x3fu;
    // Sign-extend 6-bit to determine direction: negative = left, non-negative = right
    int count = (raw_newimmag & 0x20) ? static_cast<int>(raw_newimmag | ~0x3fu) : static_cast<int>(raw_newimmag);
    auto* rs = load_dreg(src1, "rs");
    llvm::Value* result;
    llvm::Value* v_flag;
    if (count < 0) {
        // Left-shift by -count (no saturation for non-S variant)
        int lamt = -count;
        result = builder_.CreateShl(rs, builder_.getInt32(lamt), "shl32");
        // V: detect sign change
        auto* in_sign = builder_.CreateLShr(rs, 31, "in_sign");
        auto* out_sign = builder_.CreateLShr(result, 31, "out_sign");
        v_flag = builder_.CreateZExt(
            builder_.CreateICmpNE(in_sign, out_sign), builder_.getInt32Ty(), "v_flag");
    } else {
        // Arithmetic right shift by count
        if (count >= 32) count = 31;  // clamp: AShr by >=32 is UB in LLVM
        result = builder_.CreateAShr(rs, builder_.getInt32(count), "ashr32");
        v_flag = builder_.getInt32(0);
    }
    store_dreg(dst, result);
    emit_v_vs_update(v_flag);
    emit_flags_az_an(result);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_ASHIFT32S_left(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // Rd = Rs << immag (S) — 32-bit shift with saturation
    // immag is a 6-bit signed field: b is bit5 (sign), imm5 is bits[4:0]
    // count < 0 → arithmetic right shift; count >= 0 → left shift with saturation
    int count = static_cast<int>((b << 5) | imm5);
    if (count & 0x20) count |= ~0x3f;  // sign-extend from 6 bits

    auto* rs = load_dreg(src1, "rs");
    if (count < 0) {
        // Arithmetic right shift (no saturation)
        int ramt = -count;
        if (ramt >= 32) ramt = 31;
        auto* result = builder_.CreateAShr(rs, builder_.getInt32(ramt), "ashr32s");
        store_dreg(dst, result);
        emit_v_vs_update(builder_.getInt32(0));
        emit_flags_az_an(result);
    } else {
        // Left shift with saturation
        auto* i32ty = builder_.getInt32Ty();
        auto* i64ty = builder_.getInt64Ty();
        auto* in_sign = builder_.CreateLShr(rs, builder_.getInt32(31), "in_sign");
        auto* rs64 = builder_.CreateSExt(rs, i64ty, "rs64");
        auto* left64 = builder_.CreateShl(rs64, builder_.getInt64(count), "left64");
        auto* left_shifted = builder_.CreateTrunc(left64, i32ty, "left_shifted");
        auto* left_sext64 = builder_.CreateSExt(left_shifted, i64ty, "left_sext64");
        auto* overflow = builder_.CreateICmpNE(left64, left_sext64, "overflow");
        auto* sat_neg = builder_.getInt32(0x80000000);
        auto* sat_pos = builder_.getInt32(0x7FFFFFFF);
        auto* saturated = builder_.CreateSelect(
            builder_.CreateICmpNE(in_sign, builder_.getInt32(0)), sat_neg, sat_pos, "saturated");
        auto* final_result = builder_.CreateSelect(overflow, saturated, left_shifted, "final_result");
        store_dreg(dst, final_result);
        auto* v_flag = builder_.CreateZExt(overflow, i32ty, "v_flag");
        emit_v_vs_update(v_flag);
        emit_flags_az_an(final_result);
    }
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_LSHIFT32_left(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd = Rs << imm5  (logical left shift, 32-bit)
    auto* rs = load_dreg(src1, "rs");
    auto* result = builder_.CreateShl(rs, builder_.getInt32(imm5), "lsl32");
    store_dreg(dst, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_LSHIFT32_right(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // Rd = Rs >> newimmag  (logical right shift, 32-bit)
    // bit8=1 encoding: newimmag = (-(0x20 | imm5)) & 0x3f
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>(0x20u | imm5))) & 0x3fu;
    auto* rs = load_dreg(src1, "rs");
    llvm::Value* result;
    if (newimmag >= 32)
        result = builder_.getInt32(0);
    else
        result = builder_.CreateLShr(rs, builder_.getInt32(newimmag), "lsr32");
    store_dreg(dst, result);
    emit_flags_az_an(result);
    store_v(builder_.getInt32(0));
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_ROT32(
        uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // Rd = ROT Rs BY immag: compile-time 33-bit rotate through CC
    uint32_t immag = (b << 5) | imm5;
    // imm6 sign-extend: 6-bit signed
    int32_t shift = static_cast<int32_t>(immag << 26) >> 26;
    auto* i32ty  = builder_.getInt32Ty();
    auto* i64ty  = builder_.getInt64Ty();
    auto* rs     = load_dreg(src1, "rs");
    auto* cc_old = load_cpu_u32(offsetof(CpuState, cc), "cc_old");
    if (shift == 0) {
        // Zero shift: no-op, no CC update
        store_dreg(dst, rs);
        return true;
    }
    // Normalize negative shift to equivalent positive left rotation
    if (shift < 0) shift += 33;
    // Now shift is in [1..32]
    auto* rs64 = builder_.CreateZExt(rs, i64ty, "rs64");
    auto* cc64 = builder_.CreateZExt(cc_old, i64ty, "cc64");
    // left_part = (shift == 32) ? 0 : rs64 << shift
    llvm::Value* left_part;
    if (shift == 32)
        left_part = builder_.getInt64(0);
    else
        left_part = builder_.CreateShl(rs64, builder_.getInt64(shift), "left_part");
    // right_part = (shift == 1) ? 0 : rs64 >> (33 - shift)
    llvm::Value* right_part;
    if (shift == 1)
        right_part = builder_.getInt64(0);
    else
        right_part = builder_.CreateLShr(rs64, builder_.getInt64(33 - shift), "right_part");
    // cc_in = cc64 << (shift - 1)
    auto* cc_in  = builder_.CreateShl(cc64, builder_.getInt64(shift - 1), "cc_in");
    auto* new_rs = builder_.CreateTrunc(
        builder_.CreateAnd(
            builder_.CreateOr(builder_.CreateOr(left_part, right_part), cc_in),
            builder_.getInt64(0xFFFFFFFFULL)),
        i32ty, "rd_new");
    store_dreg(dst, new_rs);
    // new_cc = (rs >> (32 - shift)) & 1
    auto* new_cc = builder_.CreateTrunc(
        builder_.CreateAnd(
            builder_.CreateLShr(rs64, builder_.getInt64(32 - shift)),
            builder_.getInt64(1)),
        i32ty, "new_cc");
    store_cc(new_cc);
    return true;
}
bool LiftVisitor::emit_acc_ashift_left_imm(int n, uint32_t imm5) {
    // An = An << imm5 — 40-bit accumulator left shift by immediate
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* acc    = emit_load_acc(n);
    auto* result = builder_.CreateAnd(
        builder_.CreateShl(acc, builder_.getInt64(imm5)), mask40, "left_result");
    emit_store_acc(n, result);
    size_t av_off = n == 0 ? offsetof(CpuState, av0) : offsetof(CpuState, av1);
    store_cpu_u32(av_off, builder_.getInt32(0));
    emit_flags_az_an_acc(result);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_A0_ASHIFT_left(
        uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    return emit_acc_ashift_left_imm(0, imm5);
}
bool LiftVisitor::emit_acc_ashift_arith_imm(int n, uint32_t imm5) {
    // An = An >>> newimmag — 40-bit accumulator arithmetic right shift by immediate
    // b=1 is implicit for arith; field is 6-bit: newimmag = -(0x20|imm5) & 0x3f (range 1..32)
    uint32_t newimmag = static_cast<uint32_t>(-(static_cast<int32_t>(0x20u | imm5))) & 0x3fu;
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);
    auto* acc    = emit_load_acc(n);
    auto* result = builder_.CreateAnd(
        builder_.CreateAShr(acc, builder_.getInt64(newimmag)), mask40, "ashr_result");
    emit_store_acc(n, result);
    size_t av_off = n == 0 ? offsetof(CpuState, av0) : offsetof(CpuState, av1);
    store_cpu_u32(av_off, builder_.getInt32(0));
    emit_flags_az_an_acc(result);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_A0_ASHIFT_arith(
        uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    return emit_acc_ashift_arith_imm(0, imm5);
}
bool LiftVisitor::decode_dsp32shiftimm_A1_ASHIFT_left(
        uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    return emit_acc_ashift_left_imm(1, imm5);
}
bool LiftVisitor::decode_dsp32shiftimm_A1_ASHIFT_arith(
        uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    return emit_acc_ashift_arith_imm(1, imm5);
}
bool LiftVisitor::emit_acc_lshift_right_imm(int n, uint32_t b, uint32_t imm5) {
    // An = An >> shiftdn — 40-bit accumulator logical right shift by immediate
    uint32_t raw     = (b << 5) | imm5;
    uint32_t shiftdn = static_cast<uint32_t>(-static_cast<int32_t>(raw)) & 0x3f;

    auto* acc    = emit_load_acc_unsigned(n);
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFull);

    llvm::Value* shifted;
    if (shiftdn <= 32) {
        shifted = builder_.CreateLShr(acc, builder_.getInt64(shiftdn), "lshr_acc");
    } else {
        uint32_t lamt = 32 - (shiftdn & 0x1f);
        shifted = builder_.CreateShl(acc, builder_.getInt64(lamt), "lshl_acc_wrap");
    }
    shifted = builder_.CreateAnd(shifted, mask40, "acc40_masked");

    emit_store_acc(n, shifted);
    size_t av_off = n == 0 ? offsetof(CpuState, av0) : offsetof(CpuState, av1);
    store_cpu_u32(av_off, builder_.getInt32(0));
    emit_flags_az_an_acc(shifted);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_A0_LSHIFT_right(
        uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    return emit_acc_lshift_right_imm(0, b, imm5);
}
bool LiftVisitor::decode_dsp32shiftimm_A1_LSHIFT_right(
        uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    return emit_acc_lshift_right_imm(1, b, imm5);
}
bool LiftVisitor::emit_acc_rot_imm(int n, uint32_t b, uint32_t imm5) {
    // An = ROT An BY immag — compile-time 41-bit rotate through CC
    uint32_t raw = (b << 5) | imm5;
    int shift = (raw & 0x20) ? (int)(raw | ~0x3fu) : (int)raw;  // imm6 sign-extend

    if (shift == 0) return true;  // no-op: acc and CC unchanged

    if (shift < 0) shift += 41;   // normalize to left-rotate equivalent

    auto* i32ty  = builder_.getInt32Ty();
    auto* mask40 = builder_.getInt64(0xFFFFFFFFFFULL);

    auto* acc  = emit_load_acc_unsigned(n);
    auto* cc64 = builder_.CreateZExt(load_cpu_u32(offsetof(CpuState, cc), "cc"), builder_.getInt64Ty(), "cc64");

    llvm::Value* left_part  = (shift == 40)
        ? builder_.getInt64(0)
        : builder_.CreateShl(acc, builder_.getInt64(shift), "rot_left");
    llvm::Value* right_part = (shift == 1)
        ? builder_.getInt64(0)
        : builder_.CreateLShr(acc, builder_.getInt64(41 - shift), "rot_right");
    auto* cc_in = builder_.CreateShl(cc64, builder_.getInt64(shift - 1), "cc_in");
    auto* new_acc = builder_.CreateAnd(
        builder_.CreateOr(builder_.CreateOr(left_part, right_part), cc_in), mask40, "new_acc40");

    auto* new_cc = builder_.CreateTrunc(
        builder_.CreateAnd(
            builder_.CreateLShr(acc, builder_.getInt64(40 - shift)),
            builder_.getInt64(1)), i32ty, "new_cc");

    emit_store_acc(n, new_acc);
    store_cc(new_cc);
    return true;
}
bool LiftVisitor::decode_dsp32shiftimm_A0_ROT(
        uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    return emit_acc_rot_imm(0, b, imm5);
}
bool LiftVisitor::decode_dsp32shiftimm_A1_ROT(
        uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    return emit_acc_rot_imm(1, b, imm5);
}

// PseudoDbg_Assert
// pseudoDbg_Assert: DBGA(Rx.l/h) / DBGAL(Rx) / DBGAH(Rx)
// Checks (allreg(grp,regtest) >> shift) & 0xffff == expected; halts on mismatch.
static void emit_dbg_assert_ir(llvm::IRBuilder<>& B,
                                llvm::Value* cpu_ptr_,
                                llvm::Value* reg_val, uint32_t expected, uint32_t shift) {
    auto& ctx = B.getContext();
    auto* fn = B.GetInsertBlock()->getParent();
    auto* bb_fail = llvm::BasicBlock::Create(ctx, "dbga_fail", fn);
    auto* bb_ok   = llvm::BasicBlock::Create(ctx, "dbga_ok",   fn);

    auto* shifted = B.CreateLShr(reg_val, B.getInt32(shift), "shifted");
    auto* actual  = B.CreateAnd(shifted, B.getInt32(0xffff), "actual");
    auto* mismatch = B.CreateICmpNE(actual, B.getInt32(expected & 0xffff), "mismatch");
    B.CreateCondBr(mismatch, bb_fail, bb_ok);

    B.SetInsertPoint(bb_fail);
    auto make_ptr = [&](size_t off, llvm::Type* ty) {
        auto* i8ptr = B.CreateBitCast(cpu_ptr_, B.getInt8PtrTy());
        auto* gep   = B.CreateConstGEP1_32(B.getInt8Ty(), i8ptr, (unsigned)off);
        return B.CreateBitCast(gep, ty->getPointerTo());
    };
    B.CreateStore(B.getInt8(1),  make_ptr(offsetof(CpuState, halted),    B.getInt8Ty()));
    B.CreateStore(B.getInt32(1), make_ptr(offsetof(CpuState, exit_code), B.getInt32Ty()));
    B.CreateStore(B.getInt8(1),  make_ptr(offsetof(CpuState, did_jump),  B.getInt8Ty()));
    B.CreateRetVoid();

    B.SetInsertPoint(bb_ok);
}

bool LiftVisitor::decode_pseudoDbg_Assert_lo(uint32_t grp, uint32_t regtest, uint32_t expected) {
    llvm::Value* val;
    int fullreg = (grp << 3) | regtest;
    if (grp == 4 && regtest == 6) {
        val = emit_astat_compose();
    } else {
        val = load_cpu_u32(allreg_offset(grp, regtest), "dbga_reg");
        if (fullreg == 32 || fullreg == 34) { // A0.X or A1.X: sign-extend from 8 bits
            val = builder_.CreateAShr(builder_.CreateShl(val, builder_.getInt32(24)), builder_.getInt32(24), "ax_sext");
        }
    }
    emit_dbg_assert_ir(builder_, cpu_ptr_, val, expected, 0);
    return true;
}
bool LiftVisitor::decode_pseudoDbg_Assert_hi(uint32_t grp, uint32_t regtest, uint32_t expected) {
    llvm::Value* val;
    int fullreg = (grp << 3) | regtest;
    if (grp == 4 && regtest == 6) {
        val = emit_astat_compose();
    } else {
        val = load_cpu_u32(allreg_offset(grp, regtest), "dbga_reg");
        if (fullreg == 32 || fullreg == 34) {
            val = builder_.CreateAShr(builder_.CreateShl(val, builder_.getInt32(24)), builder_.getInt32(24), "ax_sext");
        }
    }
    emit_dbg_assert_ir(builder_, cpu_ptr_, val, expected, 16);
    return true;
}
bool LiftVisitor::decode_pseudoDbg_Assert_low(uint32_t grp, uint32_t regtest, uint32_t expected) {
    llvm::Value* val;
    int fullreg = (grp << 3) | regtest;
    if (grp == 4 && regtest == 6) {
        val = emit_astat_compose();
    } else {
        val = load_cpu_u32(allreg_offset(grp, regtest), "dbga_reg");
        if (fullreg == 32 || fullreg == 34) {
            val = builder_.CreateAShr(builder_.CreateShl(val, builder_.getInt32(24)), builder_.getInt32(24), "ax_sext");
        }
    }
    emit_dbg_assert_ir(builder_, cpu_ptr_, val, expected, 0);
    return true;
}
bool LiftVisitor::decode_pseudoDbg_Assert_high(uint32_t grp, uint32_t regtest, uint32_t expected) {
    llvm::Value* val;
    int fullreg = (grp << 3) | regtest;
    if (grp == 4 && regtest == 6) {
        val = emit_astat_compose();
    } else {
        val = load_cpu_u32(allreg_offset(grp, regtest), "dbga_reg");
        if (fullreg == 32 || fullreg == 34) {
            val = builder_.CreateAShr(builder_.CreateShl(val, builder_.getInt32(24)), builder_.getInt32(24), "ax_sext");
        }
    }
    emit_dbg_assert_ir(builder_, cpu_ptr_, val, expected, 16);
    return true;
}

// Unknown / illegal instructions
//
// Raise the appropriate CEC exception rather than halting the emulator.
// VEC_UNDEF_I (0x21): unrecognized instruction encoding.
// VEC_ILGAL_I (0x22): illegal instruction combination (parallel slot mismatch).
// Both are error exceptions (0x20–0x3F), so cec_exception sets RETX = faulting PC.

bool LiftVisitor::decode_unknown_16(uint16_t insn) {
    // When decode_* returns false (in a parallel slot), in_parallel_ is true.
    uint32_t exc = in_parallel_ ? VEC_ILGAL_I : VEC_UNDEF_I;
    auto* ft = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
    call_extern("cec_exception", ft, {cpu_ptr_, builder_.getInt32(exc)});
    terminated_ = true;
    return true;
}

bool LiftVisitor::decode_unknown_32(uint32_t insn) {
    // Parallel-slot decode failures set in_parallel_; use the more precise code.
    uint32_t exc = in_parallel_ ? VEC_ILGAL_I : VEC_UNDEF_I;
    auto* ft = FunctionType::get(builder_.getVoidTy(),
        {builder_.getInt8PtrTy(), builder_.getInt32Ty()}, false);
    call_extern("cec_exception", ft, {cpu_ptr_, builder_.getInt32(exc)});
    terminated_ = true;
    return true;
}
