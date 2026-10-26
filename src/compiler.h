#pragma once

#include <memory>
#include <string>
#include <utility>

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
    // A function the splitter leaves whole (it gets no entry count): code
    // copied as one piece (the copy-and-patch exits an arena shares).
    static constexpr const char* kNoSplitAttr = "bcore-no-split";
    Compiler();
    ~Compiler();
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    // opt_level: the IR pipeline, 0=None, 1=O1, 2=O2, 3=O3.
    // codegen_level: the backend on the same scale; <0 follows opt_level.
    // split: the machine function splitter moves cold paths apart (init()).
    // stencil_triple: for the copy-and-patch tier's stencils
    // (tools/stencilgen.cpp, cp_build.h's kStencilTriple*) rather than for
    // this host: the generic CPU of that triple's architecture, whose target
    // the caller has initialized, with the small code model and static
    // relocations, which on x86-64 fold each hole into the instruction that
    // uses it, and no vectorizers (whose constants would be data). On AArch64
    // X18 is kept for the platform (Apple's ABI) and atomics are not called
    // out of line. The splitter is on for stencils of any architecture, and
    // off on hosts other than x86-64.
    bool init(int opt_level, int codegen_level, bool split = true, const char* stencil_triple = nullptr);
    // Before init: a CPU and its features (SubtargetFeatures text, "+avx2,...")
    // in place of the host's, for code that is compiled here and never run
    // (tests/vectorize_count_test.cpp compiles for AVX-512 on any x86-64).
    void set_target_cpu(std::string cpu, std::string features) {
        cpu_ = std::move(cpu);
        features_ = std::move(features);
    }
    // The IR pipeline, in place; at opt_level 0 only a loop block's guest
    // memory lowered (guest_memory.h). Every module goes through it before
    // emit. light: a module without a loop block gets kLightPipeline instead
    // of the default pipeline (a promotion, Core::set_light_tier1): its
    // straight-line code needs none of the module, inliner and loop passes.
    void optimize(llvm::Module& mod, bool light = false);
    static const char* const kLightPipeline;
    // The module's object file, from the backend; null if that fails. The
    // module is left as compiled.
    std::unique_ptr<llvm::MemoryBuffer> emit(llvm::Module& mod);

    // A pass pipeline (PassBuilder text) optimize runs after the default one;
    // empty for none. False, and unchanged, if it does not parse.
    bool set_post_pipeline(const std::string& pipeline);
    // SteadyLoopSkip on loop blocks (steady_skip.h): 0 off, 1 dry, 2 on.
    void set_steady_skip(int mode) { steady_skip_ = mode; }

    // Whether the splitter is on (init's split, as the target allows).
    bool split() const { return split_; }
    // What modules must carry for this compiler (BBTranslator::setModuleTarget).
    std::string dataLayout() const;
    std::string targetTriple() const;

private:
    int opt_level_ = 2;
    bool split_ = true;
    int steady_skip_ = 0;
    std::string post_pipeline_;
    std::string cpu_, features_;
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
