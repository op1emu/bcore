#include "jit_engine.h"
#include "block_abi.h"
#include "helper_abi.h"
#include "host_bridge.h"

#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Constants.h>
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

    jtmb_or_err->getOptions().SwiftAsyncFramePointer = llvm::SwiftAsyncFramePointerMode::Never;
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

    // Each helper as generated code calls it, in the helpers' convention
    // (helper_abi.h).
    const std::pair<const char*, void*> helpers[] = {
        {"mem_read8", reinterpret_cast<void*>(&mem_read8)},
        {"mem_read16", reinterpret_cast<void*>(&mem_read16)},
        {"mem_read32", reinterpret_cast<void*>(&mem_read32)},
        {"mem_write8", reinterpret_cast<void*>(&mem_write8)},
        {"mem_write16", reinterpret_cast<void*>(&mem_write16)},
        {"mem_write32", reinterpret_cast<void*>(&mem_write32)},
        {"bfin_syscall", reinterpret_cast<void*>(&bfin_syscall)},
        {"bfin_putchar", reinterpret_cast<void*>(&bfin_putchar)},
        {"cec_data_alignment", reinterpret_cast<void*>(&cec_data_alignment)},
        {"cec_exception", reinterpret_cast<void*>(&cec_exception)},
        {"cec_raise", reinterpret_cast<void*>(&cec_raise)},
        {"cec_return_rti", reinterpret_cast<void*>(&cec_return_rti)},
        {"cec_return_rtx", reinterpret_cast<void*>(&cec_return_rtx)},
        {"cec_return_rtn", reinterpret_cast<void*>(&cec_return_rtn)},
        {"cec_return_rte", reinterpret_cast<void*>(&cec_return_rte)},
        {"cec_cli", reinterpret_cast<void*>(&cec_cli)},
        {"cec_sti", reinterpret_cast<void*>(&cec_sti)},
        {"cec_push_reti", reinterpret_cast<void*>(&cec_push_reti)},
        {"cec_pop_reti", reinterpret_cast<void*>(&cec_pop_reti)},
        {"cec_check_sup", reinterpret_cast<void*>(&cec_check_sup)},
        {"cec_is_user_mode", reinterpret_cast<void*>(&cec_is_user_mode)},
        {"cec_check_pending", reinterpret_cast<void*>(&cec_check_pending)},
        {"bfin_hwloop_step", reinterpret_cast<void*>(&bfin_hwloop_step)},
    };
    static_assert(sizeof(helpers) / sizeof(helpers[0]) == sizeof(bcore_abi::kHelpers) / sizeof(bcore_abi::kHelpers[0]),
                  "a function for every helper");
    std::vector<const char*> names;
    std::vector<void*> targets;
    for (const auto& [name, fn] : helpers) {
        void* caught = bcore_abi::kHelpersCatch ? bcore_bridge::wrapper(name) : nullptr;
        names.push_back(name);
        targets.push_back(caught ? caught : fn);
    }
    std::vector<void*> trampolines(names.size());
    if (!makeTrampolines(names.size(), names.data(), targets.data(), trampolines.data())) return false;
    llvm::orc::SymbolMap symbols;
    for (size_t i = 0; i < names.size(); ++i) symbols[mangle(names[i])] = sym_from_ptr(trampolines[i]);
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

// One module of functions bcore_trampoline.<name> in the helpers'
// convention (helper_abi.h): R name(args) { return target(args); }, the
// target a C function at a constant address, called through a register (so
// that no code model or reach matters). Not nounwind: a host's exception
// unwinds through a trampoline as through the block that called it. Where
// the helpers' convention is C's (x86-64), the targets themselves.
bool JitEngine::makeTrampolines(size_t n, const char* const* names, void* const* targets, void** out,
                                const std::string& prefix) {
    if (bcore_abi::kHelperCC == llvm::CallingConv::C) {
        std::copy(targets, targets + n, out);
        return true;
    }
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>(prefix, *ctx);
    mod->setDataLayout(jit_->getDataLayout());
    mod->setTargetTriple(jit_->getTargetTriple().str());
    for (size_t i = 0; i < n; ++i) {
        const bcore_abi::Helper* h = bcore_abi::find_helper(names[i]);
        if (!h) {
            llvm::errs() << "No trampoline for " << names[i] << ": not a helper\n";
            return false;
        }
        auto* ft = bcore_abi::helper_type(*ctx, *h);
        auto* fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, prefix + names[i], mod.get());
        fn->setCallingConv(bcore_abi::kHelperCC);
        bcore_abi::extend_narrow_params(*h, *fn);
        fn->setUWTableKind(llvm::UWTableKind::Async);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(*ctx, "entry", fn));
        std::vector<llvm::Value*> args;
        for (auto& arg : fn->args()) args.push_back(&arg);
        auto* target = llvm::ConstantExpr::getIntToPtr(
            b.getInt64(reinterpret_cast<uintptr_t>(targets[i])), llvm::PointerType::getUnqual(*ctx));
        auto* call = b.CreateCall(ft, target, args);
        bcore_abi::extend_narrow_params(*h, *call);  // C's prototype's, as the declaration's
        call->setTailCallKind(llvm::CallInst::TCK_NoTail);
        if (ft->getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(call);
    }
    JitWriteScope write;  // the lookups link them
    if (auto err = jit_->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx)))) {
        llvm::errs() << "Failed to add the trampolines: " << err << "\n";
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        auto sym = jit_->lookup(prefix + names[i]);
        if (!sym) {
            llvm::errs() << "Failed to lookup the trampoline of " << names[i] << ": " << sym.takeError() << "\n";
            return false;
        }
        out[i] = reinterpret_cast<void*>(sym->getValue());
    }
    return true;
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
    JitWriteScope write;  // the lookup links it
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
    JitWriteScope write;  // the lookup links it
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
