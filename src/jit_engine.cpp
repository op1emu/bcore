#include "jit_engine.h"

#include <cstdio>

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

#include "syscall_emu.h"
#include "cec.h"
#include "cpu_state.h"

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() = default;

// Helper to create a JITEvaluatedSymbol from a function pointer
static llvm::JITEvaluatedSymbol sym_from_ptr(void* ptr) {
    return llvm::JITEvaluatedSymbol(
        llvm::pointerToJITTargetAddress(ptr),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
}

bool JitEngine::init(int opt_level) {
    opt_level_ = opt_level;
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    llvm::CodeGenOpt::Level cg_level =
        opt_level == 0 ? llvm::CodeGenOpt::None :
        opt_level == 1 ? llvm::CodeGenOpt::Less :
        opt_level == 3 ? llvm::CodeGenOpt::Aggressive :
                         llvm::CodeGenOpt::Default;

    auto jtmb_or_err = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb_or_err) {
        llvm::errs() << "Failed to detect host: " << jtmb_or_err.takeError() << "\n";
        return false;
    }
    jtmb_or_err->setCodeGenOptLevel(cg_level);

    auto jit_or_err = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb_or_err))
        .create();
    if (!jit_or_err) {
        llvm::errs() << "Failed to create LLJIT: " << jit_or_err.takeError() << "\n";
        return false;
    }
    jit_ = std::move(*jit_or_err);

    // Pre-allocate hash buckets to avoid rehash overhead during execution.
    cache_.reserve(1024);

    // Manually register all extern "C" symbols that JIT'd code may call.
    // This is more reliable than DynamicLibrarySearchGenerator which depends
    // on -rdynamic / ENABLE_EXPORTS.
    auto& es = jit_->getExecutionSession();
    auto& jd = jit_->getMainJITDylib();

    llvm::orc::SymbolMap symbols;
    symbols[es.intern("mem_read8")]           = sym_from_ptr(reinterpret_cast<void*>(&mem_read8));
    symbols[es.intern("mem_read16")]          = sym_from_ptr(reinterpret_cast<void*>(&mem_read16));
    symbols[es.intern("mem_read32")]          = sym_from_ptr(reinterpret_cast<void*>(&mem_read32));
    symbols[es.intern("mem_write8")]          = sym_from_ptr(reinterpret_cast<void*>(&mem_write8));
    symbols[es.intern("mem_write16")]         = sym_from_ptr(reinterpret_cast<void*>(&mem_write16));
    symbols[es.intern("mem_write32")]         = sym_from_ptr(reinterpret_cast<void*>(&mem_write32));
    symbols[es.intern("bfin_syscall")]        = sym_from_ptr(reinterpret_cast<void*>(&bfin_syscall));
    symbols[es.intern("bfin_putchar")]        = sym_from_ptr(reinterpret_cast<void*>(&bfin_putchar));
    symbols[es.intern("cec_exception")]       = sym_from_ptr(reinterpret_cast<void*>(&cec_exception));
    symbols[es.intern("cec_raise")]           = sym_from_ptr(reinterpret_cast<void*>(&cec_raise));
    symbols[es.intern("cec_return_rti")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rti));
    symbols[es.intern("cec_return_rtx")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rtx));
    symbols[es.intern("cec_return_rtn")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rtn));
    symbols[es.intern("cec_return_rte")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rte));
    symbols[es.intern("cec_cli")]             = sym_from_ptr(reinterpret_cast<void*>(&cec_cli));
    symbols[es.intern("cec_sti")]             = sym_from_ptr(reinterpret_cast<void*>(&cec_sti));
    symbols[es.intern("cec_push_reti")]       = sym_from_ptr(reinterpret_cast<void*>(&cec_push_reti));
    symbols[es.intern("cec_pop_reti")]        = sym_from_ptr(reinterpret_cast<void*>(&cec_pop_reti));
    symbols[es.intern("cec_check_sup")]       = sym_from_ptr(reinterpret_cast<void*>(&cec_check_sup));
    symbols[es.intern("cec_is_user_mode")]    = sym_from_ptr(reinterpret_cast<void*>(&cec_is_user_mode));
    symbols[es.intern("cec_check_pending")]   = sym_from_ptr(reinterpret_cast<void*>(&cec_check_pending));
    symbols[es.intern("bfin_hwloop_step")]    = sym_from_ptr(reinterpret_cast<void*>(&bfin_hwloop_step));

    if (auto err = jd.define(llvm::orc::absoluteSymbols(symbols))) {
        llvm::errs() << "Failed to register symbols: " << err << "\n";
        return false;
    }

    return true;
}

void JitEngine::optimize_module(llvm::Module& mod) {
    if (opt_level_ == 0) return;

    llvm::OptimizationLevel lvl =
        opt_level_ == 1 ? llvm::OptimizationLevel::O1 :
        opt_level_ == 3 ? llvm::OptimizationLevel::O3 :
                          llvm::OptimizationLevel::O2;

    llvm::LoopAnalysisManager     lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager    cgam;
    llvm::ModuleAnalysisManager   mam;

    pb_.registerModuleAnalyses(mam);
    pb_.registerCGSCCAnalyses(cgam);
    pb_.registerFunctionAnalyses(fam);
    pb_.registerLoopAnalyses(lam);
    pb_.crossRegisterProxies(lam, fam, cgam, mam);

    // Rebuild per-module pipeline each call (analysis managers are fresh).
    // passBuilder itself is reused across calls (avoids plugin registration overhead).
    llvm::ModulePassManager mpm = pb_.buildPerModuleDefaultPipeline(lvl);
    mpm.run(mod, mam);
}

bool JitEngine::addModule(uint32_t pc, std::unique_ptr<llvm::Module> module,
                          std::unique_ptr<llvm::LLVMContext> ctx,
                          uint32_t fallthrough_pc) {
    auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx));

    // Create a per-module ResourceTracker so we can remove it on invalidation.
    auto rt = jit_->getMainJITDylib().createResourceTracker();

    if (auto err = jit_->addIRModule(rt, std::move(tsm))) {
        llvm::errs() << "Failed to add module for PC=0x" << llvm::format_hex(pc, 8)
                     << ": " << err << "\n";
        return false;
    }

    // Look up the function
    char name[32];
    snprintf(name, sizeof(name), "bb_0x%08x", pc);

    auto sym = jit_->lookup(name);
    if (!sym) {
        llvm::errs() << "Failed to lookup " << name << ": " << sym.takeError() << "\n";
        return false;
    }
    auto fn = reinterpret_cast<BbFunc>(sym->getValue());
    cache_[pc] = BBInfo{fn, fallthrough_pc, std::move(rt)};
    return true;
}

BbFunc JitEngine::lookup(uint32_t pc) {
    auto it = cache_.find(pc);
    if (it != cache_.end())
        return it->second.fn;
    return nullptr;
}

void JitEngine::invalidate(uint32_t addr, uint32_t size) {
    if (size == 0) return;
    uint32_t write_end = addr + size;

    // Collect PCs to evict (can't erase while iterating)
    std::vector<uint32_t> to_evict;
    for (auto& [pc, info] : cache_) {
        // BB occupies [pc, fallthrough_pc); overlap if intervals are not disjoint
        if (addr < info.fallthrough_pc && write_end > pc) {
            if (pc == executing_bb_pc_) continue;  // skip self-invalidation
            to_evict.push_back(pc);
        }
    }

    for (uint32_t pc : to_evict) {
        auto it = cache_.find(pc);
        if (it == cache_.end()) continue;
        // Defer rt->remove() — JIT'd code may still be on the call stack
        if (it->second.rt)
            pending_remove_.push_back(std::move(it->second.rt));
        cache_.erase(it);
    }
}

void JitEngine::flush_pending_removes() {
    for (auto& rt : pending_remove_) {
        if (auto err = rt->remove())
            llvm::consumeError(std::move(err));
    }
    pending_remove_.clear();
}
