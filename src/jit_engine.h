#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>

#include "cpu_state.h"
#include "mem.h"

// Function signature for JIT'd basic block functions
using BbFunc = void (*)(CpuState*, Memory*);

struct BBInfo {
    BbFunc fn = nullptr;
    uint32_t fallthrough_pc = 0;
    llvm::orc::ResourceTrackerSP rt;  // per-module tracker for removal
    BBInfo() = default;
    BBInfo(BbFunc f, uint32_t fpc, llvm::orc::ResourceTrackerSP r)
        : fn(f), fallthrough_pc(fpc), rt(std::move(r)) {}
};

class BBTranslator;

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    // opt_level: 0=None, 1=Less, 2=Default, 3=Aggressive
    bool init(int opt_level = 2);
    // Optimize a module in-place using the same pipeline level as init().
    // No-op when opt_level is 0.
    void optimize_module(llvm::Module& mod);
    bool addModule(uint32_t pc, std::unique_ptr<llvm::Module> module,
                   std::unique_ptr<llvm::LLVMContext> ctx,
                   uint32_t fallthrough_pc);
    BbFunc lookup(uint32_t pc);
    void invalidate(uint32_t addr, uint32_t size);
    void flush_pending_removes();
    void set_executing(uint32_t pc) { executing_bb_pc_ = pc; }
    void clear_executing()          { executing_bb_pc_ = 0; }
    bool has_pending_removes() const { return !pending_remove_.empty(); }

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::unordered_map<uint32_t, BBInfo> cache_;
    int opt_level_ = 2;
    std::vector<llvm::orc::ResourceTrackerSP> pending_remove_;
    uint32_t executing_bb_pc_ = 0;  // PC of currently-executing BB (0 = none)
    // Cached pass pipeline (built once in init(), reused across optimize_module() calls)
    llvm::PassBuilder pb_;
};
