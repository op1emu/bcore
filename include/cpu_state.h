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
    // Packets the last Core::run entered: the 1-based index in the block of
    // the packet running, so an early exit (a taken conditional branch, a
    // hardware-loop back edge, an exception) leaves the exact count rather
    // than the block's static length. A packet that raises an exception is
    // counted. Like pc and insn_len, generated code writes it only where it
    // can be read: before a helper call and before the block returns
    // (Core::set_lazy_state).
    uint32_t packets;
    // Memory::page_table() of the Memory this state runs with, or null; set
    // by Core before every block it runs. Generated code loads it from here
    // rather than embedding the host address, so the same block compiles to
    // the same code in every process.
    uint8_t* const* page_table;
    // The CEC's interrupt-may-be-pending flag; set by Core likewise, read by
    // generated code before it calls cec_check_pending.
    const bool* cec_pending;
    // The host's wakeup-is-now flag (Core::set_idle_waits), or null: an IDLE
    // runs on as a NOP while it or cec_pending is set. Set by Core likewise.
    const uint8_t* idle_wake;

    // Chaining: a block that ends may run the next one itself instead of
    // returning to the host (Core::set_chaining). Core::run sets the table and
    // clears the counts before every run; the host sets the budget.
    //
    // The JIT's direct-mapped table of compiled blocks (JitEngine::ChainSlot).
    const void* chain_table;
    // Packets the run may retire before it returns: a block that ends with
    // chain_packets + packets at or past it returns to the host. 0 never
    // chains. The CEC sets it to 0 on every interrupt entry, return,
    // exception and halt, and when a RAISE (cec_raise) or an IMASK write
    // leaves an interrupt pending, and so may the host's memory callbacks
    // and other threads: anything after which the host has work returns to
    // it. Below 2^31.
    uint32_t chain_budget;
    // Packets retired by the blocks that already ran in this run; the block
    // running counts its own in `packets`.
    uint32_t chain_packets;
    // Blocks that ran in this run after the first (0 if not counted,
    // Core::set_count_chained).
    uint32_t chain_blocks;
    // Set by a block compiled with Core::set_chain_dry when it would have
    // chained; it returns instead.
    uint8_t chain_would;
    // Set by a block whose entry guard found a hardware loop ending inside
    // it at a packet it was compiled to expect none at (BBTranslator's
    // lb_guard): the block returns without running a packet, and Core::run
    // compiles it again, careful at every packet.
    uint8_t lb_guard_miss;
    // Set by an IDLE (Core::set_idle_waits) that found no wakeup due now
    // (idle_wake, cec_pending): the block ended after it, with pc past it,
    // and returned. The core waits for a wakeup there: the host lets guest
    // time pass to its next event before the next run. Core::run clears it.
    uint8_t idle;
    // Memory read callbacks whose value or effect the host could not promise
    // would be the same if repeated before the run ends (a FIFO pop, a value
    // from the time): it counts them (Core::set_steady_skip). A loop whose
    // iteration made reads but left this unchanged read nothing that could
    // differ in the next one.
    uint64_t unstable_reads;
    // Steady iterations a loop would have skipped (Core::set_steady_skip's
    // dry mode), counted by generated code; the host reads and clears it.
    uint64_t steady_iterations;
    // CYCLES (cycles[1]:cycles[0]) when this run started. With
    // Core::set_host_cycles a chained block that accesses CYCLES sets it to
    // cycles_base + chain_packets first, as a host that writes CYCLES before
    // every block would.
    uint64_t cycles_base;
    // Not guest state: a parallel packet's slots 1 and 2 keep their register
    // writes here for the packet's commit (copy-and-patch, cp_translator.h).
    uint32_t slot_scratch[2][8];
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