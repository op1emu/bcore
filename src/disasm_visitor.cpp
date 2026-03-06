#include "disasm_visitor.h"
#include "utils/signextend.h"
#include <iostream>
#include <iomanip>
#include <stdexcept>

// Sign extend helper
static int32_t signext(uint32_t val, int bits) {
    uint32_t sign_bit = 1u << (bits - 1);
    if (val & sign_bit) {
        return static_cast<int32_t>(val | (~0u << bits));
    }
    return static_cast<int32_t>(val);
}

// Strip trailing newline characters from a string
static std::string strip_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

uint16_t DisasmVisitor::iftech(uint32_t addr) {
    if (addr < mem->base() || addr + 2 > mem->base() + mem->size()) {
        throw std::runtime_error("fetch out of bounds");
    }
    return mem->read16(addr);
}

void DisasmVisitor::on_parallel_begin() {
    _saved_out = _out;
    _parallel_slot = 0;
    _slot0.str(""); _slot0.clear();
    _slot1.str(""); _slot1.clear();
    _slot2.str(""); _slot2.clear();
    _out = &_slot0;
}

void DisasmVisitor::on_parallel_next_slot() {
    _parallel_slot++;
    _in_parallel_slot = true;
    _out = (_parallel_slot == 1) ? static_cast<std::ostream*>(&_slot1)
                                 : static_cast<std::ostream*>(&_slot2);
}

void DisasmVisitor::on_parallel_end() {
    _in_parallel_slot = false;
    _parallel_slot = 0;
    _out = _saved_out;
    *_out << strip_nl(_slot0.str()) << " || "
          << strip_nl(_slot1.str()) << " || "
          << strip_nl(_slot2.str()) << '\n';
}

void DisasmVisitor::on_parallel_abort() {
    _in_parallel_slot = false;
    _parallel_slot = 0;
    _out = _saved_out;
}

// Register name helpers
const char* DisasmVisitor::dregs(uint32_t r) {
    static const char* names[] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7"};
    return names[r & 7];
}

const char* DisasmVisitor::pregs(uint32_t r) {
    static const char* names[] = {"P0", "P1", "P2", "P3", "P4", "P5", "SP", "FP"};
    return names[r & 7];
}

const char* DisasmVisitor::iregs(uint32_t r) {
    static const char* names[] = {"I0", "I1", "I2", "I3"};
    return names[r & 3];
}

const char* DisasmVisitor::mregs(uint32_t r) {
    static const char* names[] = {"M0", "M1", "M2", "M3"};
    return names[r & 3];
}

const char* DisasmVisitor::statbits(uint32_t c) {
    static const char* names[] = {
        "az", "an", "ac0_copy", "v_copy",
        "astat[4 ]", "astat[5 ]", "aq", "astat[7 ]",
        "rnd_mod", "astat[9 ]", "astat[10 ]", "astat[11 ]",
        "ac0", "ac1", "astat[14 ]", "astat[15 ]",
        "av0", "av0s", "av1", "av1s",
        "astat[20 ]", "astat[21 ]", "astat[22 ]", "astat[23 ]",
        "v", "vs", "astat[26 ]", "astat[27 ]",
        "astat[28 ]", "astat[29 ]", "astat[30 ]", "astat[31 ]"
    };
    return names[c & 31];
}

const char* DisasmVisitor::allregs(uint32_t r, uint32_t g) {
    static const char* names[8][8] = {
        {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7"},
        {"P0", "P1", "P2", "P3", "P4", "P5", "SP", "FP"},
        {"I0", "I1", "I2", "I3", "M0", "M1", "M2", "M3"},
        {"B0", "B1", "B2", "B3", "L0", "L1", "L2", "L3"},
        {"A0.X", "A0.W", "A1.X", "A1.W", "??", "??", "ASTAT", "RETS"},
        {"??", "??", "??", "??", "??", "??", "??", "??"},
        {"LC0", "LT0", "LB0", "LC1", "LT1", "LB1", "CYCLES", "CYCLES2"},
        {"USP", "SEQSTAT", "SYSCFG", "RETI", "RETX", "RETN", "RETE", "EMUDAT"}
    };
    return names[g & 7][r & 7];
}

// regs_lo and regs_hi: used for DBGA half-register display.
// Emulates the reference bfin-elf-objdump's behavior where decode_regs_lo/hi
// are accessed without bounds checking. For "lo" pattern (dbgop=0):
//   indices 0-31: valid lo registers (dregs, pregs, iregs, mregs, bregs, lregs)
//   indices 32-63: overflow into decode_regs_hi (adjacent in binary)
// For "hi" pattern (dbgop=1):
//   indices 0-31: valid hi registers
//   indices 32+: overflow into unknown → LASTREG → returns nullptr (illegal)
static const char* regs_lo_lookup(uint32_t grp, uint32_t regtest) {
    // 64-entry table: [0-31]=lo registers, [32-63]=hi registers (overflow emulation)
    static const char* table[64] = {
        "r0.l","r1.l","r2.l","r3.l","r4.l","r5.l","r6.l","r7.l",   // dregs lo
        "p0.l","p1.l","p2.l","p3.l","p4.l","p5.l","sp.l","fp.l",   // pregs lo
        "i0.l","i1.l","i2.l","i3.l","m0.l","m1.l","m2.l","m3.l",   // iregs/mregs lo
        "b0.l","b1.l","b2.l","b3.l","l0.l","l1.l","l2.l","l3.l",   // bregs/lregs lo
        "r0.h","r1.h","r2.h","r3.h","r4.h","r5.h","r6.h","r7.h",   // overflow→dregs hi
        "p0.h","p1.h","p2.h","p3.h","p4.h","p5.h","sp.h","fp.h",   // overflow→pregs hi
        "i0.h","i1.h","i2.h","i3.h","m0.h","m1.h","m2.h","m3.h",   // overflow→iregs/mregs hi
        "b0.h","b1.h","b2.h","b3.h","l0.h","l1.h","l2.h","l3.h"    // overflow→bregs/lregs hi
    };
    uint32_t idx = ((grp << 3) | regtest) & 63;
    return table[idx];
}

static const char* regs_hi_lookup(uint32_t grp, uint32_t regtest) {
    static const char* table[32] = {
        "r0.h","r1.h","r2.h","r3.h","r4.h","r5.h","r6.h","r7.h",   // dregs hi
        "p0.h","p1.h","p2.h","p3.h","p4.h","p5.h","sp.h","fp.h",   // pregs hi
        "i0.h","i1.h","i2.h","i3.h","m0.h","m1.h","m2.h","m3.h",   // iregs/mregs hi
        "b0.h","b1.h","b2.h","b3.h","l0.h","l1.h","l2.h","l3.h",   // bregs/lregs hi
    };
    uint32_t idx = (grp << 3) | regtest;
    if (idx >= 32) return "...... Illegal register .......";  // overflow → LASTREG
    return table[idx];
}

const char* DisasmVisitor::dpregs(uint32_t r) {
    // R0-R7 (0-7), P0-P5, SP, FP (8-15)
    if (r < 8) return dregs(r);
    return pregs(r - 8);
}

// ProgCtrl instructions
bool DisasmVisitor::decode_ProgCtrl_NOP() {
    out() << "nop" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_RTS() {
    if (_in_parallel_slot) return false;
    out() << "rts" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_RTI() {
    if (_in_parallel_slot) return false;
    out() << "rti" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_RTX() {
    if (_in_parallel_slot) return false;
    out() << "rtx" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_RTN() {
    if (_in_parallel_slot) return false;
    out() << "rtn" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_RTE() {
    if (_in_parallel_slot) return false;
    out() << "rte" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_IDLE() {
    if (_in_parallel_slot) return false;
    out() << "idle" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_CSYNC() {
    if (_in_parallel_slot) return false;
    out() << "csync" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_SSYNC() {
    if (_in_parallel_slot) return false;
    out() << "ssync" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_EMUEXCPT() {
    if (_in_parallel_slot) return false;
    out() << "emuexcpt" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_CLI(uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "cli " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_STI(uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "sti " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_JUMP_PREG(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "jump (" << pregs(p) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_CALL_PREG(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "call (" << pregs(p) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_CALL_PC_PREG(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "call (pc + " << pregs(p) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_JUMP_PC_PREG(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "jump (pc + " << pregs(p) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_RAISE(uint16_t imm) {
    if (_in_parallel_slot) return false;
    out() << "raise 0x" << std::hex << imm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_EXCPT(uint16_t imm) {
    if (_in_parallel_slot) return false;
    out() << "excpt 0x" << std::hex << imm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_ProgCtrl_TESTSET(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "testset (" << pregs(p) << ")" << '\n';
    return true;
}

// PushPopReg instructions
bool DisasmVisitor::decode_PopReg(uint16_t g, uint16_t r) {
    if (_in_parallel_slot) return false;
    // mostreg: not dreg (g==0), not preg (g==1), not reserved
    bool is_reserved = (g == 4 && (r == 4 || r == 5)) || (g == 5);
    bool mostreg = !(g == 0 || g == 1 || is_reserved);
    if (!mostreg) return false;
    out() << allregs(r, g) << " = [SP++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_PushReg(uint16_t g, uint16_t r) {
    if (_in_parallel_slot) return false;
    // allreg: not reserved; also SP (g==1, r==6) cannot be pushed
    bool is_reserved = (g == 4 && (r == 4 || r == 5)) || (g == 5);
    if (is_reserved || (g == 1 && r == 6)) return false;
    out() << "[--SP] = " << allregs(r, g) << '\n';
    return true;
}

// PushPopMultiple instructions
bool DisasmVisitor::decode_PopMultiple_RP(uint16_t ddd, uint16_t ppp) {
    if (_in_parallel_slot) return false;
    if (ppp > 5) return false;
    out() << "(R7:" << ddd << ", P5:" << ppp << ") = [SP++]" << '\n';
    return true;
}
bool DisasmVisitor::decode_PopMultiple_R(uint16_t ddd) {
    if (_in_parallel_slot) return false;
    out() << "(R7:" << ddd << ") = [SP++]" << '\n';
    return true;
}
bool DisasmVisitor::decode_PopMultiple_P(uint16_t ppp) {
    if (_in_parallel_slot) return false;
    if (ppp > 5) return false;
    out() << "(P5:" << ppp << ") = [SP++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_PushMultiple_RP(uint16_t ddd, uint16_t ppp) {
    if (_in_parallel_slot) return false;
    if (ppp > 5) return false;
    out() << "[--SP] = (R7:" << ddd << ", P5:" << ppp << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_PushMultiple_R(uint16_t ddd) {
    if (_in_parallel_slot) return false;
    out() << "[--SP] = (R7:" << ddd << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_PushMultiple_P(uint16_t ppp) {
    if (_in_parallel_slot) return false;
    if (ppp > 5) return false;
    out() << "[--SP] = (P5:" << ppp << ")" << '\n';
    return true;
}

// CC2dreg instructions
bool DisasmVisitor::decode_CC2dreg_Read(uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << dregs(d) << " = cc" << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2dreg_Write(uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2dreg_Negate() {
    if (_in_parallel_slot) return false;
    out() << "cc = !cc" << '\n';
    return true;
}

// CaCTRL instructions
bool DisasmVisitor::decode_CaCTRL_PREFETCH(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "prefetch[" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_FLUSHINV(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "flushinv[" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_FLUSH(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "flush[" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_IFLUSH(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "iflush[" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_PREFETCH_pp(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "prefetch[" << pregs(p) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_FLUSHINV_pp(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "flushinv[" << pregs(p) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_FLUSH_pp(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "flush[" << pregs(p) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_CaCTRL_IFLUSH_pp(uint16_t p) {
    if (_in_parallel_slot) return false;
    out() << "iflush[" << pregs(p) << "++]" << '\n';
    return true;
}

// CC2stat instructions
bool DisasmVisitor::decode_CC2stat_CC_EQ_ASTAT(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << "cc = " << statbits(c) << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_CC_OR_ASTAT(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << "cc |= " << statbits(c) << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_CC_AND_ASTAT(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << "cc &= " << statbits(c) << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_CC_XOR_ASTAT(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << "cc ^= " << statbits(c) << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_ASTAT_EQ_CC(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << statbits(c) << " = cc" << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_ASTAT_OR_CC(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << statbits(c) << " |= cc" << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_ASTAT_AND_CC(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << statbits(c) << " &= cc" << '\n';
    return true;
}

bool DisasmVisitor::decode_CC2stat_ASTAT_XOR_CC(uint16_t c) {
    if (_in_parallel_slot) return false;
    if (c == 5) return false;
    out() << statbits(c) << " ^= cc" << '\n';
    return true;
}

// ccMV instructions
bool DisasmVisitor::decode_ccMV_IF_NOT(uint16_t d, uint16_t s, uint16_t dst, uint16_t src) {
    if (_in_parallel_slot) return false;
    out() << "if !cc " << (d ? pregs(dst) : dregs(dst)) << " = " << (s ? pregs(src) : dregs(src)) << '\n';
    return true;
}

bool DisasmVisitor::decode_ccMV_IF(uint16_t d, uint16_t s, uint16_t dst, uint16_t src) {
    if (_in_parallel_slot) return false;
    out() << "if cc " << (d ? pregs(dst) : dregs(dst)) << " = " << (s ? pregs(src) : dregs(src)) << '\n';
    return true;
}

// CCflag instructions
bool DisasmVisitor::decode_CCflag_EQ(bool preg, uint16_t y, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " == " << (preg ? pregs(y) : dregs(y)) << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LT(bool preg, uint16_t y, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " < " << (preg ? pregs(y) : dregs(y)) << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LE(bool preg, uint16_t y, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " <= " << (preg ? pregs(y) : dregs(y)) << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LT_U(bool preg, uint16_t y, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " < " << (preg ? pregs(y) : dregs(y)) << " (iu)" << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LE_U(bool preg, uint16_t y, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " <= " << (preg ? pregs(y) : dregs(y)) << " (iu)" << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_A0_EQ_A1() {
    if (_in_parallel_slot) return false;
    out() << "cc = a0 == a1" << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_A0_LT_A1() {
    if (_in_parallel_slot) return false;
    out() << "cc = a0 < a1" << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_A0_LE_A1() {
    if (_in_parallel_slot) return false;
    out() << "cc = A0 <= A1" << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_EQ_imm(bool preg, uint16_t i, uint16_t x) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(i, 3);
    if (simm < 0)
        out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " == -0x" << std::hex << -simm << std::dec << '\n';
    else
        out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " == 0x" << std::hex << simm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LT_imm(bool preg, uint16_t i, uint16_t x) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(i, 3);
    if (simm < 0)
        out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " < -0x" << std::hex << -simm << std::dec << '\n';
    else
        out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " < 0x" << std::hex << simm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LE_imm(bool preg, uint16_t i, uint16_t x) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(i, 3);
    if (simm < 0)
        out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " <= -0x" << std::hex << -simm << std::dec << '\n';
    else
        out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " <= 0x" << std::hex << simm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LT_U_imm(bool preg, uint16_t i, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " < 0x" << std::hex << i << std::dec << " (iu)" << '\n';
    return true;
}

bool DisasmVisitor::decode_CCflag_LE_U_imm(bool preg, uint16_t i, uint16_t x) {
    if (_in_parallel_slot) return false;
    out() << "cc = " << (preg ? pregs(x) : dregs(x)) << " <= 0x" << std::hex << i << std::dec << " (iu)" << '\n';
    return true;
}

// BRCC instructions
bool DisasmVisitor::decode_BRCC_BRT(uint16_t offset) {
    if (_in_parallel_slot) return false;
    int32_t soff = signext(offset, 10) * 2;
    out() << "if cc jump 0x" << std::hex << (current_pc + soff) << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_BRCC_BRT_BP(uint16_t offset) {
    if (_in_parallel_slot) return false;
    int32_t soff = signext(offset, 10) * 2;
    out() << "if cc jump 0x" << std::hex << (current_pc + soff) << std::dec << " (bp)" << '\n';
    return true;
}

bool DisasmVisitor::decode_BRCC_BRF(uint16_t offset) {
    if (_in_parallel_slot) return false;
    int32_t soff = signext(offset, 10) * 2;
    out() << "if !cc jump 0x" << std::hex << (current_pc + soff) << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_BRCC_BRF_BP(uint16_t offset) {
    if (_in_parallel_slot) return false;
    int32_t soff = signext(offset, 10) * 2;
    out() << "if !cc jump 0x" << std::hex << (current_pc + soff) << std::dec << " (bp)" << '\n';
    return true;
}

// UJUMP instructions
bool DisasmVisitor::decode_UJUMP(uint16_t offset) {
    if (_in_parallel_slot) return false;
    int32_t soff = signext(offset, 12) * 2;
    out() << "jump.s 0x" << std::hex << (current_pc + soff) << std::dec << '\n';
    return true;
}

// REGMV instructions
bool DisasmVisitor::decode_REGMV(uint16_t gd, uint16_t gs, uint16_t d, uint16_t s) {
    // Reserved slots are always illegal
    auto is_reserved = [](uint32_t g, uint32_t r) {
        return (g == 4 && (r == 4 || r == 5)) || (g == 5);
    };
    if (is_reserved(gs, s) || is_reserved(gd, d)) return false;

    // Valid combinations (matching reference bfin-dis.c)
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
    out() << allregs(d, gd) << " = " << allregs(s, gs) << '\n';
    return true;
}

// ALU2op instructions
bool DisasmVisitor::decode_ALU2op_ASHIFT_RIGHT(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " >>>= " << dregs(src) << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_LSHIFT_RIGHT(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " >>= " << dregs(src) << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_LSHIFT_LEFT(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " <<= " << dregs(src) << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_MUL(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " *= " << dregs(src) << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_ADD_SHIFT1(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = (" << dregs(dst) << " + " << dregs(src) << ") << 0x1" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_ADD_SHIFT2(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = (" << dregs(dst) << " + " << dregs(src) << ") << 0x2" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_DIVQ(uint16_t src, uint16_t dst) {
    out() << "divq (" << dregs(dst) << ", " << dregs(src) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_DIVS(uint16_t src, uint16_t dst) {
    out() << "divs (" << dregs(dst) << ", " << dregs(src) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_SEXT_L(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = " << dregs(src) << ".l (x)" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_ZEXT_L(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = " << dregs(src) << ".l (z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_SEXT_B(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = " << dregs(src) << ".b (x)" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_ZEXT_B(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = " << dregs(src) << ".b (z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_NEG(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " = -" << dregs(src) << '\n';
    return true;
}

bool DisasmVisitor::decode_ALU2op_NOT(uint16_t src, uint16_t dst) {
    out() << dregs(dst) << " =~ " << dregs(src) << '\n';
    return true;
}

// PTR2op instructions
bool DisasmVisitor::decode_PTR2op_SUB(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " -= " << pregs(src) << '\n';
    return true;
}

bool DisasmVisitor::decode_PTR2op_LSHIFT_LEFT2(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " = " << pregs(src) << " << 0x2" << '\n';
    return true;
}

bool DisasmVisitor::decode_PTR2op_LSHIFT_RIGHT2(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " = " << pregs(src) << " >> 0x2" << '\n';
    return true;
}

bool DisasmVisitor::decode_PTR2op_LSHIFT_RIGHT1(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " = " << pregs(src) << " >> 0x1" << '\n';
    return true;
}

bool DisasmVisitor::decode_PTR2op_ADD_BREV(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " += " << pregs(src) << " (BREV)" << '\n';
    return true;
}

bool DisasmVisitor::decode_PTR2op_ADD_SHIFT1(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " = (" << pregs(dst) << " + " << pregs(src) << ") << 0x1" << '\n';
    return true;
}

bool DisasmVisitor::decode_PTR2op_ADD_SHIFT2(uint16_t src, uint16_t dst) {
    out() << pregs(dst) << " = (" << pregs(dst) << " + " << pregs(src) << ") << 0x2" << '\n';
    return true;
}

// LOGI2op instructions
bool DisasmVisitor::decode_LOGI2op_CC_EQ_AND(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "cc = !bittst (" << dregs(d) << ", 0x" << std::hex << imm << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_CC_EQ_BITTST(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "cc = bittst (" << dregs(d) << ", 0x" << std::hex << imm << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_BITSET(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "bitset (" << dregs(d) << ", 0x" << std::hex << imm << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_BITTGL(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "bittgl (" << dregs(d) << ", 0x" << std::hex << imm << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_BITCLR(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << "bitclr (" << dregs(d) << ", 0x" << std::hex << imm << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_ASHIFT_RIGHT(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << dregs(d) << " >>>= 0x" << std::hex << imm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_LSHIFT_RIGHT(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << dregs(d) << " >>= 0x" << std::hex << imm << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_LOGI2op_LSHIFT_LEFT(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    out() << dregs(d) << " <<= 0x" << std::hex << imm << std::dec << '\n';
    return true;
}

// COMP3op instructions
bool DisasmVisitor::decode_COMP3op_ADD(uint16_t dst, uint16_t t, uint16_t s) {
    out() << dregs(dst) << " = " << dregs(s) << " + " << dregs(t) << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_SUB(uint16_t dst, uint16_t t, uint16_t s) {
    out() << dregs(dst) << " = " << dregs(s) << " - " << dregs(t) << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_AND(uint16_t dst, uint16_t t, uint16_t s) {
    out() << dregs(dst) << " = " << dregs(s) << " & " << dregs(t) << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_OR(uint16_t dst, uint16_t t, uint16_t s) {
    out() << dregs(dst) << " = " << dregs(s) << " | " << dregs(t) << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_XOR(uint16_t dst, uint16_t t, uint16_t s) {
    out() << dregs(dst) << " = " << dregs(s) << " ^ " << dregs(t) << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_PADD(uint16_t dst, uint16_t t, uint16_t s) {
    if (s == t)
        out() << pregs(dst) << " = " << pregs(s) << " << 0x1" << '\n';
    else
        out() << pregs(dst) << " = " << pregs(s) << " + " << pregs(t) << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_LSHIFT(uint16_t dst, uint16_t t, uint16_t s) {
    out() << pregs(dst) << " = " << pregs(s) << " + (" << pregs(t) << " << 0x1)" << '\n';
    return true;
}

bool DisasmVisitor::decode_COMP3op_LSHIFT2(uint16_t dst, uint16_t t, uint16_t s) {
    out() << pregs(dst) << " = " << pregs(s) << " + (" << pregs(t) << " << 0x2)" << '\n';
    return true;
}

// COMPI2opD instructions
bool DisasmVisitor::decode_COMPI2opD_EQ(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(imm, 7);
    if (simm < 0)
        out() << dregs(d) << " = -0x" << std::hex << -simm << std::dec << " (x)" << '\n';
    else
        out() << dregs(d) << " = 0x" << std::hex << simm << std::dec << " (x)" << '\n';
    return true;
}

bool DisasmVisitor::decode_COMPI2opD_ADD(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(imm, 7);
    if (simm < 0)
        out() << dregs(d) << " += -0x" << std::hex << -simm << std::dec << '\n';
    else
        out() << dregs(d) << " += 0x" << std::hex << simm << std::dec << '\n';
    return true;
}

// COMPI2opP instructions
bool DisasmVisitor::decode_COMPI2opP_EQ(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(imm, 7);
    if (simm < 0)
        out() << pregs(d) << " = -0x" << std::hex << -simm << std::dec << " (x)" << '\n';
    else
        out() << pregs(d) << " = 0x" << std::hex << simm << std::dec << " (x)" << '\n';
    return true;
}

bool DisasmVisitor::decode_COMPI2opP_ADD(uint16_t imm, uint16_t d) {
    if (_in_parallel_slot) return false;
    int32_t simm = signext(imm, 7);
    if (simm < 0)
        out() << pregs(d) << " += -0x" << std::hex << -simm << std::dec << '\n';
    else
        out() << pregs(d) << " += 0x" << std::hex << simm << std::dec << '\n';
    return true;
}


// Forward declarations for helper functions used in LDSTpmod
static const char* dregs_lo_str(int r);
static const char* dregs_hi_str(int r);

// LDSTpmod instructions
// aop=0: 32-bit [ptr++idx]
bool DisasmVisitor::decode_LDSTpmod_LD_32(uint16_t d, uint16_t idx, uint16_t ptr) {
    out() << dregs(d) << " = [" << pregs(ptr) << " ++ " << pregs(idx) << "]" << '\n';
    return true;
}
// aop=1: lo half; if idx==ptr: W[ptr], else W[ptr++idx]
bool DisasmVisitor::decode_LDSTpmod_LD_16_lo(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (idx == ptr)
        out() << dregs_lo_str(d) << " = W[" << pregs(ptr) << "]" << '\n';
    else
        out() << dregs_lo_str(d) << " = W[" << pregs(ptr) << " ++ " << pregs(idx) << "]" << '\n';
    return true;
}
// aop=2: hi half; if idx==ptr: W[ptr], else W[ptr++idx]
bool DisasmVisitor::decode_LDSTpmod_LD_16_hi(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (idx == ptr)
        out() << dregs_hi_str(d) << " = W[" << pregs(ptr) << "]" << '\n';
    else
        out() << dregs_hi_str(d) << " = W[" << pregs(ptr) << " ++ " << pregs(idx) << "]" << '\n';
    return true;
}
// aop=3, W=0: (Z) sign-extend
bool DisasmVisitor::decode_LDSTpmod_LD_16_Z(uint16_t d, uint16_t idx, uint16_t ptr) {
    out() << dregs(d) << " = W[" << pregs(ptr) << " ++ " << pregs(idx) << "] (Z)" << '\n';
    return true;
}
// aop=3, W=1: (X) zero-extend
bool DisasmVisitor::decode_LDSTpmod_LD_16_X(uint16_t d, uint16_t idx, uint16_t ptr) {
    out() << dregs(d) << " = W[" << pregs(ptr) << " ++ " << pregs(idx) << "] (X)" << '\n';
    return true;
}
// aop=0: 32-bit store
bool DisasmVisitor::decode_LDSTpmod_ST_32(uint16_t d, uint16_t idx, uint16_t ptr) {
    out() << "[" << pregs(ptr) << " ++ " << pregs(idx) << "] = " << dregs(d) << '\n';
    return true;
}
// aop=1, W=1: store lo half; if idx==ptr: W[ptr], else W[ptr++idx]
bool DisasmVisitor::decode_LDSTpmod_ST_16_lo(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (idx == ptr)
        out() << "W[" << pregs(ptr) << "] = " << dregs_lo_str(d) << '\n';
    else
        out() << "W[" << pregs(ptr) << " ++ " << pregs(idx) << "] = " << dregs_lo_str(d) << '\n';
    return true;
}
// aop=2, W=1: store hi half; if idx==ptr: W[ptr], else W[ptr++idx]
bool DisasmVisitor::decode_LDSTpmod_ST_16_hi(uint16_t d, uint16_t idx, uint16_t ptr) {
    if (idx == ptr)
        out() << "W[" << pregs(ptr) << "] = " << dregs_hi_str(d) << '\n';
    else
        out() << "W[" << pregs(ptr) << " ++ " << pregs(idx) << "] = " << dregs_hi_str(d) << '\n';
    return true;
}

// LDST instructions
bool DisasmVisitor::decode_LDST_LD_32(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = [" << pregs(p) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_32_mm(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = [" << pregs(p) << "--]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_32_ind(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = [" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_32(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "++] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_32_mm(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "--] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_32_ind(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_16_Z(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << "++] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_16_Z_mm(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << "--] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_16_Z_ind(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << "] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_16(uint16_t p, uint16_t d) {
    out() << "W[" << pregs(p) << "++] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_16_mm(uint16_t p, uint16_t d) {
    out() << "W[" << pregs(p) << "--] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_16_ind(uint16_t p, uint16_t d) {
    out() << "W[" << pregs(p) << "] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_16_X(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << "++] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_16_X_mm(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << "--] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_16_X_ind(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << "] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_8_Z(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << "++] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_8_Z_mm(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << "--] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_8_Z_ind(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << "] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_8(uint16_t p, uint16_t d) {
    out() << "B[" << pregs(p) << "++] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_8_mm(uint16_t p, uint16_t d) {
    out() << "B[" << pregs(p) << "--] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_8_ind(uint16_t p, uint16_t d) {
    out() << "B[" << pregs(p) << "] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_8_X(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << "++] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_8_X_mm(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << "--] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_8_X_ind(uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << "] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_P_32(uint16_t p, uint16_t d) {
    out() << pregs(d) << " = [" << pregs(p) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_P_32_mm(uint16_t p, uint16_t d) {
    out() << pregs(d) << " = [" << pregs(p) << "--]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_P_32_ind(uint16_t p, uint16_t d) {
    out() << pregs(d) << " = [" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_P_32(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "++] = " << pregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_P_32_mm(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "--] = " << pregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_P_32_ind(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "] = " << pregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_P_32_z(uint16_t p, uint16_t d) {
    if (p == d) { return false; }
    out() << pregs(d) << " = [" << pregs(p) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_P_32_z_mm(uint16_t p, uint16_t d) {
    if (p == d) { return false; }
    out() << pregs(d) << " = [" << pregs(p) << "--]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_LD_P_32_z_ind(uint16_t p, uint16_t d) {
    out() << pregs(d) << " = [" << pregs(p) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_P_32_z(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "++] = " << pregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_P_32_z_mm(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "--] = " << pregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDST_ST_P_32_z_ind(uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << "] = " << pregs(d) << '\n';
    return true;
}

// dspLDST instructions
// Forward declarations (defined later in file with dsp32 helpers)
static const char* dregs_lo_str(int r);
static const char* dregs_hi_str(int r);
bool DisasmVisitor::decode_dspLDST_LD_dreg(uint16_t i, uint16_t d) {
    out() << dregs(d) << " = [" << iregs(i) << "++]" << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_LD_dreg_lo(uint16_t i, uint16_t d) {
    out() << dregs_lo_str(d) << " = W[" << iregs(i) << "++]" << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_LD_dreg_hi(uint16_t i, uint16_t d) {
    out() << dregs_hi_str(d) << " = W[" << iregs(i) << "++]" << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_LD_dreg_mm(uint16_t i, uint16_t d) {
    out() << dregs(d) << " = [" << iregs(i) << "--]" << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_LD_dreg_mm_lo(uint16_t i, uint16_t d) {
    out() << dregs_lo_str(d) << " = W[" << iregs(i) << "--]" << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_LD_dreg_mm_hi(uint16_t i, uint16_t d) {
    out() << dregs_hi_str(d) << " = W[" << iregs(i) << "--]" << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_LD_dreg_Mmod(uint16_t i, uint16_t d) {
    out() << dregs(d) << " = [" << iregs(i) << "]" << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_LD_dreg_Mmod_lo(uint16_t i, uint16_t d) {
    out() << dregs_lo_str(d) << " = W[" << iregs(i) << "]" << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_LD_dreg_Mmod_hi(uint16_t i, uint16_t d) {
    out() << dregs_hi_str(d) << " = W[" << iregs(i) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_LD_dreg_brev(uint16_t m, uint16_t i, uint16_t d) {
    // aop=3: Rd = [Ii ++ Mm] (post-modify by Mm; m is M-register index)
    out() << dregs(d) << " = [" << iregs(i) << " ++ " << mregs(m) << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_ST_dreg(uint16_t i, uint16_t d) {
    out() << "[" << iregs(i) << "++] = " << dregs(d) << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_ST_dreg_lo(uint16_t i, uint16_t d) {
    out() << "W[" << iregs(i) << "++] = " << dregs_lo_str(d) << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_ST_dreg_hi(uint16_t i, uint16_t d) {
    out() << "W[" << iregs(i) << "++] = " << dregs_hi_str(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_ST_dreg_mm(uint16_t i, uint16_t d) {
    out() << "[" << iregs(i) << "--] = " << dregs(d) << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_ST_dreg_mm_lo(uint16_t i, uint16_t d) {
    out() << "W[" << iregs(i) << "--] = " << dregs_lo_str(d) << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_ST_dreg_mm_hi(uint16_t i, uint16_t d) {
    out() << "W[" << iregs(i) << "--] = " << dregs_hi_str(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_ST_dreg_Mmod(uint16_t i, uint16_t d) {
    out() << "[" << iregs(i) << "] = " << dregs(d) << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_ST_dreg_Mmod_lo(uint16_t i, uint16_t d) {
    out() << "W[" << iregs(i) << "] = " << dregs_lo_str(d) << '\n';
    return true;
}
bool DisasmVisitor::decode_dspLDST_ST_dreg_Mmod_hi(uint16_t i, uint16_t d) {
    out() << "W[" << iregs(i) << "] = " << dregs_hi_str(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_dspLDST_ST_dreg_brev(uint16_t m, uint16_t i, uint16_t d) {
    // aop=3: [Ii ++ Mm] = Rd
    out() << "[" << iregs(i) << " ++ " << mregs(m) << "] = " << dregs(d) << '\n';
    return true;
}

// LDSTii instructions
bool DisasmVisitor::decode_LDSTii_LD_32(uint16_t offset, uint16_t p, uint16_t d) {
    out() << dregs(d) << " = [" << pregs(p) << " + 0x" << std::hex << (offset * 4) << std::dec << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_LD_16_Z(uint16_t offset, uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << " + 0x" << std::hex << (offset * 2) << std::dec << "] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_LD_16_X(uint16_t offset, uint16_t p, uint16_t d) {
    out() << dregs(d) << " = W[" << pregs(p) << " + 0x" << std::hex << (offset * 2) << std::dec << "] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_LD_P_32(uint16_t offset, uint16_t p, uint16_t d) {
    out() << pregs(d) << " = [" << pregs(p) << " + 0x" << std::hex << (offset * 4) << std::dec << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_ST_32(uint16_t offset, uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << " + 0x" << std::hex << (offset * 4) << std::dec << "] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_ST_16(uint16_t offset, uint16_t p, uint16_t d) {
    out() << "W[" << pregs(p) << " + 0x" << std::hex << (offset * 2) << std::dec << "] = " << dregs(d) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_LD_8_Z(uint16_t offset, uint16_t p, uint16_t d) {
    out() << dregs(d) << " = B[" << pregs(p) << " + 0x" << std::hex << offset << std::dec << "] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTii_ST_P_32(uint16_t offset, uint16_t p, uint16_t d) {
    out() << "[" << pregs(p) << " + 0x" << std::hex << (offset * 4) << std::dec << "] = " << pregs(d) << '\n';
    return true;
}

// LDSTiiFP instructions
bool DisasmVisitor::decode_LDSTiiFP_LD_32(uint16_t offset, uint16_t reg) {
    // negimm5s4: set bit 5, sign extend from 6 bits, scale by 4
    int32_t signed_offset = offset | (1 << 5);  // Set bit 5
    signed_offset = (signed_offset << 26) >> 26;  // Sign extend from 6 bits
    int32_t final_offset = signed_offset * 4;  // Scale by 4

    out() << dpregs(reg) << " = [fp ";
    if (final_offset < 0)
        out() << "-0x" << std::hex << (-final_offset) << std::dec;
    else
        out() << "+0x" << std::hex << final_offset << std::dec;
    out() << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTiiFP_ST_32(uint16_t offset, uint16_t reg) {
    // negimm5s4: set bit 5, sign extend from 6 bits, scale by 4
    int32_t signed_offset = offset | (1 << 5);  // Set bit 5
    signed_offset = (signed_offset << 26) >> 26;  // Sign extend from 6 bits
    int32_t final_offset = signed_offset * 4;  // Scale by 4

    out() << "[fp ";
    if (final_offset < 0)
        out() << "-0x" << std::hex << (-final_offset) << std::dec;
    else
        out() << "+0x" << std::hex << final_offset << std::dec;
    out() << "] = " << dpregs(reg) << '\n';
    return true;
}

// dagMODim instructions
bool DisasmVisitor::decode_dagMODim_ADD(uint16_t m, uint16_t i) {
    out() << iregs(i) << " += " << mregs(m) << '\n';
    return true;
}

bool DisasmVisitor::decode_dagMODim_SUB(uint16_t m, uint16_t i) {
    out() << iregs(i) << " -= " << mregs(m) << '\n';
    return true;
}

bool DisasmVisitor::decode_dagMODim_ADD_BREV(uint16_t m, uint16_t i) {
    out() << iregs(i) << " += " << mregs(m) << " (BREV)" << '\n';
    return true;
}

// dagMODik instructions
bool DisasmVisitor::decode_dagMODik_ADD2(uint16_t i) {
    out() << iregs(i) << " += 0x2" << '\n';
    return true;
}

bool DisasmVisitor::decode_dagMODik_SUB2(uint16_t i) {
    out() << iregs(i) << " -= 0x2" << '\n';
    return true;
}

bool DisasmVisitor::decode_dagMODik_ADD4(uint16_t i) {
    out() << iregs(i) << " += 0x4" << '\n';
    return true;
}

bool DisasmVisitor::decode_dagMODik_SUB4(uint16_t i) {
    out() << iregs(i) << " -= 0x4" << '\n';
    return true;
}

// pseudoDEBUG instructions
bool DisasmVisitor::decode_pseudoDEBUG_DBG_reg(uint16_t g, uint16_t r) {
    if (_in_parallel_slot) return false;
    out() << "dbg " << allregs(r, g) << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_PRNT_reg(uint16_t g, uint16_t r) {
    if (_in_parallel_slot) return false;
    out() << "prnt " << allregs(r, g) << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_OUTC_dreg(uint16_t r) {
    if (_in_parallel_slot) return false;
    out() << "outc " << dregs(r) << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_DBG_A0(uint16_t g) {
    if (_in_parallel_slot) return false;
    (void)g;
    out() << "dbg A0" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_DBG_A1(uint16_t g) {
    if (_in_parallel_slot) return false;
    (void)g;
    out() << "dbg A1" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_ABORT(uint16_t g) {
    if (_in_parallel_slot) return false;
    (void)g;
    out() << "abort" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_HLT(uint16_t g) {
    if (_in_parallel_slot) return false;
    (void)g;
    out() << "hlt" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_DBGHALT(uint16_t g) {
    if (_in_parallel_slot) return false;
    (void)g;
    out() << "dbghalt" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_DBGCMPLX(uint16_t g) {
    if (_in_parallel_slot) return false;
    out() << "dbgcmplx (" << dregs(g) << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDEBUG_DBG(uint16_t g) {
    if (_in_parallel_slot) return false;
    (void)g;
    out() << "dbg" << '\n';
    return true;
}

// pseudoChr instructions
bool DisasmVisitor::decode_pseudoChr_OUTC(uint16_t ch) {
    out() << "outc 0x" << std::hex << ch << std::dec << '\n';
    return true;
}

// 32-bit instructions

// LoopSetup instructions
bool DisasmVisitor::decode_LoopSetup_LC0(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc0" << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC0_P(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc0 = " << pregs(reg) << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC1(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc1" << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC1_P(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc1 = " << pregs(reg) << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC0_half(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc0 = " << pregs(reg) << " >> 0x1" << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC1_half(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc1 = " << pregs(reg) << " >> 0x1" << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC0_P_half(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc0 = " << pregs(reg) << " >> 0x1" << '\n';
    return true;
}

bool DisasmVisitor::decode_LoopSetup_LC1_P_half(uint32_t soffset, uint32_t reg, uint32_t eoffset) {
    if (reg > 7) return false;
    out() << "lsetup(0x" << std::hex << (current_pc + signextend<4>(soffset) * 2) << ", 0x" << (current_pc + signextend<10>(eoffset) * 2) << std::dec << ") lc1 = " << pregs(reg) << " >> 0x1" << '\n';
    return true;
}

// LDIMMhalf instructions
bool DisasmVisitor::decode_LDIMMhalf_low(uint32_t g, uint32_t r, uint16_t hword) {
    out() << allregs(r, g) << ".l = 0x" << std::hex << hword << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_LDIMMhalf_high(uint32_t g, uint32_t r, uint16_t hword) {
    out() << allregs(r, g) << ".h = 0x" << std::hex << hword << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_LDIMMhalf_full(uint32_t g, uint32_t r, uint16_t hword) {
    out() << allregs(r, g) << " = 0x" << std::hex << hword << std::dec << " (z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDIMMhalf_full_sext(uint32_t g, uint32_t r, uint16_t hword) {
    int32_t simm = signext(hword, 16);
    if (simm < 0)
        out() << allregs(r, g) << " = -0x" << std::hex << -simm << std::dec << " (x)" << '\n';
    else
        out() << allregs(r, g) << " = 0x" << std::hex << simm << std::dec << " (x)" << '\n';
    return true;
}

// CALLa instructions
bool DisasmVisitor::decode_CALLa_CALL(uint32_t addr) {
    int32_t saddr = signext(addr, 24) * 2;
    out() << "call 0x" << std::hex << (current_pc + saddr) << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_CALLa_JUMP(uint32_t addr) {
    int32_t saddr = signext(addr, 24) * 2;
    out() << "jump.l 0x" << std::hex << (current_pc + saddr) << std::dec << '\n';
    return true;
}

// LDSTidxI instructions
// Offsets are element-scaled: sz=00 (32-bit) *4, sz=01 (16-bit) *2, sz=10 (byte) *1
// Negative offsets print as "+ -0x<val>" to match reference disassembler format
bool DisasmVisitor::decode_LDSTidxI_LD_32(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 4;
    if (soff < 0)
        out() << dregs(r) << " = [" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "]" << '\n';
    else
        out() << dregs(r) << " = [" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_LD_16_Z(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 2;
    if (soff < 0)
        out() << dregs(r) << " = W[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] (Z)" << '\n';
    else
        out() << dregs(r) << " = W[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_LD_16_X(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 2;
    if (soff < 0)
        out() << dregs(r) << " = W[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] (X)" << '\n';
    else
        out() << dregs(r) << " = W[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_LD_B_Z(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16);
    if (soff < 0)
        out() << dregs(r) << " = B[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] (Z)" << '\n';
    else
        out() << dregs(r) << " = B[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] (Z)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_LD_B_X(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16);
    if (soff < 0)
        out() << dregs(r) << " = B[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] (X)" << '\n';
    else
        out() << dregs(r) << " = B[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] (X)" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_ST_32(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 4;
    if (soff < 0)
        out() << "[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] = " << dregs(r) << '\n';
    else
        out() << "[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] = " << dregs(r) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_ST_16(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 2;
    if (soff < 0)
        out() << "W[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] = " << dregs(r) << '\n';
    else
        out() << "W[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] = " << dregs(r) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_ST_B(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16);
    if (soff < 0)
        out() << "B[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] = " << dregs(r) << '\n';
    else
        out() << "B[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] = " << dregs(r) << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_LD_P_32(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 4;
    if (soff < 0)
        out() << pregs(r) << " = [" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "]" << '\n';
    else
        out() << pregs(r) << " = [" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "]" << '\n';
    return true;
}

bool DisasmVisitor::decode_LDSTidxI_ST_P_32(uint32_t p, uint32_t r, uint32_t offset) {
    int32_t soff = signext(offset, 16) * 4;
    if (soff < 0)
        out() << "[" << pregs(p) << " + -0x" << std::hex << -soff << std::dec << "] = " << pregs(r) << '\n';
    else
        out() << "[" << pregs(p) << " + 0x" << std::hex << soff << std::dec << "] = " << pregs(r) << '\n';
    return true;
}

// Linkage instructions
bool DisasmVisitor::decode_Linkage_LINK(uint32_t framesize) {
    out() << "link 0x" << std::hex << (framesize * 4) << std::dec << '\n';
    return true;
}

bool DisasmVisitor::decode_Linkage_UNLINK() {
    out() << "unlink" << '\n';
    return true;
}

// Constants matching refs/opcode/bfin.h
static const int M_S2RND = 1;
static const int M_T     = 2;
static const int M_W32   = 3;
static const int M_FU    = 4;
static const int M_TFU   = 6;
static const int M_IS    = 8;
static const int M_ISS2  = 9;
static const int M_IH    = 11;
static const int M_IU    = 12;

static const char* dregs_lo_str(int r) {
    static const char* names[] = { "r0.l", "r1.l", "r2.l", "r3.l", "r4.l", "r5.l", "r6.l", "r7.l" };
    return (r >= 0 && r < 8) ? names[r] : "?";
}

static const char* dregs_hi_str(int r) {
    static const char* names[] = { "r0.h", "r1.h", "r2.h", "r3.h", "r4.h", "r5.h", "r6.h", "r7.h" };
    return (r >= 0 && r < 8) ? names[r] : "?";
}

static const char* dregs_str(int r) {
    static const char* names[] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7" };
    return (r >= 0 && r < 8) ? names[r] : "?";
}

// For "range" notation (e.g., r0:7), the start register wraps: (src+1)&7
static const char* dregs_range_start(int r) {
    return dregs_str(r & 7);
}

static void decode_multfunc_impl(int h0, int h1, int src0, int src1, std::ostream& os) {
    os << (h0 ? dregs_hi_str(src0) : dregs_lo_str(src0))
       << " * "
       << (h1 ? dregs_hi_str(src1) : dregs_lo_str(src1));
}

static void decode_macfunc_impl(int which, int op, int h0, int h1, int src0, int src1, std::ostream& os) {
    const char* a = which ? "A1" : "A0";
    if (op == 3) {
        os << a;
        return;
    }
    const char* sop = (op == 0) ? " = " : (op == 1) ? " += " : " -= ";
    os << a << sop;
    decode_multfunc_impl(h0, h1, src0, src1, os);
}

static void decode_optmode_impl(int mod, int MM, std::ostream& os) {
    if (mod == 0 && MM == 0) return;
    os << " (";
    if (MM && !mod) { os << "M)"; return; }
    if (MM) os << "M, ";
    if      (mod == M_S2RND) os << "S2RND";
    else if (mod == M_T)     os << "T";
    else if (mod == M_W32)   os << "W32";
    else if (mod == M_FU)    os << "FU";
    else if (mod == M_TFU)   os << "TFU";
    else if (mod == M_IS)    os << "IS";
    else if (mod == M_ISS2)  os << "ISS2";
    else if (mod == M_IH)    os << "IH";
    else if (mod == M_IU)    os << "IU";
    os << ")";
}

// ---- dsp32mac per-variant handlers ----
// Field mapping from pattern "1100 M 00 mmmm j P w1 oo aa w0 kk ee ddd fff ggg":
//   mmod=mmmm(4), MM=j(1), op1=oo(2), h01h11=aa(2), op0=kk(2), h00h10=ee(2)
//   P and w1 and w0 are fixed as literal bits per variant.
//
// Helper macro for h-field extraction used across all mac/mult variants:
//   h01 = (h01h11>>1)&1, h11 = h01h11&1
//   h00 = (h00h10>>1)&1, h10 = h00h10&1

bool DisasmVisitor::decode_dsp32mac_MNOP(uint32_t M) {
    out() << "mnop\n"; return true;
}

// Shared mac output helpers (static, file-scope)
static void mac1_output(bool P, bool w1, uint32_t op1,
    uint32_t h01, uint32_t h11, uint32_t dst, uint32_t src0, uint32_t src1, std::ostream& os) {
    if (w1)
        os << (P ? dregs_range_start(dst + 1) : dregs_hi_str(dst));
    if (op1 == 3)
        os << " = A1";
    else {
        if (w1) os << " = (";
        decode_macfunc_impl(1, op1, h01, h11, src0, src1, os);
        if (w1) os << ")";
    }
}

static void mac0_output(bool P, bool w0, uint32_t op0,
    uint32_t h00, uint32_t h10, uint32_t dst, uint32_t src0, uint32_t src1, std::ostream& os) {
    if (w0)
        os << (P ? dregs_str(dst) : dregs_lo_str(dst));
    if (op0 == 3)
        os << " = A0";
    else {
        if (w0) os << " = (";
        decode_macfunc_impl(0, op0, h00, h10, src0, src1, os);
        if (w0) os << ")";
    }
}

// P=0, w1=0, w0=0: both accumulators only (no register writes)
bool DisasmVisitor::decode_dsp32mac_P0_nn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (op1 == 3 && op0 == 3) return false;  // nothing to do (handled by MNOP or illegal)
    if (((1 << mmod) & 0x1b5f) == 0) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    // mac1 side: only active when op1 != 3 (w1=0, so condition is just op1!=3)
    bool mac1_active = (op1 != 3);
    bool mac0_active = (op0 != 3);
    if (mac1_active) {
        mac1_output(false, false, op1, h01, h11, dst, src0, src1, out());
        if (mac0_active) {
            if (MM) out() << " (M)";
            out() << ", ";
        }
    }
    if (mac0_active) {
        int mm0 = mac1_active ? 0 : MM;
        mac0_output(false, false, op0, h00, h10, dst, src0, src1, out());
        decode_optmode_impl(mmod, mm0, out());
    } else {
        decode_optmode_impl(mmod, MM, out());
    }
    out() << '\n'; return true;
}

// P=0, w1=1, w0=0: Rn.h = (mac1), A0 op
bool DisasmVisitor::decode_dsp32mac_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if ((mmod == M_W32) || (((1 << mmod) & 0x1b5f) == 0)) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    bool mac0_active = (op0 != 3);
    mac1_output(false, true, op1, h01, h11, dst, src0, src1, out());
    if (mac0_active) {
        if (MM) out() << " (M)";
        out() << ", ";
        mac0_output(false, false, op0, h00, h10, dst, src0, src1, out());
        decode_optmode_impl(mmod, 0, out());
    } else {
        decode_optmode_impl(mmod, MM, out());
    }
    out() << '\n'; return true;
}

// P=0, w1=0, w0=1: A1 op, Rn.l = (mac0)
bool DisasmVisitor::decode_dsp32mac_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if ((mmod == M_W32) || (((1 << mmod) & 0x1b5f) == 0)) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    bool mac1_active = (op1 != 3);
    if (mac1_active) {
        mac1_output(false, false, op1, h01, h11, dst, src0, src1, out());
        if (MM) out() << " (M)";
        out() << ", ";
    }
    mac0_output(false, true, op0, h00, h10, dst, src0, src1, out());
    decode_optmode_impl(mmod, 0, out());
    out() << '\n'; return true;
}

// P=0, w1=1, w0=1: Rn.h = (mac1), Rn.l = (mac0)
bool DisasmVisitor::decode_dsp32mac_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if ((mmod == M_W32) || (((1 << mmod) & 0x1b5f) == 0)) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    mac1_output(false, true, op1, h01, h11, dst, src0, src1, out());
    if (MM) out() << " (M)";
    out() << ", ";
    mac0_output(false, true, op0, h00, h10, dst, src0, src1, out());
    decode_optmode_impl(mmod, 0, out());
    out() << '\n'; return true;
}

// P=1, w1=0, w0=0: both accumulators only (no register writes)
bool DisasmVisitor::decode_dsp32mac_P1_nn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if (op1 == 3 && op0 == 3) return false;
    if (((1 << mmod) & 0x131b) == 0) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    // mac1 side: only active when op1 != 3 (w1=0, so condition is just op1!=3)
    bool mac1_active = (op1 != 3);
    bool mac0_active = (op0 != 3);
    if (mac1_active) {
        mac1_output(true, false, op1, h01, h11, dst, src0, src1, out());
        if (mac0_active) {
            if (MM) out() << " (M)";
            out() << ", ";
        }
    }
    if (mac0_active) {
        int mm0 = mac1_active ? 0 : MM;
        mac0_output(true, false, op0, h00, h10, dst, src0, src1, out());
        decode_optmode_impl(mmod, mm0, out());
    } else {
        decode_optmode_impl(mmod, MM, out());
    }
    out() << '\n'; return true;
}

// P=1, w1=1, w0=0: R(dst+1) = (mac1), A0 op
bool DisasmVisitor::decode_dsp32mac_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if ((mmod == M_W32) || (((1 << mmod) & 0x131b) == 0)) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    bool mac0_active = (op0 != 3);
    mac1_output(true, true, op1, h01, h11, dst, src0, src1, out());
    if (mac0_active) {
        if (MM) out() << " (M)";
        out() << ", ";
        mac0_output(true, false, op0, h00, h10, dst, src0, src1, out());
        decode_optmode_impl(mmod, 0, out());
    } else {
        decode_optmode_impl(mmod, MM, out());
    }
    out() << '\n'; return true;
}

// P=1, w1=0, w0=1: A1 op, R(dst) = (mac0)
bool DisasmVisitor::decode_dsp32mac_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if ((mmod == M_W32) || (((1 << mmod) & 0x131b) == 0)) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    bool mac1_active = (op1 != 3);
    if (mac1_active) {
        mac1_output(true, false, op1, h01, h11, dst, src0, src1, out());
        if (MM) out() << " (M)";
        out() << ", ";
    }
    mac0_output(true, true, op0, h00, h10, dst, src0, src1, out());
    decode_optmode_impl(mmod, 0, out());
    out() << '\n'; return true;
}

// P=1, w1=1, w0=1: R(dst+1) = (mac1), R(dst) = (mac0)
bool DisasmVisitor::decode_dsp32mac_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM, uint32_t op1,
    uint32_t h01h11, uint32_t op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (op1 == 3 && MM) return false;
    if ((mmod == M_W32) || (((1 << mmod) & 0x131b) == 0)) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    mac1_output(true, true, op1, h01, h11, dst, src0, src1, out());
    if (MM) out() << " (M)";
    out() << ", ";
    mac0_output(true, true, op0, h00, h10, dst, src0, src1, out());
    decode_optmode_impl(mmod, 0, out());
    out() << '\n'; return true;
}

// ---- dsp32mult per-variant handlers ----
// op1 and op0 are fixed as literal 00 in the patterns (ignored by reference).
// Pattern: "1100 M 01 mmmm j P w1 00 aa w0 00 ee ddd fff ggg"
// Visitor: (M, mmod, MM, h01h11, h00h10, dst, src0, src1)

// P=0, w1=1, w0=0: Rn.h = multfunc(h01,h11,src0,src1)
bool DisasmVisitor::decode_dsp32mult_P0_Wn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x1b57) == 0) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    out() << dregs_hi_str(dst) << " = ";
    decode_multfunc_impl(h01, h11, src0, src1, out());
    decode_optmode_impl(mmod, MM, out());
    out() << '\n'; return true;
}

// P=0, w1=0, w0=1: Rn.l = multfunc(h00,h10,src0,src1)
bool DisasmVisitor::decode_dsp32mult_P0_nW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x1b57) == 0) return false;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    out() << dregs_lo_str(dst) << " = ";
    decode_multfunc_impl(h00, h10, src0, src1, out());
    decode_optmode_impl(mmod, MM, out());
    out() << '\n'; return true;
}

// P=0, w1=1, w0=1: Rn.h = mult1, Rn.l = mult0
bool DisasmVisitor::decode_dsp32mult_P0_WW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x1b57) == 0) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    out() << dregs_hi_str(dst) << " = ";
    decode_multfunc_impl(h01, h11, src0, src1, out());
    if (MM) { out() << " (M)"; }
    out() << ", ";
    out() << dregs_lo_str(dst) << " = ";
    decode_multfunc_impl(h00, h10, src0, src1, out());
    decode_optmode_impl(mmod, 0, out());
    out() << '\n'; return true;
}

// P=1, w1=1, w0=0: R(dst+1) = multfunc(h01,h11,src0,src1)
bool DisasmVisitor::decode_dsp32mult_P1_Wn(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x313) == 0) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    out() << dregs_range_start(dst + 1) << " = ";
    decode_multfunc_impl(h01, h11, src0, src1, out());
    decode_optmode_impl(mmod, MM, out());
    out() << '\n'; return true;
}

// P=1, w1=0, w0=1: R(dst) = multfunc(h00,h10,src0,src1)
bool DisasmVisitor::decode_dsp32mult_P1_nW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x313) == 0) return false;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    out() << dregs_str(dst) << " = ";
    decode_multfunc_impl(h00, h10, src0, src1, out());
    decode_optmode_impl(mmod, MM, out());
    out() << '\n'; return true;
}

// P=1, w1=1, w0=1: R(dst+1) = mult1, R(dst) = mult0
bool DisasmVisitor::decode_dsp32mult_P1_WW(uint32_t M, uint32_t mmod, uint32_t MM,
    uint32_t _op1, uint32_t h01h11, uint32_t _op0, uint32_t h00h10, uint32_t dst, uint32_t src0, uint32_t src1) {
    if (((1 << mmod) & 0x313) == 0) return false;
    int h01 = (h01h11 >> 1) & 1, h11 = h01h11 & 1;
    int h00 = (h00h10 >> 1) & 1, h10 = h00h10 & 1;
    out() << dregs_range_start(dst + 1) << " = ";
    decode_multfunc_impl(h01, h11, src0, src1, out());
    if (MM) { out() << " (M)"; }
    out() << ", ";
    out() << dregs_str(dst) << " = ";
    decode_multfunc_impl(h00, h10, src0, src1, out());
    decode_optmode_impl(mmod, 0, out());
    out() << '\n'; return true;
}

// ---- dsp32alu static helpers ----
static const char* amod0_str(uint32_t s, uint32_t x) {
    if (s == 1 && x == 0) return " (S)";
    if (s == 0 && x == 1) return " (CO)";
    if (s == 1 && x == 1) return " (SCO)";
    return "";
}
static const char* amod1_str(uint32_t s, uint32_t x) {
    if (s == 0 && x == 0) return " (NS)";
    if (s == 1 && x == 0) return " (S)";
    return "";
}
static const char* amod0amod2_str(uint32_t s, uint32_t x, uint32_t aop) {
    if (aop == 0) {
        if (s == 1 && x == 0) return " (S)";
        if (s == 0 && x == 1) return " (CO)";
        if (s == 1 && x == 1) return " (SCO)";
        return "";
    }
    if (aop == 2) {
        if (s == 0 && x == 0) return " (ASR)";
        if (s == 1 && x == 0) return " (S, ASR)";
        if (s == 0 && x == 1) return " (CO, ASR)";
        if (s == 1 && x == 1) return " (SCO, ASR)";
    }
    if (aop == 3) {
        if (s == 0 && x == 0) return " (ASL)";
        if (s == 1 && x == 0) return " (S, ASL)";
        if (s == 0 && x == 1) return " (CO, ASL)";
        if (s == 1 && x == 1) return " (SCO, ASL)";
    }
    return "";
}
static const char* searchmod_str(uint32_t aop) {
    if (aop == 0) return "GT";
    if (aop == 1) return "GE";
    if (aop == 2) return "LT";
    return "LE";
}
static const char* aligndir_str(uint32_t s) {
    return (s == 1) ? " (R)" : "";
}
// imm5d: decimal representation of register index (same as dregs_str for number)
static std::string imm5d_str(uint32_t r) {
    // imm5d: decimal register index (used in range notation like "r1:0")
    std::ostringstream os; os << r; return os.str();
}

// ---- dsp32alu individual handlers ----

// aopcde=0: ADDADD/ADDSUB/SUBADD/SUBSUB (aop fixed per entry)
bool DisasmVisitor::decode_dsp32alu_ADDADD(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = " << dregs_str(src0) << " +|+ " << dregs_str(src1) << amod0_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADDSUB(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = " << dregs_str(src0) << " +|- " << dregs_str(src1) << amod0_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUBADD(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = " << dregs_str(src0) << " -|+ " << dregs_str(src1) << amod0_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUBSUB(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = " << dregs_str(src0) << " -|- " << dregs_str(src1) << amod0_str(s,x) << '\n';
    return true;
}

// aopcde=1: QUADADD (aop variable — selects ASR/ASL/etc suffix)
bool DisasmVisitor::decode_dsp32alu_QUADADD_HL0(uint32_t M, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst1) << " = " << dregs_str(src0) << " +|+ " << dregs_str(src1)
          << ", " << dregs_str(dst0) << " = " << dregs_str(src0) << " -|- " << dregs_str(src1)
          << amod0amod2_str(s,x,aop) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_QUADADD_HL1(uint32_t M, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst1) << " = " << dregs_str(src0) << " +|- " << dregs_str(src1)
          << ", " << dregs_str(dst0) << " = " << dregs_str(src0) << " -|+ " << dregs_str(src1)
          << amod0amod2_str(s,x,aop) << '\n';
    return true;
}

// aopcde=2: ADD16 half-word (HL and aop fixed per entry)
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd0_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_lo_str(src0) << " + " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd0_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_lo_str(src0) << " + " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd0_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_hi_str(src0) << " + " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd0_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_hi_str(src0) << " + " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd1_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_lo_str(src0) << " + " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd1_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_lo_str(src0) << " + " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd1_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_hi_str(src0) << " + " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD16_HLd1_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_hi_str(src0) << " + " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}

// aopcde=3: SUB16 half-word (HL and aop fixed per entry)
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd0_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_lo_str(src0) << " - " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd0_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_lo_str(src0) << " - " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd0_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_hi_str(src0) << " - " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd0_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_hi_str(src0) << " - " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd1_LL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_lo_str(src0) << " - " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd1_LH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_lo_str(src0) << " - " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd1_HL(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_hi_str(src0) << " - " << dregs_lo_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB16_HLd1_HH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_hi_str(src0) << " - " << dregs_hi_str(src1) << amod1_str(s,x) << '\n';
    return true;
}

// aopcde=4: ADD32/SUB32/ADDSUB32_dual
bool DisasmVisitor::decode_dsp32alu_ADD32(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = " << dregs_str(src0) << " + " << dregs_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB32(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = " << dregs_str(src0) << " - " << dregs_str(src1) << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADDSUB32_dual(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst1) << " = " << dregs_str(src0) << " + " << dregs_str(src1)
          << ", " << dregs_str(dst0) << " = " << dregs_str(src0) << " - " << dregs_str(src1)
          << amod1_str(s,x) << '\n';
    return true;
}

// aopcde=5: RND variants (s=0 fixed, HL and aop fixed per entry, x variable)
bool DisasmVisitor::decode_dsp32alu_ADD_RND12_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_str(src0) << " + " << dregs_str(src1) << " (RND12)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB_RND12_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_str(src0) << " - " << dregs_str(src1) << " (RND12)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD_RND20_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_str(src0) << " + " << dregs_str(src1) << " (RND20)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB_RND20_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_str(src0) << " - " << dregs_str(src1) << " (RND20)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD_RND12_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_str(src0) << " + " << dregs_str(src1) << " (RND12)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB_RND12_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_str(src0) << " - " << dregs_str(src1) << " (RND12)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ADD_RND20_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_str(src0) << " + " << dregs_str(src1) << " (RND20)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_SUB_RND20_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_str(src0) << " - " << dregs_str(src1) << " (RND20)" << '\n';
    return true;
}

// aopcde=6: VMAX/VMIN/VABS
bool DisasmVisitor::decode_dsp32alu_VMAX(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = MAX (" << dregs_str(src0) << ", " << dregs_str(src1) << ") (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_VMIN(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = MIN (" << dregs_str(src0) << ", " << dregs_str(src1) << ") (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_VABS(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = ABS " << dregs_str(src0) << " (V)" << '\n';
    return true;
}
// aopcde=7: MAX/MIN/ABS/NEG (NEG split into _NS and _S)
bool DisasmVisitor::decode_dsp32alu_MAX(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = MAX (" << dregs_str(src0) << ", " << dregs_str(src1) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_MIN(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = MIN (" << dregs_str(src0) << ", " << dregs_str(src1) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ABS(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = ABS " << dregs_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_NEG_NS(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = -" << dregs_str(src0) << " (NS)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_NEG_S(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = -" << dregs_str(src0) << " (S)" << '\n';
    return true;
}

// aopcde=8: accumulator clear/sat/copy (s and aop fixed per entry; HL ignored)
bool DisasmVisitor::decode_dsp32alu_ACC_A0_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = 0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_A0_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = A0 (S)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_A1_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = 0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_A1_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = A1 (S)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_A1A0_CLR(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = A0 = 0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_A1A0_SAT(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = A1 (S), A0 = A0 (S)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_COPY_A0_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_COPY_A1_A0(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = A0" << '\n'; return true;
}

// aopcde=9: accumulator loads (aop fixed per entry, HL and s variable)
// aop=0,s=0: A0.L=Rs.l  aop=0,s=0,HL=1: A0.H=Rs.h  aop=0,s=1: A0=Rs (full)
// aop=1,s=0: A0.X=Rs.l  aop=2,s=0: A1.L or A1.H  aop=2,s=1: A1=Rs  aop=3,s=0: A1.X=Rs.l
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP0_full(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = " << dregs_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP0_lo(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0.L = " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP0_hi(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0.H = " << dregs_hi_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0.X = " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP2_full(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = " << dregs_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP2_lo(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1.L = " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP2_hi(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1.H = " << dregs_hi_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_LOAD_AOP3(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1.X = " << dregs_lo_str(src0) << '\n'; return true;
}

// aopcde=10: A0.X/A1.X read  (aop=0→A0.X, aop=1→A1.X)
bool DisasmVisitor::decode_dsp32alu_A0X_READ(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = A0.X" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_A1X_READ(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = A1.X" << '\n';
    return true;
}

// aopcde=11: (A0 += A1), A0 += A1, A0 -= A1
bool DisasmVisitor::decode_dsp32alu_A0_PLUS_A1(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = (A0 += A1)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_A0_PLUS_A1_HL_lo(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = (A0 += A1)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A0_PLUS_A1_HL_hi(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = (A0 += A1)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A0_INC_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 += A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A0_INC_A1_W32(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 += A1 (W32)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A0_DEC_A1(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 -= A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A0_DEC_A1_W32(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 -= A1 (W32)" << '\n'; return true;
}

// aopcde=12: SIGN_MULT, ACC_ACCUM_SUM (aop=1), RND_HL (aop=3)
bool DisasmVisitor::decode_dsp32alu_SIGN_MULT(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_lo_str(dst0)
          << " = SIGN (" << dregs_hi_str(src0) << ") * " << dregs_hi_str(src1)
          << " + SIGN (" << dregs_lo_str(src0) << ") * " << dregs_lo_str(src1) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_ACC_ACCUM_SUM(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst1) << " = A1.L + A1.H, " << dregs_str(dst0) << " = A0.L + A0.H" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_RND_HL_lo(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst0) << " = " << dregs_str(src0) << " (RND)" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_RND_HL_hi(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_hi_str(dst0) << " = " << dregs_str(src0) << " (RND)" << '\n'; return true;
}

// aopcde=13: SEARCH (aop variable: GT/GE/LT/LE)
bool DisasmVisitor::decode_dsp32alu_SEARCH(uint32_t M, uint32_t HL, uint32_t aop, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "(" << dregs_str(dst1) << ", " << dregs_str(dst0) << ") = SEARCH "
          << dregs_str(src0) << " (" << searchmod_str(aop) << ")" << '\n';
    return true;
}

// aopcde=14: A_NEG (HL and aop fixed per entry)
bool DisasmVisitor::decode_dsp32alu_A_NEG_HL0_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = -A0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_NEG_HL0_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = -A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_NEG_HL1_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = -A0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_NEG_HL1_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = -A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_NEG_BOTH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = -A1, A0 = -A0" << '\n'; return true;
}

// aopcde=15: NEG_V (aop=3, HL=0 fixed)
bool DisasmVisitor::decode_dsp32alu_NEG_V(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = -" << dregs_str(src0) << " (V)" << '\n';
    return true;
}

// aopcde=16: A_ABS (HL and aop fixed per entry)
bool DisasmVisitor::decode_dsp32alu_A_ABS_HL0_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = ABS A0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_ABS_HL0_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A0 = ABS A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_ABS_HL1_AOP0(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = ABS A0" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_ABS_HL1_AOP1(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = ABS A1" << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32alu_A_ABS_BOTH(uint32_t M, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "A1 = ABS A1, A0 = ABS A0" << '\n'; return true;
}

// aopcde=17: A1+A0/A1-A0 and A0+A1/A0-A1
bool DisasmVisitor::decode_dsp32alu_A1pA0_A1mA0(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst1) << " = A1 + A0, " << dregs_str(dst0) << " = A1 - A0" << amod1_str(s,x) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_A0pA1_A0mA1(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst1) << " = A0 + A1, " << dregs_str(dst0) << " = A0 - A1" << amod1_str(s,x) << '\n';
    return true;
}

// aopcde=18: SAA/DISALGNEXCPT
bool DisasmVisitor::decode_dsp32alu_SAA(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "SAA (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ")"
          << aligndir_str(s) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_DISALGNEXCPT(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "DISALGNEXCPT" << '\n';
    return true;
}

// aopcde=20: BYTEOP1P
bool DisasmVisitor::decode_dsp32alu_BYTEOP1P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP1P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ")" << aligndir_str(s) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP1P_T(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP1P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (T)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP1P_T_R(uint32_t M, uint32_t HL, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP1P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (T, R)" << '\n';
    return true;
}

// aopcde=21: BYTEOP16P/BYTEOP16M
bool DisasmVisitor::decode_dsp32alu_BYTEOP16P(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "(" << dregs_str(dst1) << ", " << dregs_str(dst0) << ") = BYTEOP16P ("
          << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ")" << aligndir_str(s) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP16M(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "(" << dregs_str(dst1) << ", " << dregs_str(dst0) << ") = BYTEOP16M ("
          << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ")" << aligndir_str(s) << '\n';
    return true;
}

// aopcde=22: BYTEOP2P (HL and aop fixed per entry; s may add ", R")
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_RNDL(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (RNDL)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_RNDL_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (RNDL, R)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_RNDH(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (RNDH)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_RNDH_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (RNDH, R)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_TL(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (TL)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_TL_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (TL, R)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_TH(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (TH)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP2P_TH_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP2P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (TH, R)" << '\n';
    return true;
}

// aopcde=23: BYTEOP3P (HL and aop fixed per entry; s may add ", R")
bool DisasmVisitor::decode_dsp32alu_BYTEOP3P_LO(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP3P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (LO)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP3P_LO_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP3P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (LO, R)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP3P_HI(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP3P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (HI)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEOP3P_HI_R(uint32_t M, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEOP3P (" << dregs_range_start(src0+1) << ":" << imm5d_str(src0)
          << ", " << dregs_range_start(src1+1) << ":" << imm5d_str(src1) << ") (HI, R)" << '\n';
    return true;
}

// aopcde=24: BYTEPACK/BYTEUNPACK
bool DisasmVisitor::decode_dsp32alu_BYTEPACK(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst0) << " = BYTEPACK (" << dregs_str(src0) << ", " << dregs_str(src1) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32alu_BYTEUNPACK(uint32_t M, uint32_t HL, uint32_t s, uint32_t x, uint32_t dst0, uint32_t dst1, uint32_t src0, uint32_t src1) {
    out() << "(" << dregs_str(dst1) << ", " << dregs_str(dst0) << ") = BYTEUNPACK "
          << dregs_range_start(src0+1) << ":" << imm5d_str(src0) << aligndir_str(s) << '\n';
    return true;
}

// dsp32shift: individual handlers (sopcde and sop fixed in pattern)
// HLs[1:0]: bit1 selects dst half (0=lo, 1=hi), bit0 selects src1 half (0=lo, 1=hi)
#define DSP32SHIFT_HALF_SRC(r, hls)  ((hls) & 1 ? dregs_hi_str(r) : dregs_lo_str(r))
#define DSP32SHIFT_HALF_DST(r, hls)  ((hls) & 2 ? dregs_hi_str(r) : dregs_lo_str(r))

bool DisasmVisitor::decode_dsp32shift_ASHIFT16(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = ASHIFT " << DSP32SHIFT_HALF_SRC(src1, HLs) << " BY " << dregs_lo_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ASHIFT16S(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = ASHIFT " << DSP32SHIFT_HALF_SRC(src1, HLs) << " BY " << dregs_lo_str(src0) << " (S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_LSHIFT16(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = LSHIFT " << DSP32SHIFT_HALF_SRC(src1, HLs) << " BY " << dregs_lo_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VASHIFT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ASHIFT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << " (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VASHIFTS(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ASHIFT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << " (V, S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VLSHIFT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = LSHIFT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << " (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ASHIFT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ASHIFT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ASHIFT32S(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ASHIFT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << " (S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_LSHIFT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = LSHIFT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ROT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ROT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ACC_ASHIFT_A0(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A0 = ASHIFT A0 BY " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32shift_ACC_ASHIFT_A1(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A1 = ASHIFT A1 BY " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32shift_ACC_LSHIFT_A0(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A0 = LSHIFT A0 BY " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32shift_ACC_LSHIFT_A1(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A1 = LSHIFT A1 BY " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32shift_ACC_ROT_A0(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A0 = ROT A0 BY " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32shift_ACC_ROT_A1(uint32_t M, uint32_t h, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A1 = ROT A1 BY " << dregs_lo_str(src0) << '\n'; return true;
}
bool DisasmVisitor::decode_dsp32shift_ROT32_dreg(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ROT " << dregs_str(src1) << " BY " << dregs_lo_str(src0) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_PACK_LL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = PACK (" << dregs_lo_str(src1) << ", " << dregs_lo_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_PACK_LH(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = PACK (" << dregs_lo_str(src1) << ", " << dregs_hi_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_PACK_HL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = PACK (" << dregs_hi_str(src1) << ", " << dregs_lo_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_PACK_HH(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = PACK (" << dregs_hi_str(src1) << ", " << dregs_hi_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_SIGNBITS_32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = SIGNBITS " << dregs_str(src1) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_SIGNBITS_16L(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = SIGNBITS " << dregs_lo_str(src1) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_SIGNBITS_16H(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = SIGNBITS " << dregs_hi_str(src1) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_SIGNBITS_A0(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = SIGNBITS A0" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_SIGNBITS_A1(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = SIGNBITS A1" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ONES(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = ONES " << dregs_str(src1) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_EXPADJ_32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = EXPADJ (" << dregs_str(src1) << ", " << dregs_lo_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_EXPADJ_V(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = EXPADJ (" << dregs_str(src1) << ", " << dregs_lo_str(src0) << ") (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_EXPADJ_16L(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = EXPADJ (" << dregs_lo_str(src1) << ", " << dregs_lo_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_EXPADJ_16H(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = EXPADJ (" << dregs_hi_str(src1) << ", " << dregs_lo_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_BITMUX_ASR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "BITMUX (" << dregs_str(src0) << ", " << dregs_str(src1) << ", A0) (ASR)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_BITMUX_ASL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "BITMUX (" << dregs_str(src0) << ", " << dregs_str(src1) << ", A0) (ASL)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VITMAX_ASL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = VIT_MAX (" << dregs_str(src1) << ") (ASL)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VITMAX_ASR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = VIT_MAX (" << dregs_str(src1) << ") (ASR)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VITMAX2_ASL(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = VIT_MAX (" << dregs_str(src1) << ", " << dregs_str(src0) << ") (ASL)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_VITMAX2_ASR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = VIT_MAX (" << dregs_str(src1) << ", " << dregs_str(src0) << ") (ASR)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_EXTRACT_Z(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = EXTRACT (" << dregs_str(src1) << ", " << dregs_lo_str(src0) << ") (Z)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_EXTRACT_X(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = EXTRACT (" << dregs_str(src1) << ", " << dregs_lo_str(src0) << ") (X)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_DEPOSIT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = DEPOSIT (" << dregs_str(src1) << ", " << dregs_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_DEPOSIT_X(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = DEPOSIT (" << dregs_str(src1) << ", " << dregs_str(src0) << ") (X)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_BXORSHIFT(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = CC = BXORSHIFT (A0, " << dregs_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_BXOR(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = CC = BXOR (A0, " << dregs_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_BXORSHIFT3(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << "A0 = BXORSHIFT (A0, A1, CC)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_BXOR3(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_lo_str(dst) << " = CC = BXOR (A0, A1, CC)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ALIGN8(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ALIGN8 (" << dregs_str(src1) << ", " << dregs_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ALIGN16(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ALIGN16 (" << dregs_str(src1) << ", " << dregs_str(src0) << ")" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shift_ALIGN24(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t src0, uint32_t src1) {
    out() << dregs_str(dst) << " = ALIGN24 (" << dregs_str(src1) << ", " << dregs_str(src0) << ")" << '\n';
    return true;
}

// dsp32shiftimm: individual handlers
// For "arith" (right-shift) variants: displayed amount = (-immag) & mask
// For sopcde=0,1: 4-bit amount → mask=0xf; for sopcde=2,3: 5-bit → mask=0x1f
// imm5 is the lower 5 bits; b is bit8 of the original 6-bit immag field.
// For variants where b is a pattern variable, full immag = (b<<5)|imm5.
// Shift immediate formatting helpers — output hex like GNU objdump's uimm4/uimm5/imm5/imm6
// These just format; callers compute the correct value first.
static std::string uimm_hex(uint32_t v) {
    std::ostringstream os; os << "0x" << std::hex << v; return os.str();
}
static std::string imm_shex(int32_t v) {
    // signed hex (for ROT & similar)
    std::ostringstream os;
    if (v < 0) os << "-0x" << std::hex << (uint32_t)(-v);
    else       os << "0x" << std::hex << (uint32_t)v;
    return os.str();
}
// Helpers that compute the value AND format
static std::string uimm4_neg_hex(uint32_t immag) { return uimm_hex((-immag) & 0xf); }
static std::string uimm5_neg_hex(uint32_t immag) { return uimm_hex((-immag) & 0x1f); }
static std::string uimm6_neg_hex(uint32_t immag) { return uimm_hex((-immag) & 0x3f); }
static std::string imm6_hex(uint32_t immag) {
    int32_t v = (immag & 0x20) ? (int32_t)(immag | ~0x3f) : (int32_t)immag;
    return imm_shex(v);
}



bool DisasmVisitor::decode_dsp32shiftimm_ASHIFT16_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // sopcde=0, sop=0: arith right shift; full 6-bit immag = (b<<5)|imm5; display = uimm4(newimmag)
    uint32_t immag = (b << 5) | imm5;
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = " << DSP32SHIFT_HALF_SRC(src1, HLs) << " >>> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_ASHIFT16S_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; immag=imm5; uimm4(immag) formats value as-is (no masking)
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = " << DSP32SHIFT_HALF_SRC(src1, HLs) << " << " << uimm_hex(imm5) << " (S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_ASHIFT16S_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; full_immag = 0x20|imm5; display = uimm4(newimmag)
    uint32_t immag = 0x20 | imm5;
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = " << DSP32SHIFT_HALF_SRC(src1, HLs) << " >>> " << uimm6_neg_hex(immag) << " (S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_LSHIFT16_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; uimm4(immag) = imm5 unmasked
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = " << DSP32SHIFT_HALF_SRC(src1, HLs) << " << " << uimm_hex(imm5) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_LSHIFT16_right(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; full_immag = 0x20|imm5; display = uimm4(newimmag)
    uint32_t immag = 0x20 | imm5;
    out() << DSP32SHIFT_HALF_DST(dst, HLs) << " = " << DSP32SHIFT_HALF_SRC(src1, HLs) << " >> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_VASHIFT_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // sopcde=1, sop=0: vector arith right; full 6-bit; display = uimm5(newimmag)
    uint32_t immag = (b << 5) | imm5;
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " >>> " << uimm6_neg_hex(immag) << " (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_VASHIFTS_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; uimm5(immag) = imm5 unmasked
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " << " << uimm_hex(imm5) << " (V, S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_VASHIFTS_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; ref: imm5(-immag) = sign-extend 5 bits of -(0x20|imm5)
    uint32_t full_immag = 0x20 | imm5;
    int32_t v = (int32_t)(((uint32_t)(-(int32_t)full_immag) & 0x1f) << 27) >> 27;
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " >>> " << imm_shex(v) << " (V, S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_VLSHIFT_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; imm5(immag) = imm5 unmasked (signed 5-bit hex, but positive so same as uimm)
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " << " << uimm_hex(imm5) << " (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_VLSHIFT_right(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; display = uimm5(newimmag)
    uint32_t immag = 0x20 | imm5;
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " >> " << uimm6_neg_hex(immag) << " (V)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_ASHIFT32_arith(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // sopcde=2, sop=0: full 6-bit; display = uimm5(newimmag)
    uint32_t immag = (b << 5) | imm5;
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " >>> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_ASHIFT32S_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // sopcde=2, sop=1: uimm5(immag) where immag is full 6-bit; no masking
    uint32_t immag = (b << 5) | imm5;
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " << " << uimm_hex(immag) << " (S)" << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_LSHIFT32_left(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; uimm5(immag) = imm5 unmasked
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " << " << uimm_hex(imm5) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_LSHIFT32_right(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; display = uimm5(newimmag)
    uint32_t immag = 0x20 | imm5;
    out() << dregs_str(dst) << " = " << dregs_str(src1) << " >> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_ROT32(uint32_t M, uint32_t HLs, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    uint32_t immag = (b << 5) | imm5;
    out() << dregs_str(dst) << " = ROT " << dregs_str(src1) << " BY " << imm6_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A0_ASHIFT_left(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; uimm5(immag) = imm5 unmasked
    out() << "A0 = A0 << " << uimm_hex(imm5) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A0_ASHIFT_arith(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; full_immag = 0x20|imm5; display = uimm5(newimmag)
    uint32_t immag = 0x20 | imm5;
    out() << "A0 = A0 >>> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A1_ASHIFT_left(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=0 fixed; uimm5(immag) = imm5 unmasked
    out() << "A1 = A1 << " << uimm_hex(imm5) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A1_ASHIFT_arith(uint32_t M, uint32_t dst, uint32_t imm5, uint32_t src1) {
    // b=1 fixed; full_immag = 0x20|imm5; display = uimm5(newimmag)
    uint32_t immag = 0x20 | imm5;
    out() << "A1 = A1 >>> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A0_LSHIFT_right(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // b variable; display = uimm5(newimmag)
    uint32_t immag = (b << 5) | imm5;
    out() << "A0 = A0 >> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A1_LSHIFT_right(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    // b variable; display = uimm5(newimmag)
    uint32_t immag = (b << 5) | imm5;
    out() << "A1 = A1 >> " << uimm6_neg_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A0_ROT(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    uint32_t immag = (b << 5) | imm5;
    out() << "A0 = ROT A0 BY " << imm6_hex(immag) << '\n';
    return true;
}
bool DisasmVisitor::decode_dsp32shiftimm_A1_ROT(uint32_t M, uint32_t dst, uint32_t b, uint32_t imm5, uint32_t src1) {
    uint32_t immag = (b << 5) | imm5;
    out() << "A1 = ROT A1 BY " << imm6_hex(immag) << '\n';
    return true;
}

// PseudoDbg_Assert instructions
bool DisasmVisitor::decode_pseudoDbg_Assert_lo(uint32_t grp, uint32_t regtest, uint32_t expected) {
    out() << "dbga (" << regs_lo_lookup(grp, regtest) << ", 0x" << std::hex << expected << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDbg_Assert_hi(uint32_t grp, uint32_t regtest, uint32_t expected) {
    out() << "dbga (" << regs_hi_lookup(grp, regtest) << ", 0x" << std::hex << expected << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDbg_Assert_low(uint32_t grp, uint32_t regtest, uint32_t expected) {
    out() << "dbgal (" << allregs(regtest, grp) << ", 0x" << std::hex << expected << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_pseudoDbg_Assert_high(uint32_t grp, uint32_t regtest, uint32_t expected) {
    out() << "dbgah (" << allregs(regtest, grp) << ", 0x" << std::hex << expected << std::dec << ")" << '\n';
    return true;
}

bool DisasmVisitor::decode_unknown_16(uint16_t insn) {
    (void)insn;
    out() << "illegal\n";
    return true;
}

bool DisasmVisitor::decode_unknown_32(uint32_t insn) {
    (void)insn;
    out() << "illegal\n";
    return true;
}
