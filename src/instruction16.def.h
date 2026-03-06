#pragma once

// DEF_INSN(opcode, "assembly template", "binary pattern")

#ifndef DEF_INSN
#define DEF_INSN(opcode, asm_template, binary_pattern)
#endif


// ProgCtrl instructions
DEF_INSN(ProgCtrl_NOP,                   "nop",                   "00000000 0000 0000")
DEF_INSN(ProgCtrl_RTS,                   "rts",                   "00000000 0001 0000")
DEF_INSN(ProgCtrl_RTI,                   "rti",                   "00000000 0001 0001")
DEF_INSN(ProgCtrl_RTX,                   "rtx",                   "00000000 0001 0010")
DEF_INSN(ProgCtrl_RTN,                   "rtn",                   "00000000 0001 0011")
DEF_INSN(ProgCtrl_RTE,                   "rte",                   "00000000 0001 0100")
DEF_INSN(ProgCtrl_IDLE,                  "idle",                  "00000000 0010 0000")
DEF_INSN(ProgCtrl_CSYNC,                 "csync",                 "00000000 0010 0011")
DEF_INSN(ProgCtrl_SSYNC,                 "ssync",                 "00000000 0010 0100")
DEF_INSN(ProgCtrl_EMUEXCPT,              "emuexcpt",              "00000000 0010 0101")
DEF_INSN(ProgCtrl_CLI,                   "cli Rd",                "00000000 0011 0ddd")
DEF_INSN(ProgCtrl_STI,                   "sti Rd",                "00000000 0100 0ddd")
DEF_INSN(ProgCtrl_JUMP_PREG,             "jump (Preg)",           "00000000 0101 0ppp")
DEF_INSN(ProgCtrl_CALL_PREG,             "call (Preg)",           "00000000 0110 0ppp")
DEF_INSN(ProgCtrl_CALL_PC_PREG,          "call (pc + Preg)",      "00000000 0111 0ppp")
DEF_INSN(ProgCtrl_JUMP_PC_PREG,          "jump (pc + Preg)",      "00000000 1000 0ppp")
DEF_INSN(ProgCtrl_RAISE,                 "raise imm4",            "00000000 1001 iiii")
DEF_INSN(ProgCtrl_EXCPT,                 "excpt imm4",            "00000000 1010 iiii")
DEF_INSN(ProgCtrl_TESTSET,               "testset (Preg)",        "00000000 1011 0ppp")


// PushPopReg instructions
DEF_INSN(PopReg,                         "Rd = [SP++]",           "00000001 0 0 ggg rrr")
DEF_INSN(PushReg,                        "[--SP] = Rs",           "00000001 0 1 ggg rrr")


// PushPopMultiple instructions
DEF_INSN(PopMultiple_RP,                 "(R7:Rn, P5:Pn) = [SP++]", "0000010 1 1 0 ddd ppp")
DEF_INSN(PopMultiple_R,                  "(R7:Rn) = [SP++]",         "0000010 1 0 0 ddd 000")
DEF_INSN(PopMultiple_P,                  "(P5:Pn) = [SP++]",         "0000010 0 1 0 000 ppp")
DEF_INSN(PushMultiple_RP,                "[--SP] = (R7:Rn, P5:Pn)", "0000010 1 1 1 ddd ppp")
DEF_INSN(PushMultiple_R,                 "[--SP] = (R7:Rn)",         "0000010 1 0 1 ddd 000")
DEF_INSN(PushMultiple_P,                 "[--SP] = (P5:Pn)",         "0000010 0 1 1 000 ppp")


// CC2dreg instructions
DEF_INSN(CC2dreg_Read,                   "Rd = CC",               "00000010 000 00 ddd")
DEF_INSN(CC2dreg_Write,                  "CC = Rs",               "00000010 000 01 ddd")
DEF_INSN(CC2dreg_Negate,                 "CC = !CC",              "00000010 000 11 000")


// CaCTRL instructions
DEF_INSN(CaCTRL_PREFETCH,                "prefetch [Preg]",       "00000010 01 0 00 ppp")
DEF_INSN(CaCTRL_FLUSHINV,                "flushinv [Preg]",       "00000010 01 0 01 ppp")
DEF_INSN(CaCTRL_PREFETCH_pp,             "prefetch [Preg++]",     "00000010 01 1 00 ppp")
DEF_INSN(CaCTRL_FLUSHINV_pp,             "flushinv [Preg++]",     "00000010 01 1 01 ppp")
DEF_INSN(CaCTRL_FLUSH,                   "flush [Preg]",          "00000010 01 0 10 ppp")
DEF_INSN(CaCTRL_FLUSH_pp,                "flush [Preg++]",        "00000010 01 1 10 ppp")
DEF_INSN(CaCTRL_IFLUSH,                  "iflush [Preg]",         "00000010 01 0 11 ppp")
DEF_INSN(CaCTRL_IFLUSH_pp,               "iflush [Preg++]",       "00000010 01 1 11 ppp")


// CC2stat instructions
DEF_INSN(CC2stat_CC_EQ_ASTAT,            "cc = ASTAT[cbit]",      "00000011 0 00 ccccc")
DEF_INSN(CC2stat_CC_OR_ASTAT,            "cc |= ASTAT[cbit]",     "00000011 0 01 ccccc")
DEF_INSN(CC2stat_CC_AND_ASTAT,           "cc &= ASTAT[cbit]",     "00000011 0 10 ccccc")
DEF_INSN(CC2stat_CC_XOR_ASTAT,           "cc ^= ASTAT[cbit]",     "00000011 0 11 ccccc")
DEF_INSN(CC2stat_ASTAT_EQ_CC,            "ASTAT[cbit] = cc",      "00000011 1 00 ccccc")
DEF_INSN(CC2stat_ASTAT_OR_CC,            "ASTAT[cbit] |= cc",     "00000011 1 01 ccccc")
DEF_INSN(CC2stat_ASTAT_AND_CC,           "ASTAT[cbit] &= cc",     "00000011 1 10 ccccc")
DEF_INSN(CC2stat_ASTAT_XOR_CC,           "ASTAT[cbit] ^= cc",     "00000011 1 11 ccccc")


// ccMV instructions
DEF_INSN(ccMV_IF_NOT,                    "if !cc Rd = Rs",        "00000110 d s ddd ddd")
DEF_INSN(ccMV_IF,                        "if cc Rd = Rs",         "00000111 d s ddd ddd")


// CCflag instructions
DEF_INSN(CCflag_EQ,                      "cc = Rx == Ry",         "00001 0 000 g yyy xxx")
DEF_INSN(CCflag_LT,                      "cc = Rx < Ry",          "00001 0 001 g yyy xxx")
DEF_INSN(CCflag_LE,                      "cc = Rx <= Ry",         "00001 0 010 g yyy xxx")
DEF_INSN(CCflag_LT_U,                    "cc = Rx < Ry (iu)",     "00001 0 011 g yyy xxx")
DEF_INSN(CCflag_LE_U,                    "cc = Rx <= Ry (iu)",    "00001 0 100 g yyy xxx")
DEF_INSN(CCflag_EQ_imm,                  "cc = Rx == imm3",       "00001 1 000 g yyy xxx")
DEF_INSN(CCflag_LT_imm,                  "cc = Rx < imm3",        "00001 1 001 g yyy xxx")
DEF_INSN(CCflag_LE_imm,                  "cc = Rx <= imm3",       "00001 1 010 g yyy xxx")
DEF_INSN(CCflag_LT_U_imm,                "cc = Rx < uimm3 (iu)",  "00001 1 011 g yyy xxx")
DEF_INSN(CCflag_LE_U_imm,                "cc = Rx <= uimm3 (iu)", "00001 1 100 g yyy xxx")
DEF_INSN(CCflag_A0_EQ_A1,                "cc = A0 == A1",         "00001 0 101 0 000 000")
DEF_INSN(CCflag_A0_LT_A1,                "cc = A0 < A1",          "00001 0 110 0 000 000")
DEF_INSN(CCflag_A0_LE_A1,                "cc = A0 <= A1",         "00001 0 111 0 000 000")


// ALU2op instructions
DEF_INSN(ALU2op_ASHIFT_RIGHT,            "Rd >>>= Rs",            "010000 0000 ddd ddd")
DEF_INSN(ALU2op_LSHIFT_RIGHT,            "Rd >>= Rs",             "010000 0001 ddd ddd")
DEF_INSN(ALU2op_LSHIFT_LEFT,             "Rd <<= Rs",             "010000 0010 ddd ddd")
DEF_INSN(ALU2op_MUL,                     "Rd *= Rs",              "010000 0011 ddd ddd")
DEF_INSN(ALU2op_ADD_SHIFT1,              "Rd = (Rd + Rs) << 1",   "010000 0100 ddd ddd")
DEF_INSN(ALU2op_ADD_SHIFT2,              "Rd = (Rd + Rs) << 2",   "010000 0101 ddd ddd")
DEF_INSN(ALU2op_DIVQ,                    "divq(Rd, Rs)",          "010000 1000 ddd ddd")
DEF_INSN(ALU2op_DIVS,                    "divs(Rd, Rs)",          "010000 1001 ddd ddd")
DEF_INSN(ALU2op_SEXT_L,                  "Rd = Rs.l (x)",         "010000 1010 ddd ddd")
DEF_INSN(ALU2op_ZEXT_L,                  "Rd = Rs.l (z)",         "010000 1011 ddd ddd")
DEF_INSN(ALU2op_SEXT_B,                  "Rd = Rs.b (x)",         "010000 1100 ddd ddd")
DEF_INSN(ALU2op_ZEXT_B,                  "Rd = Rs.b (z)",         "010000 1101 ddd ddd")
DEF_INSN(ALU2op_NEG,                     "Rd = -Rs",              "010000 1110 ddd ddd")
DEF_INSN(ALU2op_NOT,                     "Rd = ~Rs",              "010000 1111 ddd ddd")


// pseudoDEBUG instructions
DEF_INSN(pseudoDEBUG_DBG_reg,            "dbg Rx",                "11111000 00 ggg rrr")
DEF_INSN(pseudoDEBUG_PRNT_reg,           "prnt Rx",               "11111000 01 ggg rrr")
DEF_INSN(pseudoDEBUG_OUTC_dreg,          "outc Rd",               "11111000 10 000 rrr")
DEF_INSN(pseudoDEBUG_DBG_A0,             "dbg A0",                "11111000 11 ggg 000")
DEF_INSN(pseudoDEBUG_DBG_A1,             "dbg A1",                "11111000 11 ggg 001")
DEF_INSN(pseudoDEBUG_ABORT,              "abort",                 "11111000 11 ggg 011")
DEF_INSN(pseudoDEBUG_HLT,                "hlt",                   "11111000 11 ggg 100")
DEF_INSN(pseudoDEBUG_DBGHALT,            "dbghalt",               "11111000 11 ggg 101")
DEF_INSN(pseudoDEBUG_DBGCMPLX,           "dbgcmplx (Rd)",         "11111000 11 ggg 110")
DEF_INSN(pseudoDEBUG_DBG,                "dbg",                   "11111000 11 ggg 111")
DEF_INSN(pseudoChr_OUTC,                 "outc imm8",             "11111001 cccccccc")

// ujump instructions
DEF_INSN(UJUMP,                          "jump.s offset",         "0010 cccccccccccc")


// BRCC instructions: [15:12]=0001, [11]=T(taken), [10]=B(branch predict), [9:0]=offset
DEF_INSN(BRCC_BRT,                       "if cc jump offset",          "0001 1 0 cccccccccc")
DEF_INSN(BRCC_BRT_BP,                    "if cc jump offset (bp)",      "0001 1 1 cccccccccc")
DEF_INSN(BRCC_BRF,                       "if !cc jump offset",          "0001 0 0 cccccccccc")
DEF_INSN(BRCC_BRF_BP,                    "if !cc jump offset (bp)",     "0001 0 1 cccccccccc")


// REGMV instructions: [15:12]=0011, [11:9]=gd, [8:6]=gs, [5:3]=dst, [2:0]=src
DEF_INSN(REGMV,                          "Reg = Reg",                   "0011 ggg hhh ddd sss")


// PTR2op instructions: [15:9]=0100010, [8:6]=opc, [5:3]=src, [2:0]=dst
DEF_INSN(PTR2op_SUB,                     "Pp -= Ps",                    "0100010 000 sss ddd")
DEF_INSN(PTR2op_LSHIFT_LEFT2,            "Pp = Ps << 0x2",              "0100010 001 sss ddd")
DEF_INSN(PTR2op_LSHIFT_RIGHT2,           "Pp = Ps >> 0x2",              "0100010 011 sss ddd")
DEF_INSN(PTR2op_LSHIFT_RIGHT1,           "Pp = Ps >> 0x1",              "0100010 100 sss ddd")
DEF_INSN(PTR2op_ADD_BREV,                "Pp += Ps (brev)",             "0100010 101 sss ddd")
DEF_INSN(PTR2op_ADD_SHIFT1,              "Pp = (Pp + Ps) << 0x1",       "0100010 110 sss ddd")
DEF_INSN(PTR2op_ADD_SHIFT2,              "Pp = (Pp + Ps) << 0x2",       "0100010 111 sss ddd")


// LOGI2op instructions: [15:11]=01001, [10:8]=opc, [7:3]=imm5, [2:0]=dst
DEF_INSN(LOGI2op_CC_EQ_AND,             "cc = !bittst(Rd,uimm5)",      "01001 000 iiiii ddd")
DEF_INSN(LOGI2op_CC_EQ_BITTST,          "cc = bittst(Rd,uimm5)",       "01001 001 iiiii ddd")
DEF_INSN(LOGI2op_BITSET,                "bitset(Rd,uimm5)",             "01001 010 iiiii ddd")
DEF_INSN(LOGI2op_BITTGL,                "bittgl(Rd,uimm5)",             "01001 011 iiiii ddd")
DEF_INSN(LOGI2op_BITCLR,                "bitclr(Rd,uimm5)",             "01001 100 iiiii ddd")
DEF_INSN(LOGI2op_ASHIFT_RIGHT,          "Rd >>>= uimm5",                "01001 101 iiiii ddd")
DEF_INSN(LOGI2op_LSHIFT_RIGHT,          "Rd >>= uimm5",                 "01001 110 iiiii ddd")
DEF_INSN(LOGI2op_LSHIFT_LEFT,           "Rd <<= uimm5",                 "01001 111 iiiii ddd")


// COMP3op instructions: [15:12]=0101, [11:9]=opc, [8:6]=dst, [5:3]=src1, [2:0]=src0
DEF_INSN(COMP3op_ADD,                    "Rd = Rs + Rt",                "0101 000 ddd ttt sss")
DEF_INSN(COMP3op_SUB,                    "Rd = Rs - Rt",                "0101 001 ddd ttt sss")
DEF_INSN(COMP3op_AND,                    "Rd = Rs & Rt",                "0101 010 ddd ttt sss")
DEF_INSN(COMP3op_OR,                     "Rd = Rs | Rt",                "0101 011 ddd ttt sss")
DEF_INSN(COMP3op_XOR,                    "Rd = Rs ^ Rt",                "0101 100 ddd ttt sss")
DEF_INSN(COMP3op_PADD,                   "Pp = Ps + Pt",                "0101 101 ddd ttt sss")
DEF_INSN(COMP3op_LSHIFT,                 "Rd = Rs << 1",                "0101 110 ddd ttt sss")
DEF_INSN(COMP3op_LSHIFT2,                "Rd = Rs << 2",                "0101 111 ddd ttt sss")


// COMPI2opD instructions: [15:11]=01100, [10]=op, [9:3]=imm7, [2:0]=dst
DEF_INSN(COMPI2opD_EQ,                   "Rd = imm7 (x)",               "01100 0 iiiiiii ddd")
DEF_INSN(COMPI2opD_ADD,                  "Rd += imm7",                  "01100 1 iiiiiii ddd")


// COMPI2opP instructions: [15:11]=01101, [10]=op, [9:3]=imm7, [2:0]=dst
DEF_INSN(COMPI2opP_EQ,                   "Pp = imm7 (x)",               "01101 0 iiiiiii ddd")
DEF_INSN(COMPI2opP_ADD,                  "Pp += imm7",                  "01101 1 iiiiiii ddd")


// LDSTpmod instructions: [15:12]=1000, [11]=W, [10:9]=aop, [8:6]=reg, [5:3]=idx, [2:0]=ptr
// aop=0: 32-bit [ptr++idx]; aop=1: lo W[...]; aop=2: hi W[...]; aop=3: (Z)/(X)
DEF_INSN(LDSTpmod_LD_32,                 "Rd = [Pp ++ Pi]",             "1000 0 00 ddd iii ppp")
DEF_INSN(LDSTpmod_LD_16_lo,             "Rd.l = W[Pp++Pi]",             "1000 0 01 ddd iii ppp")
DEF_INSN(LDSTpmod_LD_16_hi,             "Rd.h = W[Pp++Pi]",             "1000 0 10 ddd iii ppp")
DEF_INSN(LDSTpmod_LD_16_Z,              "Rd = W[Pp++Pi](z)",            "1000 0 11 ddd iii ppp")
DEF_INSN(LDSTpmod_LD_16_X,              "Rd = W[Pp++Pi](x)",            "1000 1 11 ddd iii ppp")
DEF_INSN(LDSTpmod_ST_32,                "[Pp ++ Pi] = Rd",              "1000 1 00 ddd iii ppp")
DEF_INSN(LDSTpmod_ST_16_lo,             "W[Pp ++ Pi] = Rd.l",           "1000 1 01 ddd iii ppp")
DEF_INSN(LDSTpmod_ST_16_hi,             "W[Pp ++ Pi] = Rd.h",           "1000 1 10 ddd iii ppp")


// dagMODim instructions: [15:8]=10011110, [7]=br, [6:5]=11(fixed), [4]=op, [3:2]=m, [1:0]=i
// MUST appear before dspLDST (more specific pattern)
DEF_INSN(dagMODim_ADD,                   "Ii += Mm",                    "10011110 0 11 0 mm ii")
DEF_INSN(dagMODim_SUB,                   "Ii -= Mm",                    "10011110 0 11 1 mm ii")
DEF_INSN(dagMODim_ADD_BREV,              "Ii += Mm (brev)",             "10011110 1 11 0 mm ii")


// dagMODik instructions: [15:4]=100111110110, [3:2]=op, [1:0]=i
// MUST appear before dspLDST (more specific pattern)
DEF_INSN(dagMODik_ADD2,                  "Ii += 2",                     "100111110110 00 ii")
DEF_INSN(dagMODik_SUB2,                  "Ii -= 2",                     "100111110110 01 ii")
DEF_INSN(dagMODik_ADD4,                  "Ii += 4",                     "100111110110 10 ii")
DEF_INSN(dagMODik_SUB4,                  "Ii -= 4",                     "100111110110 11 ii")


// dspLDST instructions: [15:10]=100111, [9]=W, [8:7]=aop, [6:5]=m, [4:3]=i, [2:0]=reg
// MUST appear AFTER dagMODim/dagMODik (those have more specific patterns in same range)
// mm field: 00=full 32-bit, 01=lo half-word, 10=hi half-word, 11=brev (already has dedicated entry)
DEF_INSN(dspLDST_LD_dreg,               "Rd = [Ii++]",                 "100111 0 00 00 ii ddd")
DEF_INSN(dspLDST_LD_dreg_lo,            "Rd.l = W[Ii++]",              "100111 0 00 01 ii ddd")
DEF_INSN(dspLDST_LD_dreg_hi,            "Rd.h = W[Ii++]",              "100111 0 00 10 ii ddd")
DEF_INSN(dspLDST_LD_dreg_mm,            "Rd = [Ii--]",                 "100111 0 01 00 ii ddd")
DEF_INSN(dspLDST_LD_dreg_mm_lo,         "Rd.l = W[Ii--]",              "100111 0 01 01 ii ddd")
DEF_INSN(dspLDST_LD_dreg_mm_hi,         "Rd.h = W[Ii--]",              "100111 0 01 10 ii ddd")
DEF_INSN(dspLDST_LD_dreg_Mmod,          "Rd = [Ii]",                   "100111 0 10 00 ii ddd")
DEF_INSN(dspLDST_LD_dreg_Mmod_lo,       "Rd.l = W[Ii]",                "100111 0 10 01 ii ddd")
DEF_INSN(dspLDST_LD_dreg_Mmod_hi,       "Rd.h = W[Ii]",                "100111 0 10 10 ii ddd")
DEF_INSN(dspLDST_LD_dreg_brev,          "Rd = [Ii++Mm]",               "100111 0 11 mm ii ddd")
DEF_INSN(dspLDST_ST_dreg,               "[Ii++] = Rd",                 "100111 1 00 00 ii ddd")
DEF_INSN(dspLDST_ST_dreg_lo,            "W[Ii++] = Rd.l",              "100111 1 00 01 ii ddd")
DEF_INSN(dspLDST_ST_dreg_hi,            "W[Ii++] = Rd.h",              "100111 1 00 10 ii ddd")
DEF_INSN(dspLDST_ST_dreg_mm,            "[Ii--] = Rd",                 "100111 1 01 00 ii ddd")
DEF_INSN(dspLDST_ST_dreg_mm_lo,         "W[Ii--] = Rd.l",              "100111 1 01 01 ii ddd")
DEF_INSN(dspLDST_ST_dreg_mm_hi,         "W[Ii--] = Rd.h",              "100111 1 01 10 ii ddd")
DEF_INSN(dspLDST_ST_dreg_Mmod,          "[Ii] = Rd",                   "100111 1 10 00 ii ddd")
DEF_INSN(dspLDST_ST_dreg_Mmod_lo,       "W[Ii] = Rd.l",                "100111 1 10 01 ii ddd")
DEF_INSN(dspLDST_ST_dreg_Mmod_hi,       "W[Ii] = Rd.h",                "100111 1 10 10 ii ddd")
DEF_INSN(dspLDST_ST_dreg_brev,          "[Ii++Mm] = Rd",               "100111 1 11 mm ii ddd")


// LDST instructions: [15:12]=1001, [11:10]=sz, [9]=W, [8:7]=aop, [6]=Z, [5:3]=ptr, [2:0]=reg
// sz=00: 32-bit dreg, sz=01: 16-bit, sz=10: 8-bit, sz=11: P-reg
// aop: 00=post-inc [p++], 01=post-dec [p--], 10=indirect [p]

// sz=00, 32-bit dreg loads
DEF_INSN(LDST_LD_32,                     "Rd = [Pp++]",                 "1001 00 0 00 0 ppp ddd")
DEF_INSN(LDST_LD_32_mm,                  "Rd = [Pp--]",                 "1001 00 0 01 0 ppp ddd")
DEF_INSN(LDST_LD_32_ind,                 "Rd = [Pp]",                   "1001 00 0 10 0 ppp ddd")
// sz=00, 32-bit dreg stores
DEF_INSN(LDST_ST_32,                     "[Pp++] = Rd",                 "1001 00 1 00 0 ppp ddd")
DEF_INSN(LDST_ST_32_mm,                  "[Pp--] = Rd",                 "1001 00 1 01 0 ppp ddd")
DEF_INSN(LDST_ST_32_ind,                 "[Pp] = Rd",                   "1001 00 1 10 0 ppp ddd")
// sz=00, Z=1: P-reg loads (pointer register as dest)
DEF_INSN(LDST_LD_P_32_z,                 "Pd = [Ps++]",                 "1001 00 0 00 1 ppp ddd")
DEF_INSN(LDST_LD_P_32_z_mm,              "Pd = [Ps--]",                 "1001 00 0 01 1 ppp ddd")
DEF_INSN(LDST_LD_P_32_z_ind,             "Pd = [Ps]",                   "1001 00 0 10 1 ppp ddd")
// sz=00, Z=1: P-reg stores (pointer register as src)
DEF_INSN(LDST_ST_P_32_z,                 "[Ps++] = Pd",                 "1001 00 1 00 1 ppp ddd")
DEF_INSN(LDST_ST_P_32_z_mm,              "[Ps--] = Pd",                 "1001 00 1 01 1 ppp ddd")
DEF_INSN(LDST_ST_P_32_z_ind,             "[Ps] = Pd",                   "1001 00 1 10 1 ppp ddd")
// sz=01, 16-bit loads (Z=0: zero-extend, Z=1: sign-extend per reference)
DEF_INSN(LDST_LD_16_Z,                   "Rd = W[Pp++](z)",             "1001 01 0 00 0 ppp ddd")
DEF_INSN(LDST_LD_16_Z_mm,                "Rd = W[Pp--](z)",             "1001 01 0 01 0 ppp ddd")
DEF_INSN(LDST_LD_16_Z_ind,               "Rd = W[Pp](z)",               "1001 01 0 10 0 ppp ddd")
DEF_INSN(LDST_LD_16_X,                   "Rd = W[Pp++](x)",             "1001 01 0 00 1 ppp ddd")
DEF_INSN(LDST_LD_16_X_mm,                "Rd = W[Pp--](x)",             "1001 01 0 01 1 ppp ddd")
DEF_INSN(LDST_LD_16_X_ind,               "Rd = W[Pp](x)",               "1001 01 0 10 1 ppp ddd")
// sz=01, 16-bit stores
DEF_INSN(LDST_ST_16,                     "W[Pp++] = Rd",                "1001 01 1 00 0 ppp ddd")
DEF_INSN(LDST_ST_16_mm,                  "W[Pp--] = Rd",                "1001 01 1 01 0 ppp ddd")
DEF_INSN(LDST_ST_16_ind,                 "W[Pp] = Rd",                  "1001 01 1 10 0 ppp ddd")
// sz=10, 8-bit loads (Z=0: zero-extend, Z=1: sign-extend per reference)
DEF_INSN(LDST_LD_8_Z,                    "Rd = B[Pp++](z)",             "1001 10 0 00 0 ppp ddd")
DEF_INSN(LDST_LD_8_Z_mm,                 "Rd = B[Pp--](z)",             "1001 10 0 01 0 ppp ddd")
DEF_INSN(LDST_LD_8_Z_ind,                "Rd = B[Pp](z)",               "1001 10 0 10 0 ppp ddd")
DEF_INSN(LDST_LD_8_X,                    "Rd = B[Pp++](x)",             "1001 10 0 00 1 ppp ddd")
DEF_INSN(LDST_LD_8_X_mm,                 "Rd = B[Pp--](x)",             "1001 10 0 01 1 ppp ddd")
DEF_INSN(LDST_LD_8_X_ind,                "Rd = B[Pp](x)",               "1001 10 0 10 1 ppp ddd")
// sz=10, 8-bit stores
DEF_INSN(LDST_ST_8,                      "B[Pp++] = Rd",                "1001 10 1 00 0 ppp ddd")
DEF_INSN(LDST_ST_8_mm,                   "B[Pp--] = Rd",                "1001 10 1 01 0 ppp ddd")
DEF_INSN(LDST_ST_8_ind,                  "B[Pp] = Rd",                  "1001 10 1 10 0 ppp ddd")
// sz=11, P-reg loads
DEF_INSN(LDST_LD_P_32,                   "Pp = [Ps++]",                 "1001 11 0 00 0 ppp ddd")
DEF_INSN(LDST_LD_P_32_mm,                "Pp = [Ps--]",                 "1001 11 0 01 0 ppp ddd")
DEF_INSN(LDST_LD_P_32_ind,               "Pp = [Ps]",                   "1001 11 0 10 0 ppp ddd")
// sz=11, P-reg stores
DEF_INSN(LDST_ST_P_32,                   "[Ps++] = Pp",                 "1001 11 1 00 0 ppp ddd")
DEF_INSN(LDST_ST_P_32_mm,                "[Ps--] = Pp",                 "1001 11 1 01 0 ppp ddd")
DEF_INSN(LDST_ST_P_32_ind,               "[Ps] = Pp",                   "1001 11 1 10 0 ppp ddd")


// LDSTiiFP instructions: [15:10]=101110, [9]=W, [8:4]=offset(5-bit), [3:0]=reg(4-bit)
// MUST appear BEFORE LDSTii (more specific: bits[15:10]=101110 subset of LDSTii's 101x)
DEF_INSN(LDSTiiFP_LD_32,                 "Reg = [FP + off]",            "101110 0 ooooo rrrr")
DEF_INSN(LDSTiiFP_ST_32,                 "[FP + off] = Reg",            "101110 1 ooooo rrrr")


// LDSTii instructions: [15:13]=101, [12]=W, [11:10]=op, [9:6]=offset(4-bit), [5:3]=ptr, [2:0]=reg
// No byte-store variant (op=2, W=1 is not a valid encoding)
DEF_INSN(LDSTii_LD_32,                   "Rd = [Pp + off]",             "101 0 00 oooo ppp ddd")
DEF_INSN(LDSTii_LD_16_Z,                 "Rd = W[Pp+off](z)",           "101 0 01 oooo ppp ddd")
DEF_INSN(LDSTii_LD_16_X,                 "Rd = W[Pp+off](x)",           "101 0 10 oooo ppp ddd")
DEF_INSN(LDSTii_LD_P_32,                 "Pp = [Ps + off]",             "101 0 11 oooo ppp ddd")
DEF_INSN(LDSTii_ST_32,                   "[Pp + off] = Rd",             "101 1 00 oooo ppp ddd")
DEF_INSN(LDSTii_ST_16,                   "W[Pp + off] = Rd",            "101 1 01 oooo ppp ddd")
DEF_INSN(LDSTii_ST_P_32,                 "[Pp + off] = Pp",             "101 1 11 oooo ppp ddd")