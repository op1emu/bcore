#pragma once

#include <llvm/IR/PassManager.h>

// Speculative copies of saturating accumulations, so that a loop the
// vectorizers cannot touch -- each step clamps a running sum, which is no
// associative reduction -- gets one they can.
//
// A guest MAC with 32-bit saturation (W32) adds its product to the
// accumulator and clamps the sum to the signed 32-bit window: in the IR an
// accumulator PHI whose next value is
//
//   select(S fits in i32, S, clamp)  with  S = acc + x
//
// where x does not depend on the accumulator. If |acc0| + sum(|x|) over the
// iterations is at most INT32_MAX, no partial sum leaves the window, no step
// clamps, and the accumulator after m steps is acc0 + sum(x): an add
// reduction. SatReductionSpeculation gives such a loop, for its first m
// iterations (m its symbolic maximum backedge-taken count, which no exit the
// loop counts can cut short), a copy that computes exactly that, with the
// test "S fits" taken to be true everywhere it is used and sum(|x|) alongside;
// it counts its iterations and has no other exit (one that only sees whether
// another thread stopped the run, an atomic load of the chain budget, it
// leaves out: the run then returns a little later, as when the stop came
// later). Where the bound holds, the loop resumes at iteration m with the
// copy's values, so the loop itself still runs the last iteration and every
// exit; where it does not, the loop runs from its entry as if the copy had
// not been there. The copy's latch carries llvm.loop.vectorize.enable.
//
// The loops it applies to have no stores and no calls (a proven copy of a
// loop block's loop, GuestLoopVersioning, often has neither), every exit
// counted or the stop test, and every accumulator's other uses dead once its
// steps cannot clamp.
struct SatReductionSpeculation : llvm::PassInfoMixin<SatReductionSpeculation> {
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& am);
};
