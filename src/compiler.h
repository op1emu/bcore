#pragma once

#include <memory>
#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

namespace llvm { class MCContext; class TargetMachine; }

// What a compiled module is made of (BcoreCompileMetrics, BcoreStats).
struct ModuleCounts {
    uint64_t ir_instructions = 0, ir_blocks = 0;
    uint64_t loop_blocks = 0, loops_proven = 0, loops_speculated = 0, loops_vectorized = 0;
    uint64_t loops_steady = 0, dag_circular = 0;
};
ModuleCounts count_module(const llvm::Module& mod);

// Turns a block's module into an object file: the IR pipeline, then the
// backend. Everything a module does not change is built once and kept: the
// TargetMachine, the pass builder and its analysis managers, and the
// backend's pass pipeline (legacy PassManager with addPassesToEmitMC). ORC's
// SimpleCompiler builds that pipeline again for every module; on a boot of
// 21,000 one-block modules that was 6.7 s of 229 s of compilation, and
// creating and tearing down its pass manager and MCContext 2.4 s more.
// Running one legacy pass manager on module after module is what
// MachineModuleInfoWrapperPass supports: doInitialization and
// doFinalization start and reset its MachineModuleInfo and MCContext for
// each module (Instrew's code generator compiles every function this way).
//
// Single-threaded: X86's subtarget cache in TargetMachine is unlocked, so a
// thread that compiles has a Compiler of its own.
class Compiler {
public:
    Compiler();
    ~Compiler();
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    // opt_level: the IR pipeline, 0=None, 1=O1, 2=O2, 3=O3.
    // codegen_level: the backend on the same scale; <0 follows opt_level.
    // split: the machine function splitter moves cold paths apart (init()).
    // stencil: for the copy-and-patch tier's stencils (tools/stencilgen.cpp)
    // rather than for this host: generic x86-64 with the small code model
    // and static relocations, which fold each hole into the instruction
    // that uses it, and no vectorizers (whose constants would be data).
    bool init(int opt_level, int codegen_level, bool split = true, bool stencil = false);
    // The IR pipeline, in place; at opt_level 0 only a loop block's guest
    // memory lowered (guest_memory.h). Every module goes through it before
    // emit.
    void optimize(llvm::Module& mod);
    // The module's object file, from the backend; null if that fails. The
    // module is left as compiled.
    std::unique_ptr<llvm::MemoryBuffer> emit(llvm::Module& mod);

    // A pass pipeline (PassBuilder text) optimize runs after the default one;
    // empty for none. False, and unchanged, if it does not parse.
    bool set_post_pipeline(const std::string& pipeline);
    // SteadyLoopSkip on loop blocks (steady_skip.h): 0 off, 1 dry, 2 on.
    void set_steady_skip(int mode) { steady_skip_ = mode; }

    // What modules must carry for this compiler (BBTranslator::setModuleTarget).
    std::string dataLayout() const;
    std::string targetTriple() const;

private:
    int opt_level_ = 2;
    bool split_ = true;
    int steady_skip_ = 0;
    std::string post_pipeline_;
    std::unique_ptr<llvm::TargetMachine> machine_;
    // The IR pipeline. The analysis managers hold results for the module
    // being optimized only: optimize clears them before it returns.
    std::unique_ptr<llvm::PassBuilder> pb_;
    llvm::LoopAnalysisManager lam_;
    llvm::FunctionAnalysisManager fam_;
    llvm::CGSCCAnalysisManager cgam_;
    llvm::ModuleAnalysisManager mam_;
    // The backend, writing into object_.
    llvm::SmallVector<char, 0> object_;
    std::unique_ptr<llvm::raw_svector_ostream> stream_;
    std::unique_ptr<llvm::legacy::PassManager> backend_;
    llvm::MCContext* mc_ = nullptr;
};
