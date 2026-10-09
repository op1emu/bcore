#include "jit_engine.h"

#include <cstdio>
#include <cstring>

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Analysis/TargetTransformInfo.h>
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

bool JitEngine::init(int opt_level, int codegen_level) {
    opt_level_ = opt_level;
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // The IR pipeline and the backend are separate knobs: the backend level
    // decides instruction selection and register allocation, the IR pipeline
    // what the backend is given, and each has its own compile-time cost.
    const int cg = codegen_level < 0 ? opt_level : codegen_level;
    llvm::CodeGenOpt::Level cg_level =
        cg <= 0 ? llvm::CodeGenOpt::None :
        cg == 1 ? llvm::CodeGenOpt::Less :
        cg == 3 ? llvm::CodeGenOpt::Aggressive :
                  llvm::CodeGenOpt::Default;

    auto jtmb_or_err = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb_or_err) {
        llvm::errs() << "Failed to detect host: " << jtmb_or_err.takeError() << "\n";
        return false;
    }
    jtmb_or_err->setCodeGenOptLevel(cg_level);

    // The IR pipeline's TargetMachine comes from a copy of the compiler's
    // builder, so its TTI describes exactly the CPU and features the code will
    // be generated for. Without one, PassBuilder's TargetIRAnalysis is the
    // generic implementation: zero vector registers for LoopVectorize/SLP, no
    // x86 unrolling preferences, generic SimplifyCFG/LICM/JumpThreading costs.
    // Single-threaded use only, like all of bcore's compilation (X86's
    // subtarget cache in TargetMachine is unlocked).
    {
        auto machine = llvm::orc::JITTargetMachineBuilder(*jtmb_or_err).createTargetMachine();
        if (!machine) {
            llvm::errs() << "Failed to create pass TargetMachine: " << machine.takeError() << "\n";
            return false;
        }
        pass_machine_ = std::move(*machine);
    }
    // Clang's -O1 leaves both vectorizers off and -O2/-O3 turn both on; the
    // PipelineTuningOptions defaults (loop vectorization on, SLP off) match
    // neither, so follow clang.
    llvm::PipelineTuningOptions tuning;
    tuning.LoopVectorization = opt_level >= 2;
    tuning.LoopInterleaving = opt_level >= 2;
    tuning.SLPVectorization = opt_level >= 2;
    pb_ = std::make_unique<llvm::PassBuilder>(pass_machine_.get(), tuning);

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
    // Not called by the lifter. With an IR pipeline, LoopIdiomRecognize can turn
    // a guest fill/copy loop into llvm.memset/memcpy/memmove, and the backend
    // lowers a non-constant length to the libc call; resolve it here like every
    // other symbol rather than through a process-wide search generator.
    symbols[es.intern("memset")]              = sym_from_ptr(reinterpret_cast<void*>(&::memset));
    symbols[es.intern("memcpy")]              = sym_from_ptr(reinterpret_cast<void*>(&::memcpy));
    symbols[es.intern("memmove")]             = sym_from_ptr(reinterpret_cast<void*>(&::memmove));

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

    // PassBuilder(TM) registers the same TargetIRAnalysis itself; registering
    // it first makes that explicit (the later default registration is then a
    // no-op).
    fam.registerPass([&] { return pass_machine_->getTargetIRAnalysis(); });
    pb_->registerModuleAnalyses(mam);
    pb_->registerCGSCCAnalyses(cgam);
    pb_->registerFunctionAnalyses(fam);
    pb_->registerLoopAnalyses(lam);
    pb_->crossRegisterProxies(lam, fam, cgam, mam);

    // Rebuild per-module pipeline each call (analysis managers are fresh).
    // passBuilder itself is reused across calls (avoids plugin registration overhead).
    llvm::ModulePassManager mpm = pb_->buildPerModuleDefaultPipeline(lvl);
    mpm.run(mod, mam);
}

bool JitEngine::addModule(uint32_t pc, std::unique_ptr<llvm::Module> module,
                          std::unique_ptr<llvm::LLVMContext> ctx,
                          uint32_t fallthrough_pc) {
    auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx));

    if (auto err = jit_->addIRModule(std::move(tsm))) {
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
    cache_[pc] = BBInfo{fn, fallthrough_pc};
    return true;
}

BbFunc JitEngine::lookup(uint32_t pc) {
    auto it = cache_.find(pc);
    if (it != cache_.end())
        return it->second.fn;
    return nullptr;
}

