#pragma once

#include <cstdint>
#include <cstring>

struct CpuState {
    // dpregs[0..7] = R0-R7, dpregs[8..15] = P0-P5, SP(14), FP(15)
    uint32_t dpregs[16];

    // 40-bit accumulators: A0 = (ax[0]:aw[0]), A1 = (ax[1]:aw[1])
    uint32_t ax[2];
    uint32_t aw[2];

    // DAG registers
    uint32_t iregs[4];
    uint32_t mregs[4];
    uint32_t bregs[4];
    uint32_t lregs[4];

    // Hardware loop registers
    uint32_t lt[2];  // loop top
    uint32_t lc[2];  // loop count
    uint32_t lb[2];  // loop bottom

    // System registers
    uint32_t rets;
    uint32_t reti;
    uint32_t retx;
    uint32_t retn;
    uint32_t rete;
    uint32_t pc;
    uint32_t cycles[3]; // cycles, cycles2, cycles2_shadow
    uint32_t usp;
    uint32_t ksp;        // kernel stack pointer (saved on user->supervisor transition)
    uint32_t seqstat;
    uint32_t syscfg;
    uint32_t emudat[2];

    // ASTAT flags (each stored as uint32_t for pointer/offset convenience)
    uint32_t az;
    uint32_t an;
    uint32_t ac0;
    uint32_t ac0_copy;
    uint32_t ac1;
    uint32_t v;
    uint32_t v_copy;
    uint32_t vs;
    uint32_t av0;
    uint32_t av0s;
    uint32_t av1;
    uint32_t av1s;
    uint32_t cc;
    uint32_t aq;
    uint32_t rnd_mod;
    uint32_t astat_reserved;

    // Execution state
    uint32_t insn_len;
    bool did_jump;
    bool halted;
    int exit_code;
    uint32_t steps_remaining; // 0 = unlimited; N > 0 = decrement per instruction
};

// ASTAT bit positions (from refs/bfin-sim.h)
enum AstatBit {
    AZ_BIT       = 0,
    AN_BIT       = 1,
    AC0_COPY_BIT = 2,
    V_COPY_BIT   = 3,
    CC_BIT       = 5,
    AQ_BIT       = 6,
    RND_MOD_BIT  = 8,
    AC0_BIT      = 12,
    AC1_BIT      = 13,
    AV0_BIT      = 16,
    AV0S_BIT     = 17,
    AV1_BIT      = 18,
    AV1S_BIT     = 19,
    V_BIT        = 24,
    VS_BIT       = 25,
};

static constexpr uint32_t ASTAT_DEFINED_BITS =
    (1u << AZ_BIT) | (1u << AN_BIT) | (1u << AC0_COPY_BIT) | (1u << V_COPY_BIT) |
    (1u << CC_BIT) | (1u << AQ_BIT) |
    (1u << RND_MOD_BIT) |
    (1u << AC0_BIT) | (1u << AC1_BIT) |
    (1u << AV0_BIT) | (1u << AV0S_BIT) | (1u << AV1_BIT) | (1u << AV1S_BIT) |
    (1u << V_BIT) | (1u << VS_BIT);

// Compose individual ASTAT flags into a 32-bit ASTAT value
inline uint32_t astat_compose(const CpuState* cpu) {
    return (cpu->az       << AZ_BIT)
         | (cpu->an       << AN_BIT)
         | (cpu->ac0_copy << AC0_COPY_BIT)
         | (cpu->v_copy   << V_COPY_BIT)
         | (cpu->cc       << CC_BIT)
         | (cpu->aq       << AQ_BIT)
         | (cpu->rnd_mod  << RND_MOD_BIT)
         | (cpu->ac0      << AC0_BIT)
         | (cpu->ac1      << AC1_BIT)
         | (cpu->av0      << AV0_BIT)
         | (cpu->av0s     << AV0S_BIT)
         | (cpu->av1      << AV1_BIT)
         | (cpu->av1s     << AV1S_BIT)
         | (cpu->v        << V_BIT)
         | (cpu->vs       << VS_BIT)
         | cpu->astat_reserved;
}

// Decompose a 32-bit ASTAT value into individual flags
inline void astat_decompose(CpuState* cpu, uint32_t astat) {
    cpu->az          = (astat >> AZ_BIT)       & 1;
    cpu->an          = (astat >> AN_BIT)       & 1;
    cpu->ac0_copy    = (astat >> AC0_COPY_BIT) & 1;
    cpu->v_copy      = (astat >> V_COPY_BIT)   & 1;
    cpu->cc          = (astat >> CC_BIT)       & 1;
    cpu->aq          = (astat >> AQ_BIT)       & 1;
    cpu->rnd_mod     = (astat >> RND_MOD_BIT)  & 1;
    cpu->ac0         = (astat >> AC0_BIT)      & 1;
    cpu->ac1         = (astat >> AC1_BIT)      & 1;
    cpu->av0         = (astat >> AV0_BIT)      & 1;
    cpu->av0s        = (astat >> AV0S_BIT)     & 1;
    cpu->av1         = (astat >> AV1_BIT)      & 1;
    cpu->av1s        = (astat >> AV1S_BIT)     & 1;
    cpu->v           = (astat >> V_BIT)        & 1;
    cpu->vs          = (astat >> VS_BIT)       & 1;
    cpu->astat_reserved = astat & ~ASTAT_DEFINED_BITS;
}

// Get 40-bit sign-extended accumulator as int64_t (matches refs/bfin-sim.c:1322)
inline int64_t get_extended_acc(const CpuState* cpu, int which) {
    uint64_t acc = cpu->ax[which];
    if (acc & 0x80)
        acc |= ~static_cast<uint64_t>(0x7F); // sign extend from bit 7
    else
        acc &= 0xFF;
    acc <<= 32;
    acc |= cpu->aw[which];
    return static_cast<int64_t>(acc);
}

// Get 40-bit unsigned accumulator
inline uint64_t get_unextended_acc(const CpuState* cpu, int which) {
    return (static_cast<uint64_t>(cpu->ax[which] & 0xFF) << 32) | cpu->aw[which];
}

// Write a 32-bit value to accumulator, sign-extending to ax
inline void set_acc_from_reg(CpuState* cpu, int which, uint32_t val) {
    cpu->aw[which] = val;
    cpu->ax[which] = (val >> 31) ? 0xFF : 0x00;
}

// extern "C" wrappers for JIT (defined in cpu_state.cpp — must NOT be inline)
extern "C" {
    uint32_t cpu_astat_compose(CpuState* cpu);
    void cpu_astat_decompose(CpuState* cpu, uint32_t astat);
    // Per-instruction hw-loop probe: update pc (Phase A) and decrement counters
    // (Phase B). Called from JIT'd code once per Blackfin instruction.
    void bfin_hwloop_step(CpuState* cpu, uint32_t insn_pc, uint32_t fallthrough_pc);
}

// Initialize CpuState to zero
void cpu_state_init(CpuState* cpu);

uint32_t hwloop_get_next_pc(CpuState* cpu, uint32_t insn_pc, uint32_t next_pc);