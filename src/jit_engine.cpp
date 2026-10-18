#include "jit_engine.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

#include "code_arena.h"
#include "syscall_emu.h"
#include "cec.h"
#include "cpu_state.h"

// A block function's entry count (addModule), and the count under which the
// splitter calls a basic block cold: a path behind a rare branch (weighted
// 1:2^20) counts 2^10 at most, one behind as many as 19 even branches still
// 2^11. Process-wide LLVM options, set once; without them the splitter would
// ask for a profile summary instead.
static constexpr uint64_t kEntryCount = uint64_t(1) << 30;
static constexpr const char* kColdCount = "2048";

static void split_by_counts() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& options = llvm::cl::getRegisteredOptions();
        auto set = [&](const char* name, const char* value) {
            auto it = options.find(name);
            if (it != options.end()) it->second->addOccurrence(0, name, value);
        };
        set("mfs-psi-cutoff", "0");
        set("mfs-count-threshold", kColdCount);
    });
}

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() = default;

// Helper to create a JITEvaluatedSymbol from a function pointer
static llvm::JITEvaluatedSymbol sym_from_ptr(void* ptr) {
    return llvm::JITEvaluatedSymbol(
        llvm::pointerToJITTargetAddress(ptr),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
}

bool JitEngine::init(int opt_level, int codegen_level, const MemoryEntries* entries) {
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

    // The machine function splitter moves a block's cold basic blocks into a
    // section of their own (.text.split.*), which the arena keeps apart from
    // the hot code, so that hot paths lie densely next to each other. It acts
    // on functions with profile counts: addModule gives every block one, and
    // the lifter's branch weights (rarely() in lift_visitor.cpp) make its rare
    // paths' counts fall under the threshold set here.
    jtmb_or_err->getOptions().EnableMachineFunctionSplitter = true;
    split_by_counts();

    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(std::move(*jtmb_or_err));
    // LLJIT's default object layer (LLJIT::createObjectLinkingLayer), with
    // every object's sections placed in the engine's arenas instead of pages
    // of their own (CodeArenaPool; an object no arena can take gets the
    // default memory manager).
    builder.setObjectLinkingLayerCreator(
        [pool = std::make_shared<CodeArenaPool>()](llvm::orc::ExecutionSession& es, const llvm::Triple&)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
            return std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                es, [pool] { return std::make_unique<ArenaMemoryManager>(pool); });
        });
    auto jit_or_err = builder.create();
    if (!jit_or_err) {
        llvm::errs() << "Failed to create LLJIT: " << jit_or_err.takeError() << "\n";
        return false;
    }
    jit_ = std::move(*jit_or_err);

    // Pre-allocate hash buckets to avoid rehash overhead during execution.
    cache_.reserve(1024);
    chain_ = std::make_unique<ChainSlot[]>(size_t(1) << kChainBits);
    for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};

    // Manually register all extern "C" symbols that JIT'd code may call.
    // This is more reliable than DynamicLibrarySearchGenerator which depends
    // on -rdynamic / ENABLE_EXPORTS.
    auto& es = jit_->getExecutionSession();
    auto& jd = jit_->getMainJITDylib();
    llvm::orc::MangleAndInterner mangle(es, jit_->getDataLayout());

    llvm::orc::SymbolMap symbols;
    // The host's own entries when it gives them: the call then goes straight
    // to the access instead of through mem_read*'s virtual call.
    const MemoryEntries direct = {&mem_read8, &mem_read16, &mem_read32, &mem_write8, &mem_write16, &mem_write32};
    const MemoryEntries& mem = entries ? *entries : direct;
    symbols[mangle("mem_read8")]           = sym_from_ptr(reinterpret_cast<void*>(mem.read8));
    symbols[mangle("mem_read16")]          = sym_from_ptr(reinterpret_cast<void*>(mem.read16));
    symbols[mangle("mem_read32")]          = sym_from_ptr(reinterpret_cast<void*>(mem.read32));
    symbols[mangle("mem_write8")]          = sym_from_ptr(reinterpret_cast<void*>(mem.write8));
    symbols[mangle("mem_write16")]         = sym_from_ptr(reinterpret_cast<void*>(mem.write16));
    symbols[mangle("mem_write32")]         = sym_from_ptr(reinterpret_cast<void*>(mem.write32));
    symbols[mangle("bfin_syscall")]        = sym_from_ptr(reinterpret_cast<void*>(&bfin_syscall));
    symbols[mangle("bfin_putchar")]        = sym_from_ptr(reinterpret_cast<void*>(&bfin_putchar));
    symbols[mangle("cec_data_alignment")]  = sym_from_ptr(reinterpret_cast<void*>(&cec_data_alignment));
    symbols[mangle("cec_exception")]       = sym_from_ptr(reinterpret_cast<void*>(&cec_exception));
    symbols[mangle("cec_raise")]           = sym_from_ptr(reinterpret_cast<void*>(&cec_raise));
    symbols[mangle("cec_return_rti")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rti));
    symbols[mangle("cec_return_rtx")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rtx));
    symbols[mangle("cec_return_rtn")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rtn));
    symbols[mangle("cec_return_rte")]      = sym_from_ptr(reinterpret_cast<void*>(&cec_return_rte));
    symbols[mangle("cec_cli")]             = sym_from_ptr(reinterpret_cast<void*>(&cec_cli));
    symbols[mangle("cec_sti")]             = sym_from_ptr(reinterpret_cast<void*>(&cec_sti));
    symbols[mangle("cec_push_reti")]       = sym_from_ptr(reinterpret_cast<void*>(&cec_push_reti));
    symbols[mangle("cec_pop_reti")]        = sym_from_ptr(reinterpret_cast<void*>(&cec_pop_reti));
    symbols[mangle("cec_check_sup")]       = sym_from_ptr(reinterpret_cast<void*>(&cec_check_sup));
    symbols[mangle("cec_is_user_mode")]    = sym_from_ptr(reinterpret_cast<void*>(&cec_is_user_mode));
    symbols[mangle("cec_check_pending")]   = sym_from_ptr(reinterpret_cast<void*>(&cec_check_pending));
    symbols[mangle("bfin_hwloop_step")]    = sym_from_ptr(reinterpret_cast<void*>(&bfin_hwloop_step));
    // Not called by the lifter. With an IR pipeline, LoopIdiomRecognize can turn
    // a guest fill/copy loop into llvm.memset/memcpy/memmove, and the backend
    // lowers a non-constant length to the libc call; resolve it here like every
    // other symbol rather than through a process-wide search generator.
    symbols[mangle("memset")]              = sym_from_ptr(reinterpret_cast<void*>(&::memset));
    symbols[mangle("memcpy")]              = sym_from_ptr(reinterpret_cast<void*>(&::memcpy));
    symbols[mangle("memmove")]             = sym_from_ptr(reinterpret_cast<void*>(&::memmove));

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
    // For the backend only, after the IR pipeline (see init()).
    for (llvm::Function& function : *module)
        if (!function.isDeclaration()) function.setEntryCount(kEntryCount);
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
    publishChain(pc, fn);
    return true;
}

// The function before the PC: generated code that matches the PC calls the
// function, so it must never see a new PC with the old function. Single
// threaded, so ordering the compiler's stores is enough.
void JitEngine::publishChain(uint32_t pc, BbFunc fn) {
    if (chain_filter_ && !chain_filter_(pc)) return;
    ChainSlot& slot = chain_[chainIndex(pc)];
    slot.pc = kChainEmpty;
    std::atomic_signal_fence(std::memory_order_release);
    slot.fn = fn;
    std::atomic_signal_fence(std::memory_order_release);
    slot.pc = pc;
}

void JitEngine::setChainFilter(std::function<bool(uint32_t)> filter) {
    chain_filter_ = std::move(filter);
    if (chain_)
        for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};
}

BbFunc JitEngine::lookup(uint32_t pc) {
    const ChainSlot& slot = chain_[chainIndex(pc)];
    if (slot.pc == pc) return slot.fn;
    auto it = cache_.find(pc);
    if (it == cache_.end())
        return nullptr;
    // Lost its slot to a block with the same index, or filtered.
    publishChain(pc, it->second.fn);
    return it->second.fn;
}

bool JitEngine::registerJITEventListener(llvm::JITEventListener& L) {
    // Runtime check, not an assumption: LLJIT's default on x86-64 Linux is
    // RTDyldObjectLinkingLayer (LLJIT.h), but a custom linking-layer creator
    // or another platform may differ.
    auto* rtdyld = llvm::dyn_cast<llvm::orc::RTDyldObjectLinkingLayer>(
        &jit_->getObjLinkingLayer());
    if (!rtdyld)
        return false;
    rtdyld->registerJITEventListener(L);
    return true;
}

void JitEngine::unregisterJITEventListener(llvm::JITEventListener& L) {
    if (auto* rtdyld = llvm::dyn_cast<llvm::orc::RTDyldObjectLinkingLayer>(&jit_->getObjLinkingLayer()))
        rtdyld->unregisterJITEventListener(L);
}
