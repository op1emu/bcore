#pragma once

#include <llvm/IR/PassManager.h>

// Loops that wait. A loop block's loop that polls -- a device register, a
// flag in memory -- does the same thing every iteration until something
// outside it changes, and nothing outside it changes while the run lasts:
// the host's devices change only between runs (or in the memory callbacks,
// which say so), and no other code runs. Such an iteration differs from the
// next only in its counters: the packets it retired, a hardware loop's
// count, a pointer it steps. SteadyLoopSkip gives the loop a test, at its
// latch, that the iteration just run was such an iteration; where it was,
// every later one is too until the first exit the loop counts (the run's
// budget, a hardware loop's end, a counted compare), and the loop goes
// straight to that last iteration, with each counter at its value there
// (ScalarEvolution's evaluateAtIteration), and runs it as always: it exits
// where it would have, with the same packets, registers and memory.
//
// An iteration is steady when every header PHI but the counters comes back
// to its value, no memory callback it made was unstable (the host counts
// reads it cannot promise would repeat in CpuState::unstable_reads), and it
// can have no other effect: the loop stores only to CpuState and calls only
// the memory read callbacks. The counters are the header PHIs ScalarEvolution
// finds affine in the loop; their values may feed only the loop's counted
// exits and stores to CpuState, so that skipping their intermediate values
// changes nothing else. A load from a CpuState field the loop also stores
// to, or an exit the loop cannot count whose condition depends on a counter,
// keeps the loop out.
//
// Another thread that stops the run (it sets the budget to 0) is seen at the
// last iteration rather than the next one: as if it had stopped the run that
// much later.
//
// dry: the latch counts the steady iterations it would have skipped into
// CpuState::steady_iterations instead, and the loop runs as before.
struct SteadyLoopSkip : llvm::PassInfoMixin<SteadyLoopSkip> {
    explicit SteadyLoopSkip(bool dry) : dry_(dry) {}
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& am);

private:
    bool dry_;
};

// The function attribute counting the loops given the test (BcoreStats).
constexpr const char* kLoopsSteadyAttr = "bcore-loops-steady";
