#pragma once

// DEF_INSN(opcode, "assembly template", "binary pattern")

#ifndef DEF_INSN
#define DEF_INSN(opcode, asm_template, binary_pattern)
#endif


// CALLa instructions (0xe2xxxxxx - 0xe3xxxxxx)
// Pattern: 1110 00 1 S [24-bit address]
// Bits 31-25 = 1110001, bit 24 = S (1=CALL, 0=JUMP.L)
// Address is a 24-bit PC-relative offset (bits 23-0)
DEF_INSN(CALLa_CALL,         "call pcrel24",          "11100 01 1 aaaaaaaaaaaaaaaaaaaaaaaa")
DEF_INSN(CALLa_JUMP,         "jump.l pcrel24",        "11100 01 0 aaaaaaaaaaaaaaaaaaaaaaaa")


// LDIMMhalf instructions (0xe1xxxxxx)
// Pattern: 11100001 ZHS gg rrr hhhhhhhhhhhhhhhh
// Z=zero-extend (bit 23), H=high-half (bit 22), S=sign-extend (bit 21)
// gg=group (bits 20-19), rrr=register (bits 18-16), hhhh=halfword (bits 15-0)
DEF_INSN(LDIMMhalf_low,        "Reg.l = imm16",       "11100001 00 0 gg rrr hhhhhhhhhhhhhhhh")
DEF_INSN(LDIMMhalf_high,       "Reg.h = imm16",       "11100001 01 0 gg rrr hhhhhhhhhhhhhhhh")
DEF_INSN(LDIMMhalf_full,       "Reg = imm16 (z)",     "11100001 10 0 gg rrr hhhhhhhhhhhhhhhh")
DEF_INSN(LDIMMhalf_full_sext,  "Reg = imm16 (x)",     "11100001 00 1 gg rrr hhhhhhhhhhhhhhhh")


// LoopSetup instructions (0xe080xxxx with iw1[11:10]==00)
// Pattern: 11100000 1 rop c ssss rrrr 00 eeeeeeeeee
// rop=operation, c=counter(LC0/LC1), s=start offset, r=register, e=end offset
DEF_INSN(LoopSetup_LC0,        "lsetup(s,e) lc0",     "11100000 1 00 0 ssss rrrr 00 eeeeeeeeee")
DEF_INSN(LoopSetup_LC0_P,      "lsetup(s,e) lc0=P",   "11100000 1 01 0 ssss pppp 00 eeeeeeeeee")
DEF_INSN(LoopSetup_LC1,        "lsetup(s,e) lc1",     "11100000 1 00 1 ssss rrrr 00 eeeeeeeeee")
DEF_INSN(LoopSetup_LC1_P,      "lsetup(s,e) lc1=P",   "11100000 1 01 1 ssss pppp 00 eeeeeeeeee")
DEF_INSN(LoopSetup_LC0_half,   "lsetup(s,e) lc0=P>>1","11100000 1 11 0 ssss pppp 00 eeeeeeeeee")
DEF_INSN(LoopSetup_LC1_half,   "lsetup(s,e) lc1=P>>1","11100000 1 11 1 ssss pppp 00 eeeeeeeeee")


// LDSTidxI instructions — dispatch: (iw0 & 0xfc00) == 0xe400
// Layout: 111001 W Z sz[1:0] ptr[2:0] reg[2:0] | offset[15:0]
DEF_INSN(LDSTidxI_LD_32,   "Dreg = [Preg+off]",     "111001 0 0 00 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_LD_P_32, "Preg = [Preg+off]",     "111001 0 1 00 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_LD_16_Z, "Dreg = W[Preg+off](Z)", "111001 0 0 01 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_LD_16_X, "Dreg = W[Preg+off](X)", "111001 0 1 01 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_LD_B_Z,  "Dreg = B[Preg+off](Z)", "111001 0 0 10 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_LD_B_X,  "Dreg = B[Preg+off](X)", "111001 0 1 10 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_ST_32,   "[Preg+off] = Dreg",     "111001 1 0 00 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_ST_P_32, "[Preg+off] = Preg",     "111001 1 1 00 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_ST_16,   "W[Preg+off] = Dreg",    "111001 1 0 01 ppp rrr oooooooooooooooo")
DEF_INSN(LDSTidxI_ST_B,    "B[Preg+off] = Dreg",    "111001 1 0 10 ppp rrr oooooooooooooooo")

// PseudoDbg_Assert instructions (0xf0xxxxxx - 0xfexxxxxx)
// Pattern: 11110 ccc dd gg rrr eeeeeeeeeeeeeeee
// ccc=code (bits 27-29), dd=dbgop (bits 22-23), gg=grp (bits 19-21), rrr=regtest (bits 16-18), e=expected (bits 0-15)
// dbgop: 00=DBGA(lo), 01=DBGA(hi), 10=DBGAL, 11=DBGAH
DEF_INSN(pseudoDbg_Assert_lo,    "dbga (Rx.l, imm16)", "11110 000 00 ggg rrr eeeeeeeeeeeeeeee")
DEF_INSN(pseudoDbg_Assert_hi,    "dbga (Rx.h, imm16)", "11110 000 01 ggg rrr eeeeeeeeeeeeeeee")
DEF_INSN(pseudoDbg_Assert_low,   "dbgal (Rx, imm16)",  "11110 000 10 ggg rrr eeeeeeeeeeeeeeee")
DEF_INSN(pseudoDbg_Assert_high,  "dbgah (Rx, imm16)",  "11110 000 11 ggg rrr eeeeeeeeeeeeeeee")


// Linkage instructions (0xe800xxxx)
DEF_INSN(Linkage_LINK,         "link framesize",      "1110100000000000 ffffffffffffffff")
DEF_INSN(Linkage_UNLINK,       "unlink",              "1110100000000001 0000000000000000")


// DSP32 instructions (0xc0000000 - 0xcfffffff)
// All start with 1100 in bits [31:28]

// dsp32mac / dsp32mult field layout:
//   [31:28]=1100  [27]=M  [26:25]=00(mac)/01(mult)
//   [24:21]=mmod(4)  [20]=MM  [19]=P  [18]=w1  [17:16]=op1(2)
//   [15]=h01  [14]=h11  [13]=w0  [12:11]=op0(2)  [10]=h00  [9]=h10
//   [8:6]=dst(3)  [5:3]=src0(3)  [2:0]=src1(3)
//
// Pattern letter key: M=M, m=mmod(4-bit), j=MM(1-bit), o=op1(2-bit),
//   a=h01h11(2-bit), k=op0(2-bit), e=h00h10(2-bit), d=dst, f=src0, g=src1
// P, w1, w0 are fixed as literal 0/1 bits per variant.

// dsp32mac: MNOP special case (MM=0,P=0,w1=0,op1=3, w0=0,op0=3, aa=ee=dst=src=0)
DEF_INSN(dsp32mac_MNOP,  "mnop",            "1100 M 00 mmmm 0 0 0 11 00 0 11 00 000 000 000")

// dsp32mac P=0 variants (half-word writes: Rn.h for mac1, Rn.l for mac0)
// Visitor: (M, mmod, MM, op1, h01h11, op0, h00h10, dst, src0, src1)
DEF_INSN(dsp32mac_P0_nn, "(A1op, A0op)",    "1100 M 00 mmmm j 0 0 oo aa 0 kk ee ddd fff ggg")
DEF_INSN(dsp32mac_P0_Wn, "Rn.h=, A0op",    "1100 M 00 mmmm j 0 1 oo aa 0 kk ee ddd fff ggg")
DEF_INSN(dsp32mac_P0_nW, "A1op, Rn.l=",    "1100 M 00 mmmm j 0 0 oo aa 1 kk ee ddd fff ggg")
DEF_INSN(dsp32mac_P0_WW, "Rn.h=, Rn.l=",  "1100 M 00 mmmm j 0 1 oo aa 1 kk ee ddd fff ggg")

// dsp32mac P=1 variants (full-word writes: R(dst+1) for mac1, R(dst) for mac0)
// Visitor: (M, mmod, MM, op1, h01h11, op0, h00h10, dst, src0, src1)
DEF_INSN(dsp32mac_P1_nn, "(A1op, A0op)",    "1100 M 00 mmmm j 1 0 oo aa 0 kk ee ddd fff ggg")
DEF_INSN(dsp32mac_P1_Wn, "R(n+1)=, A0op",  "1100 M 00 mmmm j 1 1 oo aa 0 kk ee ddd fff ggg")
DEF_INSN(dsp32mac_P1_nW, "A1op, Rn=",      "1100 M 00 mmmm j 1 0 oo aa 1 kk ee ddd fff ggg")
DEF_INSN(dsp32mac_P1_WW, "R(n+1)=, Rn=",  "1100 M 00 mmmm j 1 1 oo aa 1 kk ee ddd fff ggg")

// dsp32mult: op1 and op0 are present in encoding but are ignored (don't-care).
// w1=0 && w0=0 is always illegal, so only 3 (w1,w0) combos × 2 P values = 6 entries.
// Visitor: (M, mmod, MM, _op1, h01h11, _op0, h00h10, dst, src0, src1)
// Note: _op1 (bits 17:16) and _op0 (bits 12:11) are captured but ignored.

// dsp32mult P=0 variants (half-word writes: Rn.h for mult1, Rn.l for mult0)
DEF_INSN(dsp32mult_P0_Wn, "Rn.h=mult",     "1100 M 01 mmmm j 0 1 pp aa 0 qq ee ddd fff ggg")
DEF_INSN(dsp32mult_P0_nW, "Rn.l=mult",     "1100 M 01 mmmm j 0 0 pp aa 1 qq ee ddd fff ggg")
DEF_INSN(dsp32mult_P0_WW, "Rn.h=, Rn.l=", "1100 M 01 mmmm j 0 1 pp aa 1 qq ee ddd fff ggg")

// dsp32mult P=1 variants (full-word writes: R(dst+1) for mult1, R(dst) for mult0)
DEF_INSN(dsp32mult_P1_Wn, "R(n+1)=mult",   "1100 M 01 mmmm j 1 1 pp aa 0 qq ee ddd fff ggg")
DEF_INSN(dsp32mult_P1_nW, "Rn=mult",       "1100 M 01 mmmm j 1 0 pp aa 1 qq ee ddd fff ggg")
DEF_INSN(dsp32mult_P1_WW, "R(n+1)=, Rn=", "1100 M 01 mmmm j 1 1 pp aa 1 qq ee ddd fff ggg")

// dsp32alu: (iw0 & 0xf7c0) == 0xc400
// bits[31:28]=1100, [27]=M, [26:25]=10, [24:22]=000(fixed), [21]=HL, [20:16]=aopcde(5)
// [15:14]=aop(2), [13]=s, [12]=x, [11:9]=dst0, [8:6]=dst1, [5:3]=src0, [2:0]=src1
// s and x remain as variable args (they control output suffixes).
// Signature (when aopcde, aop, and HL are all fixed): (M, s, x, dst0, dst1, src0, src1)
// Signature (when HL also variable): (M, HL, s, x, dst0, dst1, src0, src1)
// aopcde=0: complex vector add/sub (aop selects operation; dst1 used for dual output via aopcde=1)
DEF_INSN(dsp32alu_ADDADD,         "Rd = Rs +|+ Rt",         "1100 M 10 000 H 00000 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADDSUB,         "Rd = Rs +|- Rt",         "1100 M 10 000 H 00000 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUBADD,         "Rd = Rs -|+ Rt",         "1100 M 10 000 H 00000 10 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUBSUB,         "Rd = Rs -|- Rt",         "1100 M 10 000 H 00000 11 s x ddd eee fff ggg")
// aopcde=1: quad SIMD add/sub (two outputs; HL selects +|-+,-|+ vs +|+,-|-)
DEF_INSN(dsp32alu_QUADADD_HL0,    "Rd1=Rs+|+Rt,Rd0=Rs-|-Rt","1100 M 10 000 0 00001 pp s x ddd eee fff ggg")
DEF_INSN(dsp32alu_QUADADD_HL1,    "Rd1=Rs+|-Rt,Rd0=Rs-|+Rt","1100 M 10 000 1 00001 pp s x ddd eee fff ggg")
// aopcde=2: 16-bit half-word add (8 variants: HL x aop)
DEF_INSN(dsp32alu_ADD16_HLd0_LL,  "Rd.l = Rs.l + Rt.l",    "1100 M 10 000 0 00010 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd0_LH,  "Rd.l = Rs.l + Rt.h",    "1100 M 10 000 0 00010 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd0_HL,  "Rd.l = Rs.h + Rt.l",    "1100 M 10 000 0 00010 10 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd0_HH,  "Rd.l = Rs.h + Rt.h",    "1100 M 10 000 0 00010 11 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd1_LL,  "Rd.h = Rs.l + Rt.l",    "1100 M 10 000 1 00010 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd1_LH,  "Rd.h = Rs.l + Rt.h",    "1100 M 10 000 1 00010 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd1_HL,  "Rd.h = Rs.h + Rt.l",    "1100 M 10 000 1 00010 10 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD16_HLd1_HH,  "Rd.h = Rs.h + Rt.h",    "1100 M 10 000 1 00010 11 s x ddd eee fff ggg")
// aopcde=3: 16-bit half-word sub (8 variants: HL x aop)
DEF_INSN(dsp32alu_SUB16_HLd0_LL,  "Rd.l = Rs.l - Rt.l",    "1100 M 10 000 0 00011 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd0_LH,  "Rd.l = Rs.l - Rt.h",    "1100 M 10 000 0 00011 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd0_HL,  "Rd.l = Rs.h - Rt.l",    "1100 M 10 000 0 00011 10 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd0_HH,  "Rd.l = Rs.h - Rt.h",    "1100 M 10 000 0 00011 11 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd1_LL,  "Rd.h = Rs.l - Rt.l",    "1100 M 10 000 1 00011 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd1_LH,  "Rd.h = Rs.l - Rt.h",    "1100 M 10 000 1 00011 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd1_HL,  "Rd.h = Rs.h - Rt.l",    "1100 M 10 000 1 00011 10 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB16_HLd1_HH,  "Rd.h = Rs.h - Rt.h",    "1100 M 10 000 1 00011 11 s x ddd eee fff ggg")
// aopcde=4: 32-bit add/sub
DEF_INSN(dsp32alu_ADD32,          "Rd = Rs + Rt",           "1100 M 10 000 H 00100 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB32,          "Rd = Rs - Rt",           "1100 M 10 000 H 00100 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADDSUB32_dual,  "Rd1=Rs+Rt, Rd0=Rs-Rt",   "1100 M 10 000 H 00100 10 s x ddd eee fff ggg")
// aopcde=5: rounded add/sub (aop+HL selects specific combination; x is direction)
// Each aop/HL/x combination is a distinct instruction
DEF_INSN(dsp32alu_ADD_RND12_LO,   "Rd.l = Rs + Rt (RND12)", "1100 M 10 000 0 00101 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB_RND12_LO,   "Rd.l = Rs - Rt (RND12)", "1100 M 10 000 0 00101 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD_RND20_LO,   "Rd.l = Rs + Rt (RND20)", "1100 M 10 000 0 00101 10 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB_RND20_LO,   "Rd.l = Rs - Rt (RND20)", "1100 M 10 000 0 00101 11 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD_RND12_HI,   "Rd.h = Rs + Rt (RND12)", "1100 M 10 000 1 00101 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB_RND12_HI,   "Rd.h = Rs - Rt (RND12)", "1100 M 10 000 1 00101 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ADD_RND20_HI,   "Rd.h = Rs + Rt (RND20)", "1100 M 10 000 1 00101 10 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_SUB_RND20_HI,   "Rd.h = Rs - Rt (RND20)", "1100 M 10 000 1 00101 11 0 x ddd eee fff ggg")
// aopcde=6: vector MAX/MIN/ABS
DEF_INSN(dsp32alu_VMAX,           "Rd = MAX (Rs, Rt) (V)",  "1100 M 10 000 H 00110 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_VMIN,           "Rd = MIN (Rs, Rt) (V)",  "1100 M 10 000 H 00110 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_VABS,           "Rd = ABS Rs (V)",        "1100 M 10 000 H 00110 10 s x ddd eee fff ggg")
// aopcde=7: scalar MAX/MIN/ABS/NEG
DEF_INSN(dsp32alu_MAX,            "Rd = MAX (Rs, Rt)",      "1100 M 10 000 H 00111 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_MIN,            "Rd = MIN (Rs, Rt)",      "1100 M 10 000 H 00111 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ABS,            "Rd = ABS Rs",            "1100 M 10 000 H 00111 10 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_NEG_NS,          "Rd = -Rs (NS)",          "1100 M 10 000 0 00111 11 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_NEG_S,           "Rd = -Rs (S)",           "1100 M 10 000 0 00111 11 1 x ddd eee fff ggg")
// aopcde=8: accumulator clear/copy (aop and s fixed per entry, HL ignored)
DEF_INSN(dsp32alu_ACC_A0_CLR,     "A0 = 0",                 "1100 M 10 000 H 01000 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_A0_SAT,     "A0 = A0 (S)",            "1100 M 10 000 H 01000 00 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_A1_CLR,     "A1 = 0",                 "1100 M 10 000 H 01000 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_A1_SAT,     "A1 = A1 (S)",            "1100 M 10 000 H 01000 01 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_A1A0_CLR,   "A1 = A0 = 0",            "1100 M 10 000 H 01000 10 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_A1A0_SAT,   "A1 = A1 (S), A0 = A0 (S)","1100 M 10 000 H 01000 10 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_COPY_A0_A1, "A0 = A1",                "1100 M 10 000 H 01000 11 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_COPY_A1_A0, "A1 = A0",                "1100 M 10 000 H 01000 11 1 x ddd eee fff ggg")
// aopcde=9: accumulator load (s=0: half-reg, HL selects half; s=1: full dreg)
// Note: both s=0 and s=1 variants use same aopcde+aop, so keep H and s variable, dispatch at runtime
// aopcde=9: accumulator load — split by s and HL
DEF_INSN(dsp32alu_ACC_LOAD_AOP0_full, "A0 = Rs",             "1100 M 10 000 H 01001 00 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP0_lo,  "A0.L = Rs.l",         "1100 M 10 000 0 01001 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP0_hi,  "A0.H = Rs.h",         "1100 M 10 000 1 01001 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP1,  "A0.X = Rs.l",            "1100 M 10 000 H 01001 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP2_full, "A1 = Rs",             "1100 M 10 000 H 01001 10 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP2_lo,  "A1.L = Rs.l",         "1100 M 10 000 0 01001 10 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP2_hi,  "A1.H = Rs.h",         "1100 M 10 000 1 01001 10 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_LOAD_AOP3,  "A1.X = Rs.l",            "1100 M 10 000 H 01001 11 0 x ddd eee fff ggg")
// aopcde=10: accumulator extract
DEF_INSN(dsp32alu_A0X_READ,       "Rd.l = A0.X",            "1100 M 10 000 H 01010 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A1X_READ,       "Rd.l = A1.X",            "1100 M 10 000 H 01010 01 s x ddd eee fff ggg")
// aopcde=11: accumulator add/subtract combinations
DEF_INSN(dsp32alu_A0_PLUS_A1,     "Rd = (A0 += A1)",        "1100 M 10 000 H 01011 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0_PLUS_A1_HL_lo, "Rd.l = (A0 += A1)",   "1100 M 10 000 0 01011 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0_PLUS_A1_HL_hi, "Rd.h = (A0 += A1)",   "1100 M 10 000 1 01011 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0_INC_A1,      "A0 += A1",               "1100 M 10 000 H 01011 10 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0_INC_A1_W32,  "A0 += A1 (W32)",         "1100 M 10 000 H 01011 10 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0_DEC_A1,      "A0 -= A1",               "1100 M 10 000 H 01011 11 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0_DEC_A1_W32,  "A0 -= A1 (W32)",         "1100 M 10 000 H 01011 11 1 x ddd eee fff ggg")
// aopcde=12: accumulator to register
DEF_INSN(dsp32alu_SIGN_MULT,      "Rd.h=Rd.l=SIGN(Rs.h)*Rt.h+SIGN(Rs.l)*Rt.l","1100 M 10 000 H 01100 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_ACC_ACCUM_SUM,  "Rd1=A1.L+A1.H, Rd0=A0.L+A0.H","1100 M 10 000 H 01100 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_RND_HL_lo,      "Rd.l = Rs (RND)",        "1100 M 10 000 0 01100 11 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_RND_HL_hi,      "Rd.h = Rs (RND)",        "1100 M 10 000 1 01100 11 s x ddd eee fff ggg")
// aopcde=13: SEARCH
DEF_INSN(dsp32alu_SEARCH,         "(Rd1, Rd0) = SEARCH Rs (mod)","1100 M 10 000 H 01101 pp s x ddd eee fff ggg")
// aopcde=14: accumulator negation
DEF_INSN(dsp32alu_A_NEG_HL0_AOP0, "A0 = -A0",               "1100 M 10 000 0 01110 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_NEG_HL0_AOP1, "A0 = -A1",               "1100 M 10 000 0 01110 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_NEG_HL1_AOP0, "A1 = -A0",               "1100 M 10 000 1 01110 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_NEG_HL1_AOP1, "A1 = -A1",               "1100 M 10 000 1 01110 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_NEG_BOTH,     "A1 = -A1, A0 = -A0",     "1100 M 10 000 0 01110 11 s x ddd eee fff ggg")
// aopcde=15: vector negation
DEF_INSN(dsp32alu_NEG_V,          "Rd = -Rs (V)",            "1100 M 10 000 0 01111 11 s x ddd eee fff ggg")
// aopcde=16: accumulator absolute value
DEF_INSN(dsp32alu_A_ABS_HL0_AOP0, "A0 = ABS A0",            "1100 M 10 000 0 10000 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_ABS_HL0_AOP1, "A0 = ABS A1",            "1100 M 10 000 0 10000 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_ABS_HL1_AOP0, "A1 = ABS A0",            "1100 M 10 000 1 10000 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_ABS_HL1_AOP1, "A1 = ABS A1",            "1100 M 10 000 1 10000 01 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A_ABS_BOTH,     "A1 = ABS A1, A0 = ABS A0","1100 M 10 000 0 10000 11 s x ddd eee fff ggg")
// aopcde=17: accumulator add/sub outputs
DEF_INSN(dsp32alu_A1pA0_A1mA0,   "Rd1=A1+A0, Rd0=A1-A0",   "1100 M 10 000 H 10001 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_A0pA1_A0mA1,   "Rd1=A0+A1, Rd0=A0-A1",   "1100 M 10 000 H 10001 01 s x ddd eee fff ggg")
// aopcde=18: SAA and DISALGNEXCPT
DEF_INSN(dsp32alu_SAA,            "SAA (Rs+1:Rs, Rt+1:Rt)",  "1100 M 10 000 H 10010 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_DISALGNEXCPT,   "DISALGNEXCPT",            "1100 M 10 000 H 10010 11 s x ddd eee fff ggg")
// aopcde=20: BYTEOP1P
DEF_INSN(dsp32alu_BYTEOP1P,       "Rd = BYTEOP1P",          "1100 M 10 000 H 10100 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP1P_T,     "Rd = BYTEOP1P (T)",      "1100 M 10 000 H 10100 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP1P_T_R,   "Rd = BYTEOP1P (T, R)",   "1100 M 10 000 H 10100 01 1 x ddd eee fff ggg")
// aopcde=21: BYTEOP16P/M
DEF_INSN(dsp32alu_BYTEOP16P,      "(Rd1,Rd0) = BYTEOP16P",  "1100 M 10 000 H 10101 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP16M,      "(Rd1,Rd0) = BYTEOP16M",  "1100 M 10 000 H 10101 01 s x ddd eee fff ggg")
// aopcde=22: BYTEOP2P (HL selects RNDH/RNDL; aop selects RND vs T; s adds ", R")
DEF_INSN(dsp32alu_BYTEOP2P_RNDL,  "Rd = BYTEOP2P (RNDL)",   "1100 M 10 000 0 10110 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_RNDL_R,"Rd = BYTEOP2P (RNDL, R)","1100 M 10 000 0 10110 00 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_RNDH,  "Rd = BYTEOP2P (RNDH)",   "1100 M 10 000 1 10110 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_RNDH_R,"Rd = BYTEOP2P (RNDH, R)","1100 M 10 000 1 10110 00 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_TL,    "Rd = BYTEOP2P (TL)",     "1100 M 10 000 0 10110 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_TL_R,  "Rd = BYTEOP2P (TL, R)",  "1100 M 10 000 0 10110 01 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_TH,    "Rd = BYTEOP2P (TH)",     "1100 M 10 000 1 10110 01 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP2P_TH_R,  "Rd = BYTEOP2P (TH, R)",  "1100 M 10 000 1 10110 01 1 x ddd eee fff ggg")
// aopcde=23: BYTEOP3P (HL selects HI/LO; s adds ", R")
DEF_INSN(dsp32alu_BYTEOP3P_LO,    "Rd = BYTEOP3P (LO)",     "1100 M 10 000 0 10111 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP3P_LO_R,  "Rd = BYTEOP3P (LO, R)",  "1100 M 10 000 0 10111 00 1 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP3P_HI,    "Rd = BYTEOP3P (HI)",     "1100 M 10 000 1 10111 00 0 x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEOP3P_HI_R,  "Rd = BYTEOP3P (HI, R)",  "1100 M 10 000 1 10111 00 1 x ddd eee fff ggg")
// aopcde=24: BYTEPACK/BYTEUNPACK
DEF_INSN(dsp32alu_BYTEPACK,       "Rd = BYTEPACK (Rs, Rt)",  "1100 M 10 000 H 11000 00 s x ddd eee fff ggg")
DEF_INSN(dsp32alu_BYTEUNPACK,     "(Rd1,Rd0) = BYTEUNPACK",  "1100 M 10 000 H 11000 01 s x ddd eee fff ggg")

// dsp32shift: (iw0 & 0xf780) == 0xc600 AND (iw1 & 0x01c0) == 0
// bits[31:28]=1100, [27]=M, [26:23]=1100, [22:21]=00(fixed), [20:16]=sopcde(5)
// [15:14]=sop(2), [13:12]=HLs(2), [11:9]=dst(3), [8:6]=000(fixed), [5:3]=src0, [2:0]=src1
// HLs is kept as a variable arg for all variants.
// sopcde=0: half-word ASHIFT/LSHIFT by register
DEF_INSN(dsp32shift_ASHIFT16,    "dReg_h/l = ASHIFT dReg_h/l BY dReg.l",      "1100 M 110 0 00 00000 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ASHIFT16S,   "dReg_h/l = ASHIFT dReg_h/l BY dReg.l (S)",  "1100 M 110 0 00 00000 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_LSHIFT16,    "dReg_h/l = LSHIFT dReg_h/l BY dReg.l",      "1100 M 110 0 00 00000 10 hh ddd 000 aaa bbb")
// sopcde=1: vector 32-bit shifts
DEF_INSN(dsp32shift_VASHIFT,     "Rd = ASHIFT Rs BY Rt.l (V)",                 "1100 M 110 0 00 00001 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_VASHIFTS,    "Rd = ASHIFT Rs BY Rt.l (V, S)",              "1100 M 110 0 00 00001 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_VLSHIFT,     "Rd = LSHIFT Rs BY Rt.l (V)",                 "1100 M 110 0 00 00001 10 hh ddd 000 aaa bbb")
// sopcde=2: 32-bit shifts
DEF_INSN(dsp32shift_ASHIFT32,    "Rd = ASHIFT Rs BY Rt.l",                     "1100 M 110 0 00 00010 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ASHIFT32S,   "Rd = ASHIFT Rs BY Rt.l (S)",                 "1100 M 110 0 00 00010 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_LSHIFT32,    "Rd = LSHIFT Rs BY Rt.l",                     "1100 M 110 0 00 00010 10 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ROT32,       "Rd = ROT Rs BY Rt.l",                        "1100 M 110 0 00 00010 11 hh ddd 000 aaa bbb")
// sopcde=3: accumulator shifts (bit[0] of HLs selects A0/A1; bit[1] is don't-care)
DEF_INSN(dsp32shift_ACC_ASHIFT_A0, "A0 = ASHIFT A0 BY Rt.l",              "1100 M 110 0 00 00011 00 h0 ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ACC_ASHIFT_A1, "A1 = ASHIFT A1 BY Rt.l",              "1100 M 110 0 00 00011 00 h1 ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ACC_LSHIFT_A0, "A0 = LSHIFT A0 BY Rt.l",              "1100 M 110 0 00 00011 01 h0 ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ACC_LSHIFT_A1, "A1 = LSHIFT A1 BY Rt.l",              "1100 M 110 0 00 00011 01 h1 ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ACC_ROT_A0,    "A0 = ROT A0 BY Rt.l",                 "1100 M 110 0 00 00011 10 h0 ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ACC_ROT_A1,    "A1 = ROT A1 BY Rt.l",                 "1100 M 110 0 00 00011 10 h1 ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ROT32_dreg,  "Rd = ROT Rs BY Rt.l",                        "1100 M 110 0 00 00011 11 hh ddd 000 aaa bbb")
// sopcde=4: PACK
DEF_INSN(dsp32shift_PACK_LL,     "Rd = PACK (Rs.l, Rt.l)",                     "1100 M 110 0 00 00100 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_PACK_LH,     "Rd = PACK (Rs.l, Rt.h)",                     "1100 M 110 0 00 00100 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_PACK_HL,     "Rd = PACK (Rs.h, Rt.l)",                     "1100 M 110 0 00 00100 10 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_PACK_HH,     "Rd = PACK (Rs.h, Rt.h)",                     "1100 M 110 0 00 00100 11 hh ddd 000 aaa bbb")
// sopcde=5: SIGNBITS on dreg/half
DEF_INSN(dsp32shift_SIGNBITS_32, "Rd.l = SIGNBITS Rs",                         "1100 M 110 0 00 00101 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_SIGNBITS_16L,"Rd.l = SIGNBITS Rs.l",                       "1100 M 110 0 00 00101 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_SIGNBITS_16H,"Rd.l = SIGNBITS Rs.h",                       "1100 M 110 0 00 00101 10 hh ddd 000 aaa bbb")
// sopcde=6: SIGNBITS on accumulator / ONES
DEF_INSN(dsp32shift_SIGNBITS_A0, "Rd.l = SIGNBITS A0",                         "1100 M 110 0 00 00110 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_SIGNBITS_A1, "Rd.l = SIGNBITS A1",                         "1100 M 110 0 00 00110 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ONES,        "Rd.l = ONES Rs",                             "1100 M 110 0 00 00110 11 hh ddd 000 aaa bbb")
// sopcde=7: EXPADJ
DEF_INSN(dsp32shift_EXPADJ_32,   "Rd.l = EXPADJ (Rs, Rt.l)",                   "1100 M 110 0 00 00111 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_EXPADJ_V,    "Rd.l = EXPADJ (Rs, Rt.l) (V)",               "1100 M 110 0 00 00111 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_EXPADJ_16L,  "Rd.l = EXPADJ (Rs.l, Rt.l)",                 "1100 M 110 0 00 00111 10 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_EXPADJ_16H,  "Rd.l = EXPADJ (Rs.h, Rt.l)",                 "1100 M 110 0 00 00111 11 hh ddd 000 aaa bbb")
// sopcde=8: BITMUX
DEF_INSN(dsp32shift_BITMUX_ASR,  "BITMUX (Rs, Rt, A0) (ASR)",                  "1100 M 110 0 00 01000 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_BITMUX_ASL,  "BITMUX (Rs, Rt, A0) (ASL)",                  "1100 M 110 0 00 01000 01 hh ddd 000 aaa bbb")
// sopcde=9: VIT_MAX
DEF_INSN(dsp32shift_VITMAX_ASL,  "Rd.l = VIT_MAX (Rs) (ASL)",                  "1100 M 110 0 00 01001 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_VITMAX_ASR,  "Rd.l = VIT_MAX (Rs) (ASR)",                  "1100 M 110 0 00 01001 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_VITMAX2_ASL, "Rd = VIT_MAX (Rs, Rt) (ASL)",                "1100 M 110 0 00 01001 10 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_VITMAX2_ASR, "Rd = VIT_MAX (Rs, Rt) (ASR)",                "1100 M 110 0 00 01001 11 hh ddd 000 aaa bbb")
// sopcde=10: EXTRACT / DEPOSIT
DEF_INSN(dsp32shift_EXTRACT_Z,   "Rd = EXTRACT (Rs, Rt.l) (Z)",                "1100 M 110 0 00 01010 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_EXTRACT_X,   "Rd = EXTRACT (Rs, Rt.l) (X)",                "1100 M 110 0 00 01010 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_DEPOSIT,     "Rd = DEPOSIT (Rs, Rt)",                       "1100 M 110 0 00 01010 10 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_DEPOSIT_X,   "Rd = DEPOSIT (Rs, Rt) (X)",                  "1100 M 110 0 00 01010 11 hh ddd 000 aaa bbb")
// sopcde=11: BXORSHIFT / BXOR (two-operand)
DEF_INSN(dsp32shift_BXORSHIFT,   "Rd.l = CC = BXORSHIFT (A0, Rs)",             "1100 M 110 0 00 01011 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_BXOR,        "Rd.l = CC = BXOR (A0, Rs)",                  "1100 M 110 0 00 01011 01 hh ddd 000 aaa bbb")
// sopcde=12: BXORSHIFT / BXOR (three-operand)
DEF_INSN(dsp32shift_BXORSHIFT3,  "A0 = BXORSHIFT (A0, A1, CC)",               "1100 M 110 0 00 01100 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_BXOR3,       "Rd.l = CC = BXOR (A0, A1, CC)",             "1100 M 110 0 00 01100 01 hh ddd 000 aaa bbb")
// sopcde=13: ALIGN
DEF_INSN(dsp32shift_ALIGN8,      "Rd = ALIGN8 (Rs, Rt)",                       "1100 M 110 0 00 01101 00 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ALIGN16,     "Rd = ALIGN16 (Rs, Rt)",                      "1100 M 110 0 00 01101 01 hh ddd 000 aaa bbb")
DEF_INSN(dsp32shift_ALIGN24,     "Rd = ALIGN24 (Rs, Rt)",                      "1100 M 110 0 00 01101 10 hh ddd 000 aaa bbb")

// dsp32shiftimm: (iw0 & 0xf780) == 0xc680 (bit[23]=1)
// Same iw0 layout as dsp32shift but bit[23]=1; iw1[8:3]=immag(6-bit), [2:0]=src1
// The 6-bit immag field is split as "b iiiii" where b=bit8 selects shift direction.
// sopcde=0: half-word shifts (HLs selects src/dst halves)
DEF_INSN(dsp32shiftimm_ASHIFT16_arith, "dReg_h/l = dReg_h/l >>> newimmag",     "1100 M 110 1 00 00000 00 hh ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_ASHIFT16S_left, "dReg_h/l = dReg_h/l << immag (S)",     "1100 M 110 1 00 00000 01 hh ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_ASHIFT16S_arith,"dReg_h/l = dReg_h/l >>> newimmag (S)", "1100 M 110 1 00 00000 01 hh ddd 1 iiiii rrr")
DEF_INSN(dsp32shiftimm_LSHIFT16_left,  "dReg_h/l = dReg_h/l << immag",         "1100 M 110 1 00 00000 10 hh ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_LSHIFT16_right, "dReg_h/l = dReg_h/l >> newimmag",      "1100 M 110 1 00 00000 10 hh ddd 1 iiiii rrr")
// sopcde=1: vector shifts
DEF_INSN(dsp32shiftimm_VASHIFT_arith,  "Rd = Rs >>> newimmag (V)",              "1100 M 110 1 00 00001 00 hh ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_VASHIFTS_left,  "Rd = Rs << immag (V, S)",               "1100 M 110 1 00 00001 01 hh ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_VASHIFTS_arith, "Rd = Rs >>> newimmag (V, S)",           "1100 M 110 1 00 00001 01 hh ddd 1 iiiii rrr")
DEF_INSN(dsp32shiftimm_VLSHIFT_left,   "Rd = Rs << immag (V)",                  "1100 M 110 1 00 00001 10 hh ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_VLSHIFT_right,  "Rd = Rs >> newimmag (V)",               "1100 M 110 1 00 00001 10 hh ddd 1 iiiii rrr")
// sopcde=2: 32-bit shifts
DEF_INSN(dsp32shiftimm_ASHIFT32_arith, "Rd = Rs >>> newimmag",                  "1100 M 110 1 00 00010 00 hh ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_ASHIFT32S_left, "Rd = Rs << immag (S)",                  "1100 M 110 1 00 00010 01 hh ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_LSHIFT32_left,  "Rd = Rs << immag",                      "1100 M 110 1 00 00010 10 hh ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_LSHIFT32_right, "Rd = Rs >> newimmag",                   "1100 M 110 1 00 00010 10 hh ddd 1 iiiii rrr")
DEF_INSN(dsp32shiftimm_ROT32,          "Rd = ROT Rs BY immag",                  "1100 M 110 1 00 00010 11 hh ddd b iiiii rrr")
// sopcde=3: accumulator shifts (HLs selects A0 vs A1)
DEF_INSN(dsp32shiftimm_A0_ASHIFT_left, "A0 = A0 << immag",                      "1100 M 110 1 00 00011 00 00 ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_A0_ASHIFT_arith,"A0 = A0 >>> newimmag",                  "1100 M 110 1 00 00011 00 00 ddd 1 iiiii rrr")
DEF_INSN(dsp32shiftimm_A1_ASHIFT_left, "A1 = A1 << immag",                      "1100 M 110 1 00 00011 00 01 ddd 0 iiiii rrr")
DEF_INSN(dsp32shiftimm_A1_ASHIFT_arith,"A1 = A1 >>> newimmag",                  "1100 M 110 1 00 00011 00 01 ddd 1 iiiii rrr")
DEF_INSN(dsp32shiftimm_A0_LSHIFT_right,"A0 = A0 >> newimmag",                   "1100 M 110 1 00 00011 01 00 ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_A1_LSHIFT_right,"A1 = A1 >> newimmag",                   "1100 M 110 1 00 00011 01 01 ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_A0_ROT,         "A0 = ROT A0 BY immag",                  "1100 M 110 1 00 00011 10 00 ddd b iiiii rrr")
DEF_INSN(dsp32shiftimm_A1_ROT,         "A1 = ROT A1 BY immag",                  "1100 M 110 1 00 00011 10 01 ddd b iiiii rrr")
