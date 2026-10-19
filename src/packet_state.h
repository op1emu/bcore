#pragma once

namespace llvm {
class Function;
class Value;
}

// Writes CpuState::pc, insn_len and packets only where something can read
// them.
//
// The lifter stores all three as it goes: insn_len and packets at the top of
// every packet, pc at the end of every packet that falls through. Nearly all
// of those values are compile-time constants that nobody sees: generated code
// never loads them, and the host reads them only inside a helper call (a
// memory callback asks the time, an exception reads pc and insn_len, an
// interrupt takes pc as its return address) or after the block returns. This
// pass deletes each constant store and stores the constant again right before
// every reader it reaches -- a call, a return, a load -- provided every
// definition reaching that reader is the same constant. Where definitions
// disagree (a join of two different PCs, or of a store and a helper's own
// write) the stores stay where they are, so the memory image at every reader
// is exactly what it was.
//
// The model of what a helper does to pc (see packet_state.cpp) is the one
// non-local fact: a helper writes pc only when it also sets did_jump, which
// generated code tests right after the call; bfin_hwloop_step also writes the
// fall-through PC when it does not jump; cec_return_* and anything unknown
// may write it at any time.
//
// Runs on the lifted function before the chain exit is built, so every
// return is still a ReturnInst.
void sink_packet_state(llvm::Function& fn, llvm::Value* cpu);
