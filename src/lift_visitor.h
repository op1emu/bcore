#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "cpu_state.h"
#include "mem.h"

class LiftVisitor {
public:
    using return_type = bool;

    LiftVisitor(llvm::IRBuilder<>& builder, llvm::Value* cpu_ptr, llvm::Value* mem_ptr,
                llvm::Module* module, Memory* mem, bool unlimited = true,
                bool fastmem = false, uint64_t fast_base = 0,
                uint32_t rawmem_limit = 0);

    // State
    uint32_t current_pc = 0;

    void set_pc(uint32_t pc) { current_pc = pc; }
    uint16_t iftech(uint32_t addr);

    // Parallel instruction lifecycle hooks
    void on_parallel_begin()     { in_parallel_ = true; parallel_slot_ = 0; dis_algn_expt_ = false; shadow_writes_.clear(); }
    void on_parallel_next_slot() { ++parallel_slot_; }
    void on_parallel_end();
    void on_parallel_abort()     { shadow_writes_.clear(); in_parallel_ = false; parallel_slot_ = 0; }

    // Whether the current BB has been terminated (branch/jump)
    bool is_terminated() const { return terminated_; }

    // The PC that should be used for fallthrough (set during translation)
    uint32_t fallthrough_pc() const { return fallthrough_pc_; }
    void set_fallthrough_pc(uint32_t pc) { fallthrough_pc_ = pc; }

    // Reset terminated_ so a linear scan can continue past branch instructions
    void reset_terminated()  { terminated_ = false; }

    void emit_insn_len(uint32_t len);

    // Emit per-instruction hwloop probe: PC redirect (when !did_jump) and counter decrement.
    // Mirrors the reference semantics that run after every instruction:
    //   if (!did_jump) SetPC(hwloop_get_next_pc(insn_pc, insn_len));
    //   for i=1..0: if (lc[i] && insn_pc==lb[i]) { lc[i]--; if(lc[i]) break; }
    void emit_epilog(uint32_t insn_pc, uint32_t insn_len);

    // Pending exit blocks from conditional branches: taken-paths that had
    // emit_jump_imm called but no RetVoid yet, so they still need the epilog
    // (counter decrement) before being terminated.
    struct PendingExit { llvm::BasicBlock* block; };

    // Emit epilog + RetVoid on all pending exit blocks, then clear the list.
    // Must be called by BBTranslator right after emit_epilog().
    void finalize_pending_exits(uint32_t insn_pc, uint32_t insn_len);

    // ProgCtrl instructions
    bool decode_ProgCtrl_NOP();
    bool decode_ProgCtrl_RTS();
    bool decode_ProgCtrl_RTI();
    bool decode_ProgCtrl_RTX();
    bool decode_ProgCtrl_RTN();
    bool decode_ProgCtrl_RTE();
    bool decode_ProgCtrl_IDLE();
    bool decode_ProgCtrl_CSYNC();
    bool decode_ProgCtrl_SSYNC();
    bool decode_ProgCtrl_EMUEXCPT();
    bool decode_ProgCtrl_CLI(uint16_t d);
    bool decode_ProgCtrl_STI(uint16_t d);
    bool decode_ProgCtrl_JUMP_PREG(uint16_t p);
    bool decode_ProgCtrl_CALL_PREG(uint16_t p);
    bool decode_ProgCtrl_CALL_PC_PREG(uint16_t p);
    bool decode_ProgCtrl_JUMP_PC_PREG(uint16_t p);
    bool decode_ProgCtrl_RAISE(uint16_t imm);
    bool decode_ProgCtrl_EXCPT(uint16_t imm);
    bool decode_ProgCtrl_TESTSET(uint16_t p);

    // PushPopReg instructions
    bool decode_PopReg(uint16_t g, uint16_t r);
    bool decode_PushReg(uint16_t g, uint16_t r);

    // PushPopMultiple instructions
    bool decode_PopMultiple_RP(uint16_t ddd, uint16_t ppp);
    bool decode_PopMultiple_R(uint16_t ddd);
    bool decode_PopMultiple_P(uint16_t ppp);
    bool decode_PushMultiple_RP(uint16_t ddd, uint16_t ppp);
    bool decode_PushMultiple_R(uint16_t ddd);
    bool decode_PushMultiple_P(uint16_t ppp);

    // CC2dreg instructions
    bool decode_CC2dreg_Read(uint16_t d);
    bool decode_CC2dreg_Write(uint16_t d);
    bool decode_CC2dreg_Negate();

    // CaCTRL instructions
    bool decode_CaCTRL_PREFETCH(uint16_t p);
    bool decode_CaCTRL_FLUSHINV(uint16_t p);
    bool decode_CaCTRL_FLUSH(uint16_t p);
    bool decode_CaCTRL_IFLUSH(uint16_t p);
    bool decode_CaCTRL_PREFETCH_pp(uint16_t p);
    bool decode_CaCTRL_FLUSHINV_pp(uint16_t p);
    bool decode_CaCTRL_FLUSH_pp(uint16_t p);
    bool decode_CaCTRL_IFLUSH_pp(uint16_t p);

    // CC2stat instructions
    bool decode_CC2stat_CC_EQ_ASTAT(uint16_t c);
    bool decode_CC2stat_CC_OR_ASTAT(uint16_t c);
    bool decode_CC2stat_CC_AND_ASTAT(uint16_t c);
    bool decode_CC2stat_CC_XOR_ASTAT(uint16_t c);
    bool decode_CC2stat_ASTAT_EQ_CC(uint16_t c);
    bool decode_CC2stat_ASTAT_OR_CC(uint16_t c);
    bool decode_CC2stat_ASTAT_AND_CC(uint16_t c);
    bool decode_CC2stat_ASTAT_XOR_CC(uint16_t c);

    // ccMV instructions
    bool decode_ccMV_IF_NOT(uint16_t d, uint16_t s, uint16_t dst, uint16_t src);
    bool decode_ccMV_IF(uint16_t d, uint16_t s, uint16_t dst, uint16_t src);

    // CCflag instructions
    bool decode_CCflag_EQ(bool preg, uint16_t y, uint16_t x);
    bool decode_CCflag_LT(bool preg, uint16_t y, uint16_t x);
    bool decode_CCflag_LE(bool preg, uint16_t y, uint16_t x);
    bool decode_CCflag_LT_U(bool preg, uint16_t y, uint16_t x);
    bool decode_CCflag_LE_U(bool preg, uint16_t y, uint16_t x);
    bool decode_CCflag_A0_EQ_A1();
    bool decode_CCflag_A0_LT_A1();
    bool decode_CCflag_A0_LE_A1();
    bool decode_CCflag_EQ_imm(bool preg, uint16_t i, uint16_t x);
    bool decode_CCflag_LT_imm(bool preg, uint16_t i, uint16_t x);
    bool decode_CCflag_LE_imm(bool preg, uint16_t i, uint16_t x);
    bool decode_CCflag_LT_U_imm(bool preg, uint16_t i, uint16_t x);
    bool decode_CCflag_LE_U_imm(bool preg, uint16_t i, uint16_t x);

    // BRCC instructions
    bool decode_BRCC_BRT(uint16_t offset);
    bool decode_BRCC_BRT_BP(uint16_t offset);
    bool decode_BRCC_BRF(uint16_t offset);
    bool decode_BRCC_BRF_BP(uint16_t offset);

    // UJUMP instructions
    bool decode_UJUMP(uint16_t offset);

    // REGMV instructions
    bool decode_REGMV(uint16_t gd, uint16_t gs, uint16_t d, uint16_t s);

    // ALU2op instructions
    bool decode_ALU2op_ASHIFT_RIGHT(uint16_t src, uint16_t dst);
    bool decode_ALU2op_LSHIFT_RIGHT(uint16_t src, uint16_t dst);
    bool decode_ALU2op_LSHIFT_LEFT(uint16_t src, uint16_t dst);
    bool decode_ALU2op_MUL(uint16_t src, uint16_t dst);
    bool decode_ALU2op_ADD_SHIFT1(uint16_t src, uint16_t dst);
    bool decode_ALU2op_ADD_SHIFT2(uint16_t src, uint16_t dst);
    bool decode_ALU2op_DIVQ(uint16_t src, uint16_t dst);
    bool decode_ALU2op_DIVS(uint16_t src, uint16_t dst);
    bool decode_ALU2op_SEXT_L(uint16_t src, uint16_t dst);
    bool decode_ALU2op_ZEXT_L(uint16_t src, uint16_t dst);
    bool decode_ALU2op_SEXT_B(uint16_t src, uint16_t dst);
    bool decode_ALU2op_ZEXT_B(uint16_t src, uint16_t dst);
    bool decode_ALU2op_NEG(uint16_t src, uint16_t dst);
    bool decode_ALU2op_NOT(uint16_t src, uint16_t dst);

    // PTR2op instructions
    bool decode_PTR2op_SUB(uint16_t src, uint16_t dst);
    bool decode_PTR2op_LSHIFT_LEFT2(uint16_t src, uint16_t dst);
    bool decode_PTR2op_LSHIFT_RIGHT2(uint16_t src, uint16_t dst);
    bool decode_PTR2op_LSHIFT_RIGHT1(uint16_t src, uint16_t dst);
    bool decode_PTR2op_ADD_BREV(uint16_t src, uint16_t dst);
    bool decode_PTR2op_ADD_SHIFT1(uint16_t src, uint16_t dst);
    bool decode_PTR2op_ADD_SHIFT2(uint16_t src, uint16_t dst);

    // LOGI2op instructions
    bool decode_LOGI2op_CC_EQ_AND(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_CC_EQ_BITTST(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_BITSET(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_BITTGL(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_BITCLR(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_ASHIFT_RIGHT(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_LSHIFT_RIGHT(uint16_t imm, uint16_t d);
    bool decode_LOGI2op_LSHIFT_LEFT(uint16_t imm, uint16_t d);

    // COMP3op instructions
    bool decode_COMP3op_ADD(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_SUB(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_AND(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_OR(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_XOR(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_PADD(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_LSHIFT(uint16_t dst, uint16_t t, uint16_t s);
    bool decode_COMP3op_LSHIFT2(uint16_t dst, uint16_t t, uint16_t s);

    // COMPI2opD instructions
    bool decode_COMPI2opD_EQ(uint16_t imm, uint16_t d);
    bool decode_COMPI2opD_ADD(uint16_t imm, uint16_t d);

    // COMPI2opP instructions
    bool decode_COMPI2opP_EQ(uint16_t imm, uint16_t d);
    bool decode_COMPI2opP_ADD(uint16_t imm, uint16_t d);

    // LDSTpmod instructions
    bool decode_LDSTpmod_LD_32(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_LD_16_lo(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_LD_16_hi(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_LD_16_Z(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_LD_16_X(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_ST_32(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_ST_16_lo(uint16_t d, uint16_t idx, uint16_t ptr);
    bool decode_LDSTpmod_ST_16_hi(uint16_t d, uint16_t idx, uint16_t ptr);

    // LDST instructions
    bool decode_LDST_LD_32(uint16_t p, uint16_t d);
    bool decode_LDST_LD_32_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_32_ind(uint16_t p, uint16_t d);
    bool decode_LDST_ST_32(uint16_t p, uint16_t d);
    bool decode_LDST_ST_32_mm(uint16_t p, uint16_t d);
    bool decode_LDST_ST_32_ind(uint16_t p, uint16_t d);
    bool decode_LDST_LD_16_Z(uint16_t p, uint16_t d);
    bool decode_LDST_LD_16_Z_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_16_Z_ind(uint16_t p, uint16_t d);
    bool decode_LDST_ST_16(uint16_t p, uint16_t d);
    bool decode_LDST_ST_16_mm(uint16_t p, uint16_t d);
    bool decode_LDST_ST_16_ind(uint16_t p, uint16_t d);
    bool decode_LDST_LD_16_X(uint16_t p, uint16_t d);
    bool decode_LDST_LD_16_X_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_16_X_ind(uint16_t p, uint16_t d);
    bool decode_LDST_LD_8_Z(uint16_t p, uint16_t d);
    bool decode_LDST_LD_8_Z_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_8_Z_ind(uint16_t p, uint16_t d);
    bool decode_LDST_ST_8(uint16_t p, uint16_t d);
    bool decode_LDST_ST_8_mm(uint16_t p, uint16_t d);
    bool decode_LDST_ST_8_ind(uint16_t p, uint16_t d);
    bool decode_LDST_LD_8_X(uint16_t p, uint16_t d);
    bool decode_LDST_LD_8_X_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_8_X_ind(uint16_t p, uint16_t d);
    bool decode_LDST_LD_P_32(uint16_t p, uint16_t d);
    bool decode_LDST_LD_P_32_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_P_32_ind(uint16_t p, uint16_t d);
    bool decode_LDST_ST_P_32(uint16_t p, uint16_t d);
    bool decode_LDST_ST_P_32_mm(uint16_t p, uint16_t d);
    bool decode_LDST_ST_P_32_ind(uint16_t p, uint16_t d);
    bool decode_LDST_LD_P_32_z(uint16_t p, uint16_t d);
    bool decode_LDST_LD_P_32_z_mm(uint16_t p, uint16_t d);
    bool decode_LDST_LD_P_32_z_ind(uint16_t p, uint16_t d);
    bool decode_LDST_ST_P_32_z(uint16_t p, uint16_t d);
    bool decode_LDST_ST_P_32_z_mm(uint16_t p, uint16_t d);
    bool decode_LDST_ST_P_32_z_ind(uint16_t p, uint16_t d);

    // dspLDST instructions
    bool decode_dspLDST_LD_dreg(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_lo(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_hi(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_mm(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_mm_lo(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_mm_hi(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_Mmod(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_Mmod_lo(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_Mmod_hi(uint16_t i, uint16_t d);
    bool decode_dspLDST_LD_dreg_brev(uint16_t m, uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_lo(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_hi(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_mm(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_mm_lo(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_mm_hi(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_Mmod(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_Mmod_lo(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_Mmod_hi(uint16_t i, uint16_t d);
    bool decode_dspLDST_ST_dreg_brev(uint16_t m, uint16_t i, uint16_t d);

    // LDSTii instructions
    bool decode_LDSTii_LD_32(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_LD_16_Z(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_LD_16_X(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_LD_P_32(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_ST_32(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_ST_16(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_LD_8_Z(uint16_t offset, uint16_t p, uint16_t d);
    bool decode_LDSTii_ST_P_32(uint16_t offset, uint16_t p, uint16_t d);

    // LDSTiiFP instructions
    bool decode_LDSTiiFP_LD_32(uint16_t offset, uint16_t reg);
    bool decode_LDSTiiFP_ST_32(uint16_t offset, uint16_t reg);

    // dagMODim instructions
    bool decode_dagMODim_ADD(uint16_t m, uint16_t i);
    bool decode_dagMODim_SUB(uint16_t m, uint16_t i);
    bool decode_dagMODim_ADD_BREV(uint16_t m, uint16_t i);

    // dagMODik instructions
    bool decode_dagMODik_ADD2(uint16_t i);
    bool decode_dagMODik_SUB2(uint16_t i);
    bool decode_dagMODik_ADD4(uint16_t i);
    bool decode_dagMODik_SUB4(uint16_t i);

    // pseudoDEBUG instructions
    bool decode_pseudoDEBUG_DBG_reg(uint16_t g, uint16_t r);
    bool decode_pseudoDEBUG_PRNT_reg(uint16_t g, uint16_t r);
    bool decode_pseudoDEBUG_OUTC_dreg(uint16_t r);
    bool decode_pseudoDEBUG_DBG_A0(uint16_t g);
    bool decode_pseudoDEBUG_DBG_A1(uint16_t g);
    bool decode_pseudoDEBUG_ABORT(uint16_t g);
    bool decode_pseudoDEBUG_HLT(uint16_t g);
    bool decode_pseudoDEBUG_DBGHALT(uint16_t g);
    bool decode_pseudoDEBUG_DBGCMPLX(uint16_t g);
    bool decode_pseudoDEBUG_DBG(uint16_t g);

    // pseudoChr instructions
    bool decode_pseudoChr_OUTC(uint16_t ch);

    // 32-bit instructions

    // LoopSetup instructions
    bool decode_LoopSetup_LC0(uint32_t soffset, uint32_t reg, uint32_t eoffset);
    bool decode_LoopSetup_LC0_P(uint32_t soffset, uint32_t reg, uint32_t eoffset);
    bool decode_LoopSetup_LC1(uint32_t soffset, uint32_t reg, uint32_t eoffset);
    bool decode_LoopSetup_LC1_P(uint32_t soffset, uint32_t reg, uint32_t eoffset);
    bool decode_LoopSetup_LC0_half(uint32_t soffset, uint32_t reg, uint32_t eoffset);
    bool decode_LoopSetup_LC1_half(uint32_t soffset, uint32_t reg, uint32_t eoffset);

    // LDIMMhalf instructions
    bool decode_LDIMMhalf_low(uint32_t g, uint32_t r, uint16_t hword);
    bool decode_LDIMMhalf_high(uint32_t g, uint32_t r, uint16_t hword);
    bool decode_LDIMMhalf_full(uint32_t g, uint32_t r, uint16_t hword);
    bool decode_LDIMMhalf_full_sext(uint32_t g, uint32_t r, uint16_t hword);

    // CALLa instructions
    bool decode_CALLa_CALL(uint32_t addr);
    bool decode_CALLa_JUMP(uint32_t addr);

    // LDSTidxI instructions
    bool decode_LDSTidxI_LD_32(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_LD_16_Z(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_LD_16_X(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_LD_B_Z(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_LD_B_X(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_ST_32(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_ST_16(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_ST_B(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_LD_P_32(uint32_t p, uint32_t r, uint32_t offset);
    bool decode_LDSTidxI_ST_P_32(uint32_t p, uint32_t r, uint32_t offset);

    // Linkage instructions
    bool decode_Linkage_LINK(uint32_t framesize);
    bool decode_Linkage_UNLINK();

    // DSP32 mac
    bool decode_dsp32mac_MNOP(uint32_t M);
    bool decode_dsp32mac_P0_nn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_nn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);

    // DSP32 mult
    bool decode_dsp32mult_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);

    // dsp32alu
    bool decode_dsp32alu_ADDADD(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADDSUB(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUBADD(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUBSUB(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_QUADADD_HL0(uint32_t M, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_QUADADD_HL1(uint32_t M, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd0_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd0_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd0_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd0_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd1_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd1_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd1_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD16_HLd1_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd0_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd0_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd0_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd0_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd1_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd1_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd1_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB16_HLd1_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD32(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB32(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADDSUB32_dual(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD_RND12_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB_RND12_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD_RND20_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB_RND20_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD_RND12_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB_RND12_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADD_RND20_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUB_RND20_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_VMAX(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_VMIN(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_VABS(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_MAX(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_MIN(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ABS(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_NEG_NS(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_NEG_S(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A0_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A0_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1A0_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1A0_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_COPY_A0_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_COPY_A1_A0(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP0_full(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP0_lo(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP0_hi(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP2_full(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP2_lo(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP2_hi(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_LOAD_AOP3(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0X_READ(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A1X_READ(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_PLUS_A1(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_PLUS_A1_HL_lo(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_PLUS_A1_HL_hi(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_INC_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_INC_A1_W32(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_DEC_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_DEC_A1_W32(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SIGN_MULT(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_ACCUM_SUM(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_RND_HL_lo(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_RND_HL_hi(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SEARCH(uint32_t M, uint32_t HL, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_NEG_HL0_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_NEG_HL0_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_NEG_HL1_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_NEG_HL1_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_NEG_BOTH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_NEG_V(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_ABS_HL0_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_ABS_HL0_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_ABS_HL1_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_ABS_HL1_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A_ABS_BOTH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A1pA0_A1mA0(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0pA1_A0mA1(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SAA(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_DISALGNEXCPT(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP1P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP1P_T(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP1P_T_R(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP16P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP16M(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_RNDL(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_RNDL_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_RNDH(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_RNDH_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_TL(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_TL_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_TH(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP2P_TH_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP3P_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP3P_LO_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP3P_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP3P_HI_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEPACK(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEUNPACK(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);

    // dsp32shift
    bool decode_dsp32shift_ASHIFT16(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ASHIFT16S(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_LSHIFT16(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VASHIFT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VASHIFTS(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VLSHIFT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ASHIFT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ASHIFT32S(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_LSHIFT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ROT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ACC_ASHIFT_A0(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ACC_ASHIFT_A1(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ACC_LSHIFT_A0(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ACC_LSHIFT_A1(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ACC_ROT_A0(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ACC_ROT_A1(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ROT32_dreg(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_PACK_LL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_PACK_LH(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_PACK_HL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_PACK_HH(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_SIGNBITS_32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_SIGNBITS_16L(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_SIGNBITS_16H(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_SIGNBITS_A0(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_SIGNBITS_A1(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ONES(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_EXPADJ_32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_EXPADJ_V(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_EXPADJ_16L(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_EXPADJ_16H(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_BITMUX_ASR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_BITMUX_ASL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VITMAX_ASL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VITMAX_ASR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VITMAX2_ASL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_VITMAX2_ASR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_EXTRACT_Z(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_EXTRACT_X(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_DEPOSIT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_DEPOSIT_X(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_BXORSHIFT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_BXOR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_BXORSHIFT3(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_BXOR3(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ALIGN8(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ALIGN16(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32shift_ALIGN24(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1);

    // dsp32shiftimm
    bool decode_dsp32shiftimm_ASHIFT16_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_ASHIFT16S_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_ASHIFT16S_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_LSHIFT16_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_LSHIFT16_right(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_VASHIFT_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_VASHIFTS_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_VASHIFTS_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_VLSHIFT_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_VLSHIFT_right(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_ASHIFT32_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_ASHIFT32S_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_LSHIFT32_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_LSHIFT32_right(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_ROT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A0_ASHIFT_left(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A0_ASHIFT_arith(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A1_ASHIFT_left(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A1_ASHIFT_arith(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A0_LSHIFT_right(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A1_LSHIFT_right(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A0_ROT(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);
    bool decode_dsp32shiftimm_A1_ROT(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1);

    // PseudoDbg_Assert instructions
    bool decode_pseudoDbg_Assert_lo(uint32_t grp, uint32_t regtest, uint32_t expected);
    bool decode_pseudoDbg_Assert_hi(uint32_t grp, uint32_t regtest, uint32_t expected);
    bool decode_pseudoDbg_Assert_low(uint32_t grp, uint32_t regtest, uint32_t expected);
    bool decode_pseudoDbg_Assert_high(uint32_t grp, uint32_t regtest, uint32_t expected);

    // Fallback for instructions not matched by any pattern
    bool decode_unknown_16(uint16_t insn);
    bool decode_unknown_32(uint32_t insn);

private:
    llvm::IRBuilder<>& builder_;
    llvm::Value* cpu_ptr_;
    llvm::Value* mem_ptr_;
    llvm::Module* module_;
    Memory* mem_ = nullptr;
    bool terminated_ = false;
    bool unlimited_mode_ = true;
    bool in_parallel_ = false;
    int  parallel_slot_ = 0;      // 0=slot0/non-parallel, 1=slot1, 2=slot2
    bool dis_algn_expt_ = false;  // DISALGNEXCPT fired in current parallel group
    bool fastmem_ = false;
    bool check_did_jump_ = false;  // whether to check did_jump flag at end of block and exit if set
    bool check_cec_pending_ = false;  // whether to check cec_pending flag at end of block and exit if set
    uint64_t fast_base_value_ = 0;
    uint32_t rawmem_limit_ = 0;   // non-zero: inline mem reads for addr < rawmem_limit_
    std::map<size_t, llvm::Value*> shadow_writes_;
    uint32_t fallthrough_pc_ = 0;
    std::vector<PendingExit> pending_exits_;

    // Helper to get a GEP pointer to a field in CpuState
    llvm::Value* cpu_field_ptr(size_t offset, llvm::Type* ty, const llvm::Twine& name = "");
    // Load a uint32_t field from CpuState
    llvm::Value* load_cpu_u32(size_t offset, const llvm::Twine& name = "");
    // Store a uint32_t value to CpuState
    void store_cpu_u32(size_t offset, llvm::Value* val);
    // Load dpregs[idx] (idx can be reg number 0-15)
    llvm::Value* load_dreg(uint32_t idx, const llvm::Twine& name = "");
    llvm::Value* load_preg(uint32_t idx, const llvm::Twine& name = "");

    void store_dreg(uint32_t idx, llvm::Value* val);
    void store_preg(uint32_t idx, llvm::Value* val);

    // allreg access (grp, reg) -> (offset, is_astat)
    size_t allreg_offset(uint32_t grp, uint32_t reg) const;

    // Set CC and auto-copy ac0->ac0_copy, v->v_copy
    void store_cc(llvm::Value* val);
    void store_ac0(llvm::Value* val);
    void store_v(llvm::Value* val);

    // Emit a runtime check: if cpu->did_jump is set (exception was raised),
    // exit the BB immediately. Used after calls that may raise exceptions
    // (cec_push_reti, cec_check_sup) in the middle of a non-terminator instruction.
    void emit_did_jump_exit(bool force);

    // Emit per-instruction step counter check; repositions builder_ to continue block.
    // No-op when terminated_=true. Called by BBTranslator after each instruction.
    void emit_step_check(uint32_t post_insn_pc);

    // Flag helpers — emit AZ/AN from a 32-bit result value
    void emit_flags_az_an(llvm::Value* result);
    // AZ/AN + clear AC0=0, V=0 (logical ops: AND, OR, XOR, NOT, SEXT, ZEXT)
    void emit_flags_logic(llvm::Value* result);
    // AZ/AN/AC0/V/VS for arithmetic ops (V sticky-ORed into VS)
    void emit_flags_arith(llvm::Value* result, llvm::Value* v_flag, llvm::Value* ac0);

    // Shared signed-subtract comparison kernel used by all CCflag EQ/LT/LE variants.
    // Returns AZ, AN, and AC0 values for the subtraction (src - dst).
    struct CmpFlags { llvm::Value* az; llvm::Value* an; llvm::Value* ac0; };
    CmpFlags emit_cc_cmp(llvm::Value* src, llvm::Value* dst);

    // Memory access helpers — build FunctionType + call_extern in one call
    llvm::Value* emit_mem_read(const char* fn, llvm::Type* ret_ty, llvm::Value* addr);
    void emit_mem_write(const char* fn, llvm::Value* addr, llvm::Value* val);

    // Inline IR equivalents of astat_compose / astat_decompose (no extern call)
    llvm::Value* emit_astat_compose();
    void emit_astat_decompose(llvm::Value* astat);

    // Set did_jump=true and pc=target
    void emit_jump(llvm::Value* target);
    void emit_jump_imm(uint32_t target);

    // Compute hwloop-adjusted return address for CALL instructions:
    // if current_pc == lb[i] and lc[i] > 1, return lt[i]; else return current_pc + insn_len.
    llvm::Value* emit_hwloop_next_pc(uint32_t insn_len);

    // Emit call to external function
    llvm::Value* call_extern(const char* name, llvm::FunctionType* ft,
                             llvm::ArrayRef<llvm::Value*> args);

    // Mark instruction as unimplemented
    bool unimplemented(const char* name);

    // Build a sign-extended 40-bit accumulator as an i64 from (ax, aw) fields
    llvm::Value* build_acc_i64(llvm::Value* ax, llvm::Value* aw);
    // Convenience: load accumulator N (ax[n]+aw[n]) as a sign-extended 40-bit i64
    llvm::Value* emit_load_acc(int n);
    llvm::Value* emit_signbits_acc(llvm::Value* ax, llvm::Value* aw);

    // Emit ABS of accumulator src_acc, store result to dst_acc.
    // Returns av flag as i32 (1 if overflow/saturation occurred).
    llvm::Value* emit_acc_abs(int src_acc, int dst_acc);

    // --- dsp32shift / dsp32shiftimm helpers ---

    // Extract 6-bit signed shift from Rt.L: (bs8)(Rt << 2) >> 2.  Returns i32.
    llvm::Value* emit_extract_shift6(uint32_t src0_dreg);

    // Load accumulator n as unsigned (zero-extended) 40-bit i64.
    llvm::Value* emit_load_acc_unsigned(int n);

    // Decompose i64 → aw[n]/ax[n] and store both.
    void emit_store_acc(int n, llvm::Value* acc64);

    // AZ/AN for 16-bit results (AN tests bit 15).  Input is i32 with low 16 bits valid.
    void emit_flags_az_an_16(llvm::Value* result16);

    // AZ/AN for vector 2×16-bit results: AZ = either half zero, AN = either half negative.
    void emit_flags_az_an_v2x16(llvm::Value* val0, llvm::Value* val1);

    // AZ/AN for 40-bit accumulator results (AN tests bit 39).  Input is i64.
    void emit_flags_az_an_acc(llvm::Value* result64);

    // store_v(v_flag) + sticky VS update.
    void emit_v_vs_update(llvm::Value* v_flag);

    // 16-bit overflow detection for arithmetic left shifts.  Returns i1.
    llvm::Value* emit_shift16_overflow(llvm::Value* left_shifted, llvm::Value* left_result,
                                        llvm::Value* shft, llvm::Value* in_sign);

    // lshift v_i for a 16-bit immediate shift: NOT(bits_lost==0 OR (bits_lost==all_ones AND result_neg)).
    // bits_lost = shifted_full_i32 >> 16; result16 = low 16 bits of shifted value; all_ones_bits = (1<<cnt)-1.
    // Returns i1.
    llvm::Value* emit_lshift16_vi_imm(llvm::Value* bits_lost, llvm::Value* result16,
                                       uint32_t all_ones_bits);

    // Shared wrap block for vector arithmetic right shift when newimmag > 16.
    // lamt = 16 - (newimmag & 0xF).  Returns {out0, out1, v_flag_i32}.
    std::tuple<llvm::Value*, llvm::Value*, llvm::Value*>
    emit_vashift_wrap(llvm::Value* val0, llvm::Value* val1, uint32_t lamt);

    // Extract unsigned 16-bit half based on (HLs & 1).  Returns i32 with low 16 bits.
    llvm::Value* emit_extract_half16(llvm::Value* rs, uint32_t HLs);

    // Write result16 into high or low half of dst dreg based on (HLs & 2).
    void emit_merge_half16(uint32_t dst, llvm::Value* result16, uint32_t HLs);

    // XOR-fold parity of (a & b).  Inputs are i64, returns i32 (0 or 1).
    llvm::Value* emit_xor_reduce_parity(llvm::Value* a, llvm::Value* b);

    // Shared kernels for collapsing A0/A1 dsp32shift pairs
    bool emit_acc_ashift(int n, uint32_t src0);
    bool emit_acc_lshift(int n, uint32_t src0);
    bool emit_acc_rot(int n, uint32_t src0);

    // Shared kernels for collapsing A0/A1 dsp32shiftimm pairs
    bool emit_acc_ashift_left_imm(int n, uint32_t imm5);
    bool emit_acc_ashift_arith_imm(int n, uint32_t imm5);
    bool emit_acc_lshift_right_imm(int n, uint32_t b, uint32_t imm5);
    bool emit_acc_rot_imm(int n, uint32_t b, uint32_t imm5);

    // DAG post-modify: update ireg[dagno] by +delta or -delta with circular buffer.
    // Carry-bit accurate dagadd/dagsub matching reference bfin-sim.c.
    // m_val is i32, treated as signed (M register or constant).
    void emit_dagmod_add(uint16_t dagno, llvm::Value* m_val);
    void emit_dagmod_sub(uint16_t dagno, llvm::Value* m_val);
    // Convenience wrappers:
    void emit_dagadd(uint16_t dagno, uint32_t delta);
    void emit_dagsub(uint16_t dagno, uint32_t delta);
    void emit_dagadd_mreg(uint16_t dagno, uint16_t mreg_idx);
    void emit_dagsub_mreg(uint16_t dagno, uint16_t mreg_idx);

    // BYTEOP alignment rotation: aln=0 → l; else (l >> 8*aln) | (h << (32 - 8*aln))
    llvm::Value* emit_byte_align(llvm::Value* l, llvm::Value* h, llvm::Value* aln);
    // BYTEOP source loading: load dreg pair at src/src+1, read ireg[ireg_idx] & 3,
    // call emit_byte_align. reversed swaps L/H order.
    llvm::Value* emit_byteop_load_align(uint32_t src, uint32_t ireg_idx,
                                        bool reversed, const llvm::Twine& name = "");
    // Variant that takes a pre-computed alignment value (i_aln = ireg & 3).
    llvm::Value* emit_byteop_load_align_v(uint32_t src, llvm::Value* i_aln,
                                          bool reversed, const llvm::Twine& name = "");
    // Extract unsigned byte lane k from 32-bit value.
    llvm::Value* emit_byteop_extract_ub(llvm::Value* v, unsigned k);
    // Pack two byte/halfword values into low or high 16-bit slot of a 32-bit word.
    llvm::Value* emit_byteop_pack2(llvm::Value* b0, llvm::Value* b1, bool hi_slot);

    bool emit_quadadd(uint32_t HL, uint32_t aop, uint32_t s, uint32_t x,
                      uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);

    // DSP saturation / rounding helpers (used by MAC extraction)
    llvm::Value* emit_rnd16(llvm::Value* v64);
    llvm::Value* emit_trunc16(llvm::Value* v64);
    llvm::Value* emit_sat_s16(llvm::Value* v64, llvm::Value** ov_out = nullptr);
    llvm::Value* emit_sat_u16(llvm::Value* v64, llvm::Value** ov_out = nullptr);
    llvm::Value* emit_sat_s32(llvm::Value* v64, llvm::Value** ov_out = nullptr);
    llvm::Value* emit_sat_u32(llvm::Value* v64, llvm::Value** ov_out = nullptr);

    // MAC helpers
    llvm::Value* emit_saturate_acc(
        llvm::Value*& new_acc, llvm::Value* tsat_flag, llvm::Value* sgn40,
        uint32_t mmod, uint32_t MM, uint32_t which, bool update_acc);
    llvm::Value* emit_mac_common(uint32_t which, uint32_t op,
        uint32_t h0, uint32_t h1, uint32_t src0reg, uint32_t src1reg,
        uint32_t mmod, uint32_t MM, bool fullword,
        llvm::Value** v_out = nullptr, bool update_acc = true);
    bool emit_dsp32mac(uint32_t M, uint32_t mmod, uint32_t MM,
        uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10,
        uint32_t dst, uint32_t src0, uint32_t src1,
        bool w0, bool w1, bool P, bool is_mult = false);
    void store_dreg_lo(uint32_t dst, llvm::Value* val16);
    void store_dreg_hi(uint32_t dst, llvm::Value* val16);

    // Shared kernel for all dsp32alu ADD16/SUB16 variants
    bool emit_add_sub16(bool is_sub, bool dst_hi, bool src0_hi, bool src1_hi,
                        uint32_t s, uint32_t dst0, uint32_t src0, uint32_t src1);

    // Shared kernels for dsp32alu packed-16 dual add/sub (ADDADD/ADDSUB/SUBADD/SUBSUB)
    bool emit_addsubpair(bool hi_is_sub, bool lo_is_sub,
                         uint32_t s, uint32_t x,
                         uint32_t dst0, uint32_t src0, uint32_t src1);

    // Shared kernels for dsp32alu RND12 and RND20 variants
    bool emit_rnd12(bool is_sub, bool dst_hi,
                    uint32_t dst0, uint32_t src0, uint32_t src1);
    bool emit_rnd20(bool is_sub, bool dst_hi,
                    uint32_t dst0, uint32_t src0, uint32_t src1);

    // Shared kernel for 32-bit ADD/SUB (ADD32/SUB32)
    bool emit_addsub32(bool is_sub, uint32_t s,
                       uint32_t dst0, uint32_t src0, uint32_t src1);

    // Shared kernel for NEG_NS/NEG_S
    bool emit_neg32(bool saturate,
                    uint32_t dst0, uint32_t src0, uint32_t src1);

    // ---- Extracted helpers for DSP32 simplification ----

    // Sign-extend lo or hi 16-bit half of a 32-bit value to i32.
    llvm::Value* emit_sext_half16(llvm::Value* r, bool hi);

    // AZ = (lo==0 || hi==0), AN = (lo[15] || hi[15]), store to ASTAT.
    void emit_flags_nz_2x16(llvm::Value* lo_half, llvm::Value* hi_half);

    // Clamp value to [0, 255].
    llvm::Value* emit_clamp_u8(llvm::Value* v);

    // LLVM intrinsic wrappers
    llvm::Value* emit_smin(llvm::Value* a, llvm::Value* b);
    llvm::Value* emit_smax(llvm::Value* a, llvm::Value* b);
    llvm::Value* emit_umin(llvm::Value* a, llvm::Value* b);
    llvm::Value* emit_abs(llvm::Value* v);   // llvm.abs with is_int_min_poison=false
    llvm::Value* emit_fshr(llvm::Value* hi, llvm::Value* lo, llvm::Value* amt);

    // An_dst = -An_src, saturate_s40, update az/an/ac{dst}/av{dst}/avs{dst}.
    void emit_acc_neg(int src, int dst);

    // Saturate An to s32 range, sign-extend bit31, update av{n}/avs{n}/az/an.
    void emit_acc_sat(int n);

    // After emit_acc_abs(src,dst): reload dst acc, compute az/an=0, store av/avs.
    void emit_acc_abs_flags(int dst_acc, llvm::Value* av);

    // A0 += A1 or A0 -= A1, saturate_s40, optional W32 masking, update flags.
    void emit_acc_inc_dec(bool is_sub, bool w32);

    // A0 += A1, then Dreg.H or .L = saturate_s16(rnd16(A0)).
    void emit_a0_plus_a1_hl(bool hi, uint32_t dst0);

    // Sum=A0+A1, diff=Afirst-Asecond; saturate, store dregs, update flags.
    void emit_a_sum_diff(bool a1_minus_a0, uint32_t s, uint32_t dst0, uint32_t dst1);

    // Shared BYTEOP2P: reversed, hi_slot, rounding select the variant.
    void emit_byteop2p(bool reversed, bool hi_slot, bool rounding,
                       uint32_t dst0, uint32_t src0, uint32_t src1);

    // Shared BYTEOP3P: reversed, hi_slot select the variant.
    void emit_byteop3p(bool reversed, bool hi_slot,
                       uint32_t dst0, uint32_t src0, uint32_t src1);
};
