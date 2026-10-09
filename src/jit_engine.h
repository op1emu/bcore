#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>

#include "cpu_state.h"
#include "mem.h"

namespace llvm { class TargetMachine; }

// Function signature for JIT'd basic block functions
using BbFunc = void (*)(CpuState*, Memory*);

struct BBInfo {
    BbFunc fn = nullptr;
    uint32_t fallthrough_pc = 0;
    BBInfo() = default;
    BBInfo(BbFunc f, uint32_t fpc) : fn(f), fallthrough_pc(fpc) {}
};

class BBTranslator;

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    // opt_level: the IR pipeline, 0=None, 1=O1, 2=O2, 3=O3.
    // codegen_level: the backend on the same scale; <0 follows opt_level.
    bool init(int opt_level = 2, int codegen_level = -1);
    // Optimize a module in-place using the same pipeline level as init().
    // No-op when opt_level is 0. Core::run calls it on every translated module
    // before addModule; before that, only the IR-dump path did, so opt_level
    // never reached executed code.
    void optimize_module(llvm::Module& mod);
    // The layout and triple LLJIT compiles for. A module carrying exactly these
    // passes addIRModule's check; any other non-default layout is an error.
    std::string dataLayout() const { return jit_->getDataLayout().getStringRepresentation(); }
    std::string targetTriple() const { return jit_->getTargetTriple().str(); }
    bool addModule(uint32_t pc, std::unique_ptr<llvm::Module> module,
                   std::unique_ptr<llvm::LLVMContext> ctx,
                   uint32_t fallthrough_pc);
    BbFunc lookup(uint32_t pc);
    void set_executing(uint32_t pc) { executing_bb_pc_ = pc; }
    void clear_executing()          { executing_bb_pc_ = 0; }

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::unordered_map<uint32_t, BBInfo> cache_;
    int opt_level_ = 2;
    uint32_t executing_bb_pc_ = 0;  // PC of currently-executing BB (0 = none)
    // Cached pass pipeline (built once in init(), reused across optimize_module() calls).
    // pass_machine_ outlives pb_, which holds it.
    std::unique_ptr<llvm::TargetMachine> pass_machine_;
    std::unique_ptr<llvm::PassBuilder> pb_;
};
