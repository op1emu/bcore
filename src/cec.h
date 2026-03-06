#pragma once

#include <cstdint>
#include "mmr.h"
#include "cpu_state.h"
#include "evt.h"
#include "mem.h"

// Core Event Controller (CEC) helpers — extern "C" so JIT can call them.
//
// Semantics derived from refs/dv-bfin_cec.c (GNU binutils simulator).
// This is a usermode-focused subset: no MMU/CPLB, no peripheral interrupts.
//
// CEC owns its register state as module-level statics:
//   s_cec_mmr[5] — EVT_OVERRIDE/IMASK/IPEND/ILAT/IPRIO at 0xFFE02100-0xFFE02110
//   s_evt[16]    — EVT0-EVT15 at 0xFFE02000-0xFFE0203C
//
// FlatMemory delegates read32/write32 for these address ranges to the
// cec_mmr_read/write and cec_evt_read/write functions below.

// Address constants, exception cause codes, and address predicates
// are defined in include/mmr.h (included above).

// ── Runtime helpers ───────────────────────────────────────────────────────────

/// Returns current active IVG level from an IPEND value: -1 = user mode, >=0 = supervisor.
// Excludes only IVG_EMU(0) and IVG_IRPTEN(4), matching reference _cec_get_ivg.
// IVG_RST(1) is NOT excluded: it marks the active boot supervisor context.
inline int cec_current_ivg_val(uint32_t ipend) {
    uint32_t p = ipend & ~((1u<<0)|(1u<<4));
    return p ? static_cast<int>(__builtin_ctz(p)) : -1;
}

extern "C" {
    // EXCPT imm: set SEQSTAT.EXCAUSE, RETX, dispatch to EVT3 handler.
    void cec_exception(CpuState* cpu, uint32_t excp);

    // RAISE imm: latch ILAT, dispatch if enabled and higher priority than current.
    void cec_raise(CpuState* cpu, uint32_t ivg);

    // RTI: clear IPEND for current IVG, return RETI.
    uint32_t cec_return_rti(CpuState* cpu);

    // RTX: clear IPEND for IVG_EVX (3), return RETX.
    uint32_t cec_return_rtx(CpuState* cpu);

    // RTN: clear IPEND for IVG_NMI (2), return RETN.
    uint32_t cec_return_rtn(CpuState* cpu);

    // RTE: clear IPEND for IVG_EMU (0), return RETE.
    uint32_t cec_return_rte(CpuState* cpu);

    // CLI Rd: globally mask interrupts, return old IMASK (to be stored in Rd).
    // Raises VEC_ILL_RES if called from user mode.
    uint32_t cec_cli(CpuState* cpu);

    // STI Rd: restore IMASK from the value in Rd.
    // Raises VEC_ILL_RES if called from user mode.
    void cec_sti(CpuState* cpu, uint32_t mask);

    // [--SP] = RETI: clears IRPTEN in IPEND, enabling nested maskable interrupts.
    // Raises VEC_ILL_RES if called from user mode (supervisor register access).
    void cec_push_reti(CpuState* cpu);

    // RETI = [SP++]: sets IRPTEN in IPEND, restoring the global interrupt mask.
    // Raises VEC_ILL_RES if called from user mode.
    void cec_pop_reti(CpuState* cpu);

    // Returns true if currently in user mode (IPEND has no supervisor bits).
    // Used as a JIT guard to conditionally raise different exceptions per access type.
    bool cec_is_user_mode();

    // Raise VEC_ILL_RES if currently in user mode.
    // Used as a JIT guard for supervisor-only register accesses (group-7 regs).
    void cec_check_sup(CpuState* cpu);

    void cec_check_pending(CpuState* cpu);

    // Raise VEC_DCPLB_MISS if in user mode and addr is in System MMR space (>= 0xFFC00000).
    // Returns true if an exception was raised, false otherwise.
    bool user_mmr_check(CpuState* cpu, uint32_t addr);
}
