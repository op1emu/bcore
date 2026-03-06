#pragma once

#include <cstdint>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

#include "mem.h"

// Register types
struct DREG {
    uint32_t value;
    DREG(uint32_t v) : value(v) {}
};

struct PREG {
    uint32_t value;
    PREG(uint32_t v) : value(v) {}
};

struct IREG {
    uint32_t value;
    IREG(uint32_t v) : value(v) {}
};

struct MREG {
    uint32_t value;
    MREG(uint32_t v) : value(v) {}
};

class DisasmVisitor {
public:
    using return_type = bool;
    Memory* mem = nullptr;
    uint32_t current_pc = 0;

    std::ostream* _out = &std::cout;
    std::ostream& out() { return *_out; }
    void set_out(std::ostream* o) { _out = o ? o : &std::cout; }

    void set_pc(uint32_t pc) { current_pc = pc; }
    uint16_t iftech(uint32_t addr);

    // Parallel (VLIW) instruction lifecycle hooks (called by decoder)
    void on_parallel_begin();
    void on_parallel_next_slot();
    void on_parallel_end();
    void on_parallel_abort();

    // Helper for register names
    static const char* dregs(uint32_t r);
    static const char* pregs(uint32_t r);
    static const char* dpregs(uint32_t r);  // D or P registers (R0-R7, P0-P5, SP, FP)
    static const char* iregs(uint32_t r);
    static const char* mregs(uint32_t r);
    static const char* statbits(uint32_t c);
    static const char* allregs(uint32_t r, uint32_t g);

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
    bool decode_LoopSetup_LC0_P_half(uint32_t soffset, uint32_t reg, uint32_t eoffset);
    bool decode_LoopSetup_LC1_P_half(uint32_t soffset, uint32_t reg, uint32_t eoffset);

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

    // DSP32 mac: P, w1, w0 fixed per variant; (M, mmod, MM, op1, h01h11, op0, h00h10, dst, src0, src1)
    bool decode_dsp32mac_MNOP(uint32_t M);
    bool decode_dsp32mac_P0_nn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_nn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mac_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1, uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    // DSP32 mult: P, w1, w0 fixed per variant; op1/op0 literal 00 in pattern; (M, mmod, MM, h01h11, h00h10, dst, src0, src1)
    bool decode_dsp32mult_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);
    bool decode_dsp32mult_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1);

    // dsp32alu: aopcde and aop fixed in pattern; HL, s, x, dst0, dst1, src0, src1 variable
    // Most: (M, HL, s, x, dst0, dst1, src0, src1)
    // Special signatures noted inline
    bool decode_dsp32alu_ADDADD(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ADDSUB(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUBADD(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SUBSUB(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // HL is fixed (0 or 1), aop is variable
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
    // aopcde=5: s is fixed=0, so signature: (M, x, dst0, dst1, src0, src1)
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
    // HL=0 fixed: (M, s, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_NEG_NS(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_NEG_S(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // s and HL fixed per entry: (M, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_ACC_A0_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A0_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1A0_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_A1A0_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_COPY_A0_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_COPY_A1_A0(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // ACC_LOAD: s and HL each fixed per entry
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
    // HL fixed per entry: (M, s, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_A0_PLUS_A1_HL_lo(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_PLUS_A1_HL_hi(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // s fixed per entry: (M, HL, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_A0_INC_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_INC_A1_W32(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_DEC_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_A0_DEC_A1_W32(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_SIGN_MULT(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_ACC_ACCUM_SUM(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // HL fixed per entry: (M, s, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_RND_HL_lo(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_RND_HL_hi(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // aop is variable: (M, HL, aop, s, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_SEARCH(uint32_t M, uint32_t HL, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // HL fixed: (M, s, x, dst0, dst1, src0, src1)
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
    // s fixed per entry: (M, HL, x, dst0, dst1, src0, src1)
    bool decode_dsp32alu_BYTEOP1P_T(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP1P_T_R(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP16P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    bool decode_dsp32alu_BYTEOP16M(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1);
    // HL and s fixed per entry: (M, x, dst0, dst1, src0, src1)
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

    // dsp32shift: sopcde and sop fixed in pattern; HLs, dst, src0, src1 are variable
    // Signature: (M, HLs, dst, src0, src1)
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

    // dsp32shiftimm: sopcde, sop, and (for most variants) bit8 fixed in pattern.
    // Signatures: most are (M, HLs, dst, imm5, src1); sopcde=0 arith has (M, HLs, dst, b, imm5, src1)
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
    std::ostream* _saved_out = nullptr;
    std::ostringstream _slot0, _slot1, _slot2;
    int _parallel_slot = 0;
    bool _in_parallel_slot = false;
};
