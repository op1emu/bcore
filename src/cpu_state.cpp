#include "cpu_state.h"
#include "cec.h"
#include "evt.h"

extern "C" {

uint32_t cpu_astat_compose(CpuState* cpu) {
    return astat_compose(cpu);
}

void cpu_astat_decompose(CpuState* cpu, uint32_t astat) {
    astat_decompose(cpu, astat);
}

// Called once per Blackfin instruction by JIT'd code.
// Phase A (when !did_jump): update cpu->pc following hw-loop back-edge rules.
// Phase B (always):         decrement loop counter(s) whose bottom == insn_pc.
void bfin_hwloop_step(CpuState* cpu, uint32_t insn_pc, uint32_t fallthrough_pc) {
    // Phase A: PC redirect — only when the instruction itself didn't branch
    if (!cpu->did_jump) {
        uint32_t new_pc = fallthrough_pc;
        // Loop 0 (lower priority)
        if (cpu->lc[0] > 1 && cpu->lb[0] == insn_pc)
            new_pc = cpu->lt[0];
        // Loop 1 (higher priority, overwrites)
        if (cpu->lc[1] > 1 && cpu->lb[1] == insn_pc)
            new_pc = cpu->lt[1];
        cpu->pc = new_pc;
        if (new_pc != fallthrough_pc)
            cpu->did_jump = true;
    }
    // Phase B: counter decrement — runs even after a taken branch
    // Loop 1 has higher priority; if still iterating after decrement skip loop 0.
    if (cpu->lc[1] > 0 && cpu->lb[1] == insn_pc) {
        if (--cpu->lc[1] != 0) return;
    }
    if (cpu->lc[0] > 0 && cpu->lb[0] == insn_pc)
        --cpu->lc[0];
}

} // extern "C"

void cpu_state_init(CpuState* cpu) {
    std::memset(cpu, 0, sizeof(CpuState));
    cec_init();
    evt_init();
}

uint32_t hwloop_get_next_pc(CpuState* cpu, uint32_t insn_pc, uint32_t next_pc) {
    // This is called from cec_exception for EVX exceptions to determine the next PC
    // when RETX should point to the instruction after the faulting one (service exceptions).
    // It applies the same back-edge rules as bfin_hwloop_step.
    if (cpu->lc[1] > 0 && cpu->lb[1] == insn_pc)
        return cpu->lt[1];
    if (cpu->lc[0] > 0 && cpu->lb[0] == insn_pc)
        return cpu->lt[0];
    return next_pc;
}