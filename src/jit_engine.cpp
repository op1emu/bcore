#include "jit_engine.h"
#include "block_abi.h"

#include <cstddef>
#include <cstdio>
#include <atomic>
#include <cstring>

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include "code_arena.h"
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

// Compiles bcore_enter only: blocks come as object files (addObject) from
// a Compiler.
bool JitEngine::init() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jtmb_or_err = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb_or_err) {
        llvm::errs() << "Failed to detect host: " << jtmb_or_err.takeError() << "\n";
        return false;
    }

    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(std::move(*jtmb_or_err));
    // LLJIT's default object layer (LLJIT::createObjectLinkingLayer), with
    // every object's sections placed in the engine's arenas instead of pages
    // of their own (CodeArenaPool; an object no arena can take gets the
    // default memory manager).
    pool_ = std::make_shared<CodeArenaPool>();
    builder.setObjectLinkingLayerCreator(
        [pool = pool_](llvm::orc::ExecutionSession& es, const llvm::Triple&)
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
    static_assert(offsetof(ChainArea, tier) == kTierTableOffset, "generated code finds the tier table there");
    area_ = std::make_unique<ChainArea>();  // value-initialized: the tier table all zero
    chain_ = area_->slots;
    for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};

    // Manually register all extern "C" symbols that JIT'd code may call.
    // This is more reliable than DynamicLibrarySearchGenerator which depends
    // on -rdynamic / ENABLE_EXPORTS.
    auto& es = jit_->getExecutionSession();
    auto& jd = jit_->getMainJITDylib();
    llvm::orc::MangleAndInterner mangle(es, jit_->getDataLayout());

    llvm::orc::SymbolMap symbols;
    symbols[mangle("mem_read8")]           = sym_from_ptr(reinterpret_cast<void*>(&mem_read8));
    symbols[mangle("mem_read16")]          = sym_from_ptr(reinterpret_cast<void*>(&mem_read16));
    symbols[mangle("mem_read32")]          = sym_from_ptr(reinterpret_cast<void*>(&mem_read32));
    symbols[mangle("mem_write8")]          = sym_from_ptr(reinterpret_cast<void*>(&mem_write8));
    symbols[mangle("mem_write16")]         = sym_from_ptr(reinterpret_cast<void*>(&mem_write16));
    symbols[mangle("mem_write32")]         = sym_from_ptr(reinterpret_cast<void*>(&mem_write32));
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

    return defineEnter();
}

// void bcore_enter(fn, cpu, mem) { fn(cpu, mem); } with fn in the blocks'
// convention (block_abi.h): the only C-ABI frame a run of chained blocks has.
bool JitEngine::defineEnter() {
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("bcore_enter", *ctx);
    mod->setDataLayout(jit_->getDataLayout());
    mod->setTargetTriple(jit_->getTargetTriple().str());
    auto* ptr = llvm::Type::getInt8PtrTy(*ctx);
    auto* void_ty = llvm::Type::getVoidTy(*ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(void_ty, {ptr, ptr, ptr}, false),
                                      llvm::Function::ExternalLinkage, "bcore_enter", mod.get());
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(*ctx, "entry", fn));
    auto args = fn->arg_begin();
    llvm::Value* block = &*args++;
    llvm::Value* cpu = &*args++;
    llvm::Value* mem = &*args;
    bcore_abi::call_block(b, block, cpu, mem);
    b.CreateRetVoid();
    if (auto err = jit_->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx)))) {
        llvm::errs() << "Failed to add bcore_enter: " << err << "\n";
        return false;
    }
    auto sym = jit_->lookup("bcore_enter");
    if (!sym) {
        llvm::errs() << "Failed to lookup bcore_enter: " << sym.takeError() << "\n";
        return false;
    }
    enter_ = reinterpret_cast<EnterFunc>(sym->getValue());
    return true;
}

bool JitEngine::addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object,
                          uint32_t fallthrough_pc, const std::string& name) {
    if (auto err = jit_->addObjectFile(std::move(object))) {
        llvm::errs() << "Failed to add object for PC=0x" << llvm::format_hex(pc, 8)
                     << ": " << err << "\n";
        return false;
    }

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

void JitEngine::addCode(uint32_t pc, BbFunc fn, uint32_t fallthrough_pc) {
    cache_[pc] = BBInfo{fn, fallthrough_pc};
    publishChain(pc, fn);
}

std::shared_ptr<CodeArena> JitEngine::codeArena(uintptr_t code) {
    auto arena = pool_ ? pool_->arenaFor(code, 0) : nullptr;
    if (arena && (placed_.empty() || placed_.back() != arena)) placed_.push_back(arena);
    return arena;
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
