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

namespace llvm { class JITEventListener; }

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

    // opt_level: 0=None, 1=Less, 2=Default, 3=Aggressive
    bool init(int opt_level = 2);
    // Optimize a module in-place using the same pipeline level as init().
    // No-op when opt_level is 0.
    void optimize_module(llvm::Module& mod);
    bool addModule(uint32_t pc, std::unique_ptr<llvm::Module> module,
                   std::unique_ptr<llvm::LLVMContext> ctx,
                   uint32_t fallthrough_pc);
    BbFunc lookup(uint32_t pc);
    void set_executing(uint32_t pc) { executing_bb_pc_ = pc; }
    void clear_executing()          { executing_bb_pc_ = 0; }

    // Register L on the underlying object-linking layer for object load/free
    // notifications (the profiling code map builds itself from these).
    // Behavior-neutral on x86-64 Linux where LLJIT defaults to
    // RTDyldObjectLinkingLayer; returns false (and registers nothing) on any
    // other layer so callers can degrade gracefully.
    bool registerJITEventListener(llvm::JITEventListener& L);

    // Iterate all cached blocks as fn(host_addr, guest_pc). Addresses come
    // from the symbol lookup, sizes are not tracked here — use the load
    // listener (via Core::forEachCompiledBlock) when exact sizes are needed.
    template <typename Fn>
    void forEachCompiled(Fn&& fn) const {
        for (const auto& kv : cache_)
            fn(reinterpret_cast<const void*>(kv.second.fn), kv.first);
    }

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::unordered_map<uint32_t, BBInfo> cache_;
    int opt_level_ = 2;
    uint32_t executing_bb_pc_ = 0;  // PC of currently-executing BB (0 = none)
    // Cached pass pipeline (built once in init(), reused across optimize_module() calls)
    llvm::PassBuilder pb_;
};
