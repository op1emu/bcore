#include "cec.h"
#include "evt.h"
#include "syscall_emu.h"

#include <cstdio>
#include <cstdlib>

// IVG level constants (mirrors dv-bfin_cec.h)
static constexpr uint32_t IVG_EMU    = 0;
static constexpr uint32_t IVG_RST    = 1;
static constexpr uint32_t IVG_NMI    = 2;
static constexpr uint32_t IVG_EVX    = 3;
static constexpr uint32_t IVG_IRPTEN = 4;
static constexpr uint32_t IVG_IVHW   = 5;
static constexpr uint32_t IVG15      = 15;
static constexpr uint32_t IVG_USER   = 16;

// Bitmasks
static constexpr uint32_t IVG_EMU_B    = (1u << IVG_EMU);
static constexpr uint32_t IVG_RST_B    = (1u << IVG_RST);
static constexpr uint32_t IVG_IRPTEN_B = (1u << IVG_IRPTEN);
static constexpr uint32_t IVG_UNMASKABLE_B =
    IVG_EMU_B | IVG_RST_B | (1u<<IVG_NMI) | (1u<<IVG_EVX) | IVG_IRPTEN_B;
static constexpr uint32_t IVG_MASKABLE_B = ~IVG_UNMASKABLE_B & 0xFFFFu;

// CEC MMR bitmask constants for software-visible write side effects
static constexpr uint32_t CEC_UNMASKABLE_B = 0x1Fu;   // bits 0-4 (unmaskable IVG bits)
static constexpr uint32_t CEC_MASKABLE_B   = 0xFFE0u; // bits 5-15 (software-writable IMASK bits)
static constexpr uint32_t CEC_ILAT_W1C     = 0xFFEEu; // clearable ILAT bits (write-1-to-clear)

// SYSCFG constants
static constexpr uint32_t SYSCFG_SNEN = (1u << 2);  // self-nesting enable bit

// SEQSTAT.EXCAUSE mask (bits 5:0)
static constexpr uint32_t EXCAUSE_MASK = 0x3Fu;

// SP index into dpregs (PREG 6 = dpregs[14])
static constexpr uint32_t SP_IDX = 14;

// ── CEC-owned register state ──────────────────────────────────────────────────

static uint32_t s_cec_mmr[5];  // EVT_OVERRIDE, IMASK, IPEND, ILAT, IPRIO
static bool s_cec_pending; // Whether an interrupt is pending (set by cec_raise, cleared by cec_return_*)
static Memory* s_mem = nullptr; // Memory object for libgloss syscall fallback

// ── Storage-level interface ───────────────────────────────────────────────────

void cec_init() {
    s_cec_mmr[CEC_IDX_EVT_OVERRIDE] = 0u;
    s_cec_mmr[CEC_IDX_IMASK]        = CEC_UNMASKABLE_B;
    // Post-reset: IPEND has IVG_RST and IVG_IRPTEN set.
    // IVG_RST marks "in reset supervisor context".
    // IVG_IRPTEN globally masks maskable interrupts until the first RTI clears it.
    // Matches reference: bfin_cec_finish (dv-bfin_cec.c line 221).
    s_cec_mmr[CEC_IDX_IPEND]        = IVG_RST_B | IVG_IRPTEN_B;
    s_cec_mmr[CEC_IDX_ILAT]         = 0u;
    s_cec_mmr[CEC_IDX_IPRIO]        = 0u;
    s_cec_pending = false;
}

void cec_set_mem(Memory* mem) {
    s_mem = mem;
}

uint32_t cec_mmr_read(CpuState* cpu, uint32_t addr) {
    return s_cec_mmr[(addr - CEC_MMR_BASE) / 4];
}

void cec_mmr_write(CpuState* cpu, uint32_t addr, uint32_t val) {
    uint32_t idx = (addr - CEC_MMR_BASE) / 4;
    switch (idx) {
    case CEC_IDX_EVT_OVERRIDE:
        s_cec_mmr[idx] = val;
        break;
    case CEC_IDX_IMASK:
        // Only maskable bits are writable; unmaskable bits are preserved.
        s_cec_mmr[idx] = (val & CEC_MASKABLE_B) | (s_cec_mmr[idx] & CEC_UNMASKABLE_B);
        // Pending check
        s_cec_pending = true;
        break;
    case CEC_IDX_IPEND:
        // Read-only from software perspective — silently ignore.
        break;
    case CEC_IDX_ILAT:
        // Write-1-to-clear: set bits in val clear the corresponding ILAT bits.
        s_cec_mmr[idx] &= ~(val & CEC_ILAT_W1C);
        break;
    case CEC_IDX_IPRIO:
        // Only unmaskable bits stored.
        s_cec_mmr[idx] = val & CEC_UNMASKABLE_B;
        break;
    }
}

// ── Internal direct accessors (bypass software side-effect rules) ─────────────

static inline uint32_t& imask()        { return s_cec_mmr[CEC_IDX_IMASK]; }
static inline uint32_t& ipend()        { return s_cec_mmr[CEC_IDX_IPEND]; }
static inline uint32_t& ilat()         { return s_cec_mmr[CEC_IDX_ILAT];  }
static inline uint32_t& evt_override() { return s_cec_mmr[CEC_IDX_EVT_OVERRIDE]; }

static inline int current_ivg() { return cec_current_ivg_val(ipend()); }

// ── Mode helpers ──────────────────────────────────────────────────────────────

// Supervisor mode = any non-EMU, non-IRPTEN bit set in IPEND.
// Matches refs/dv-bfin_cec.c: _cec_is_supervisor_mode (excludes EMU and IRPTEN).
static constexpr uint32_t IPEND_BACKGROUND_B = IVG_EMU_B | IVG_IRPTEN_B;

static inline bool is_supervisor_mode() {
    return (ipend() & ~IPEND_BACKGROUND_B) != 0;
}

static inline bool is_user_mode() {
    return !is_supervisor_mode();
}

// Raises VEC_ILL_RES if currently in user mode (IPEND has no supervisor bits).
// Matches reference: cec_require_supervisor checks cec_is_user_mode (IPEND-based).
static void require_supervisor(CpuState* cpu) {
    if (is_user_mode())
        cec_exception(cpu, VEC_ILL_RES);
}

// User → Supervisor transition (refs/dv-bfin_cec.c lines 631-639):
//   - Poison hardware loop bottoms (set LSB).
//   - Save user SP into USP; load kernel SP from KSP.
static void enter_supervisor(CpuState* cpu) {
    for (int i = 0; i < 2; ++i) {
        if (!(cpu->lb[i] & 1))
            cpu->lb[i] |= 1u;
    }
    cpu->usp            = cpu->dpregs[SP_IDX];
    cpu->dpregs[SP_IDX] = cpu->ksp;
}

// Supervisor → User transition (refs/dv-bfin_cec.c lines 783-791):
//   - Depoison hardware loop bottoms (clear LSB).
//   - Save kernel SP into KSP; restore user SP from USP.
static void leave_supervisor(CpuState* cpu, uint32_t /*ivg*/) {
    for (int i = 0; i < 2; ++i) {
        if (cpu->lb[i] & 1)
            cpu->lb[i] &= ~1u;
    }
    cpu->ksp            = cpu->dpregs[SP_IDX];
    cpu->dpregs[SP_IDX] = cpu->usp;
}

// ── Core dispatch: _cec_raise_internal ────────────────────────────────────────
//
// Mirrors reference _cec_raise (dv-bfin_cec.c lines 488-643).
// ivg >= 0: explicit raise for that IVG level.
// ivg == -1: check-pending mode (scan ILAT & IMASK for highest-priority eligible).
// On dispatch: sets cpu->did_jump = true, cpu->pc = handler address.

static void _cec_raise_internal(CpuState* cpu, int ivg) {
    int curr_ivg = current_ivg();
    bool irpten = (ipend() & IVG_IRPTEN_B) != 0;
    bool snen   = (cpu->syscfg & SYSCFG_SNEN) != 0;

    if (curr_ivg == -1)
        curr_ivg = static_cast<int>(IVG_USER);

    // Check-pending mode: scan ILAT & IMASK for highest-priority eligible IVG.
    if (ivg == -1) {
        if (irpten)
            return;  // All interrupts globally masked.

        uint32_t pending = ilat() & imask();
        if (!pending)
            return;  // Nothing latched and enabled.

        ivg = __builtin_ctz(pending);  // Lowest bit = highest priority.
        if (ivg > curr_ivg)
            return;  // Nothing higher priority than current.

        if (!snen && ivg == curr_ivg)
            return;  // Self-nesting disabled.

        // Fall through to dispatch the pending IVG.
    }

    // Latch the IVG in ILAT.
    ilat() |= (1u << ivg);

    // Priority gate: determine whether to dispatch.
    bool do_dispatch = false;

    if (ivg <= static_cast<int>(IVG_EVX)) {
        // Unmaskable levels (EMU, RST, NMI, EVX).
        if (ivg == static_cast<int>(IVG_EMU) || ivg == static_cast<int>(IVG_RST)) {
            do_dispatch = true;
        } else if (curr_ivg <= ivg) {
            // Double fault.
            cpu->seqstat = (cpu->seqstat & ~EXCAUSE_MASK) | VEC_UNCOV;
            fprintf(stderr, "CEC: double fault at 0x%08x (curr_ivg=%d, ivg=%d)\n",
                    cpu->pc, curr_ivg, ivg);
            cpu->halted = true;
            cpu->exit_code = 1;
            return;
        } else {
            do_dispatch = true;
        }
    } else {
        // Maskable levels (5-15).
        if (irpten && curr_ivg != static_cast<int>(IVG_USER)) {
            // Globally masked (IRPTEN set and not in user mode).
        } else if (!(imask() & (1u << ivg))) {
            // Individually masked.
        } else if (ivg < curr_ivg || (snen && ivg == curr_ivg)) {
            do_dispatch = true;
        }
    }

    if (!do_dispatch)
        return;

    // ── Dispatch (process_int) ────────────────────────────────────────────────

    ipend() |= (1u << ivg);
    ilat()  &= ~(1u << ivg);

    uint32_t oldpc = cpu->pc;

    switch (ivg) {
    case static_cast<int>(IVG_EMU):
        cpu->rete = oldpc;
        // EMU trap: halt for debugger.
        cpu->halted = true;
        cpu->exit_code = 0;
        return;

    case static_cast<int>(IVG_RST):
        // Reset = shutdown.
        cpu->halted = true;
        cpu->exit_code = 0;
        return;

    case static_cast<int>(IVG_NMI):
        cpu->retn = oldpc;
        break;

    case static_cast<int>(IVG_EVX): {
        // Service exceptions (EXCAUSE <= 0x1f): RETX = next instruction (oldpc + 2).
        // Error exceptions (EXCAUSE >= 0x20): RETX = faulting instruction (oldpc).
        // EXCPT is always a 16-bit instruction so +2 is correct for service exceptions.
        uint32_t excause = cpu->seqstat & EXCAUSE_MASK;
        if (excause >= 0x20)
            cpu->retx = oldpc;
        else
            cpu->retx = hwloop_get_next_pc(cpu, oldpc, oldpc + cpu->insn_len);
        break;
    }

    case static_cast<int>(IVG_IRPTEN):
        fprintf(stderr, "CEC: RAISE 4 not supported\n");
        cpu->halted = true;
        cpu->exit_code = 1;
        return;

    default:
        // IVG5-15: RETI = oldpc | (1 if self-nesting same level).
        cpu->reti = oldpc | (ivg == curr_ivg ? 1u : 0u);
        break;
    }

    // Load PC from EVT (or reset EVT if EVT_OVERRIDE).
    if ((evt_override() & 0xff80u) & (1u << ivg))
        cpu->pc = 0xef000000u;  // Reset EVT address (reference cec_get_reset_evt).
    else
        cpu->pc = evt_get(static_cast<uint32_t>(ivg));

    cpu->did_jump = true;

    // Enable global interrupt mask upon interrupt entry (IVG5+).
    if (ivg >= static_cast<int>(IVG_IVHW))
        ipend() |= IVG_IRPTEN_B;

    // User → Supervisor transition if we were in user mode.
    if (curr_ivg == static_cast<int>(IVG_USER))
        enter_supervisor(cpu);
}

// ── EXCPT ─────────────────────────────────────────────────────────────────────

extern "C" void cec_exception(CpuState* cpu, uint32_t excp) {
    // Set SEQSTAT.EXCAUSE (bits 5:0).
    cpu->seqstat = (cpu->seqstat & ~EXCAUSE_MASK) | (excp & EXCAUSE_MASK);

    if (excp <= 0x3Fu) {
        // If EXCPT 0 and no EVT3 handler is installed, use the libgloss syscall
        // fallback. This preserves compatibility with tests that call EXCPT 0 from
        // supervisor mode without setting up an EVT3 handler.
        if (excp == 0 && evt_get(IVG_EVX) == 0) {
            if (s_mem)
                bfin_syscall(cpu, s_mem);
            return;
        }
        // Dispatch to EVT3 (exception handler).
        _cec_raise_internal(cpu, static_cast<int>(IVG_EVX));
    }
}

// ── RAISE ─────────────────────────────────────────────────────────────────────

extern "C" void cec_raise(CpuState* cpu, uint32_t ivg) {
    if (ivg > IVG15)
        return;
    // RAISE is a supervisor-mode instruction. In user mode it causes VEC_ILL_RES.
    // cpu->pc was pre-set to the RAISE instruction address by the JIT.
    require_supervisor(cpu);
    if (cpu->did_jump) return;  // exception was raised in user mode
    // Latch the IVG in ILAT, then check for pending dispatch.
    // This matches the reference's cec_latch() pattern.
    ilat() |= (1u << ivg);
    s_cec_pending = true;
}

// ── Return instructions ───────────────────────────────────────────────────────

// Read the return register for a given IVG level.
static uint32_t read_ret_reg(CpuState* cpu, int ivg) {
    switch (ivg) {
    case static_cast<int>(IVG_EMU): return cpu->rete;
    case static_cast<int>(IVG_NMI): return cpu->retn;
    case static_cast<int>(IVG_EVX): return cpu->retx;
    default:                         return cpu->reti;
    }
}

// Shared return logic for RTI/RTX/RTN/RTE.
// ivg: -1 = RTI (auto-detect current IVG), else specific IVG for RTX/RTN/RTE.
// Returns the new PC.
static uint32_t cec_return_common(CpuState* cpu, int ivg) {
    // Reference: cec_return (dv-bfin_cec.c lines 683-795).

    // Always clear EMU bit (reference line 704).
    ipend() &= ~IVG_EMU_B;

    int curr_ivg = current_ivg();
    if (curr_ivg == -1)
        curr_ivg = static_cast<int>(IVG_USER);

    if (ivg == -1)
        ivg = curr_ivg;  // RTI: return from current IVG.

    // Cannot return from user mode.
    if (curr_ivg == static_cast<int>(IVG_USER)) {
        cec_exception(cpu, VEC_ILL_RES);
        return cpu->pc;
    }

    // Require supervisor mode.
    require_supervisor(cpu);

    // Validate return type matches current context.
    switch (ivg) {
    case static_cast<int>(IVG_EMU):
        if (curr_ivg != static_cast<int>(IVG_EMU)) {
            cec_exception(cpu, VEC_ILL_RES);
            return cpu->pc;
        }
        break;
    case static_cast<int>(IVG_NMI):
        if (curr_ivg != static_cast<int>(IVG_NMI)) {
            cec_exception(cpu, VEC_ILL_RES);
            return cpu->pc;
        }
        break;
    case static_cast<int>(IVG_EVX):
        if (curr_ivg != static_cast<int>(IVG_EVX)) {
            cec_exception(cpu, VEC_ILL_RES);
            return cpu->pc;
        }
        break;
    case static_cast<int>(IVG_IRPTEN):
        fprintf(stderr, "CEC: return from IVG_IRPTEN not supported\n");
        cpu->halted = true;
        cpu->exit_code = 1;
        return cpu->pc;
    default:
        // RTI: not valid from EMU, NMI, EVX, or user mode.
        if (curr_ivg == static_cast<int>(IVG_EMU) ||
            curr_ivg == static_cast<int>(IVG_NMI) ||
            curr_ivg == static_cast<int>(IVG_EVX) ||
            curr_ivg == static_cast<int>(IVG_USER)) {
            cec_exception(cpu, VEC_ILL_RES);
            return cpu->pc;
        }
        break;
    }

    uint32_t newpc = read_ret_reg(cpu, ivg);

    // Extract self-nesting bit from LSB.
    bool snen_bit = (newpc & 1u) != 0;
    newpc &= ~1u;

    // Clear IPEND bit for this IVG (unless self-nesting return).
    if (!snen_bit)
        ipend() &= ~(1u << ivg);

    // Disable global interrupt mask when returning from IVG5+ or IVG_RST.
    if (ivg >= static_cast<int>(IVG_IVHW) || ivg == static_cast<int>(IVG_RST))
        ipend() &= ~IVG_IRPTEN_B;

    // Check if we're returning to user mode (no supervisor bits remain).
    if (current_ivg() == -1)
        leave_supervisor(cpu, static_cast<uint32_t>(ivg));

    // Set cpu->pc to the return address before checking for pending interrupts.
    // This ensures that if a pending interrupt dispatches here, its RETI/RETN
    // is set to "the instruction about to execute" (= the return address),
    // matching the reference: "interrupts are processed in between insns which
    // means the return point is the insn-to-be-executed (which is the current PC)".
    cpu->pc = newpc;

    // Check for pending interrupts now eligible after return (matches reference
    // dv-bfin_cec.c line 794: _cec_check_pending called at end of cec_return).
    _cec_raise_internal(cpu, -1);
    if (cpu->did_jump)
        newpc = cpu->pc;  // An interrupt dispatched; return new PC from that.

    return newpc;
}

extern "C" uint32_t cec_return_rti(CpuState* cpu) {
    return cec_return_common(cpu, -1);
}

extern "C" uint32_t cec_return_rtx(CpuState* cpu) {
    return cec_return_common(cpu, static_cast<int>(IVG_EVX));
}

extern "C" uint32_t cec_return_rtn(CpuState* cpu) {
    return cec_return_common(cpu, static_cast<int>(IVG_NMI));
}

extern "C" uint32_t cec_return_rte(CpuState* cpu) {
    return cec_return_common(cpu, static_cast<int>(IVG_EMU));
}

// ── CLI / STI ─────────────────────────────────────────────────────────────────

extern "C" uint32_t cec_cli(CpuState* cpu) {
    require_supervisor(cpu);
    if (cpu->did_jump) return 0;  // Exception was raised; don't continue.
    uint32_t old_mask = imask();
    // Clear all maskable bits in IMASK.
    imask() = imask() & ~IVG_MASKABLE_B;
    return old_mask;
}

extern "C" void cec_sti(CpuState* cpu, uint32_t mask) {
    require_supervisor(cpu);
    // Write maskable bits from value; preserve unmaskable bits.
    imask() = (mask & IVG_MASKABLE_B) | (imask() & IVG_UNMASKABLE_B);
    // Check for pending interrupts that are now eligible.
    s_cec_pending = true;
}

// ── Push/Pop RETI ─────────────────────────────────────────────────────────────

extern "C" void cec_push_reti(CpuState* cpu) {
    require_supervisor(cpu);
    if (cpu->did_jump) return;  // Exception was raised; don't continue.
    // Clear IRPTEN: enable nested interrupts.
    ipend() &= ~IVG_IRPTEN_B;
    s_cec_pending = true;
}

extern "C" void cec_pop_reti(CpuState* cpu) {
    require_supervisor(cpu);
    if (cpu->did_jump) return;  // Exception was raised; don't continue.
    // Set IRPTEN: restore global interrupt mask.
    ipend() |= IVG_IRPTEN_B;
}

// ── Supervisor check ──────────────────────────────────────────────────────────

extern "C" bool cec_is_user_mode() {
    return is_user_mode();
}

extern "C" void cec_check_sup(CpuState* cpu) {
    require_supervisor(cpu);
}

// ── Pending interrupt check ───────────────────────────────────────────────────

extern "C" void cec_check_pending(CpuState* cpu) {
    if (s_cec_pending) {
        _cec_raise_internal(cpu, -1);
        s_cec_pending = false;
    }
}

extern "C" __attribute__((hot)) bool user_mmr_check(CpuState* cpu, uint32_t addr) {
    if (is_user_mode() && addr >= 0xFFC00000u) {
        cec_exception(cpu, VEC_DCPLB_MISS);
        return true;
    }
    return false;
}
