#include "code_loader.h"
#include "block_abi.h"
#include "code_arena.h"
#include "helper_abi.h"
#include "helper_functions.h"
#include "llvm_compat.h"

#include <algorithm>
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
#include <llvm/Support/Format.h>
#include <llvm/Support/TargetSelect.h>

namespace bcore {

CodeLoader::CodeLoader() = default;
CodeLoader::~CodeLoader() = default;

// The absolute symbol of a function at ptr.
#if LLVM_VERSION_MAJOR >= 17
static llvm::orc::ExecutorSymbolDef sym_from_ptr(void* ptr) {
    return {llvm::orc::ExecutorAddr::fromPtr(ptr), llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
}
#else
static llvm::JITEvaluatedSymbol sym_from_ptr(void* ptr) {
    return llvm::JITEvaluatedSymbol(
        llvm::pointerToJITTargetAddress(ptr),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
}
#endif

// Compiles bcore_enter only: blocks come as object files (load) from a
// Compiler.
bool CodeLoader::init() {
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
        // (The creator's second argument, LLVM 15's Triple and 23's
        // JITLinkMemoryManager, and the MemoryBuffer that 23 gives the
        // memory manager's getter go unused.)
        [pool = pool_](llvm::orc::ExecutionSession& es, auto&&)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
            return std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                es, [pool](auto&&...) { return std::make_unique<ArenaMemoryManager>(pool); });
        });
    auto jit_or_err = builder.create();
    if (!jit_or_err) {
        llvm::errs() << "Failed to create LLJIT: " << jit_or_err.takeError() << "\n";
        return false;
    }
    jit_ = std::move(*jit_or_err);

    // Manually register every function JIT'd code may call, by its address
    // under the name the code calls it by (none of them has a C name).
    // This is more reliable than DynamicLibrarySearchGenerator which depends
    // on -rdynamic / ENABLE_EXPORTS.
    auto& es = jit_->getExecutionSession();
    auto& jd = jit_->getMainJITDylib();
    llvm::orc::MangleAndInterner mangle(es, jit_->getDataLayout());

    // Each helper as generated code calls it, in the helpers' convention
    // (helper_abi.h).
    std::vector<const char*> names;
    std::vector<void*> targets;
    for (const auto& [name, fn] : bcore_abi::helper_functions()) {
        names.push_back(name);
        targets.push_back(fn);
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
bool CodeLoader::makeTrampolines(size_t n, const char* const* names, void* const* targets, void** out,
                                 const std::string& prefix) {
    if (bcore_abi::kHelperCC == llvm::CallingConv::C) {
        std::copy(targets, targets + n, out);
        return true;
    }
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>(prefix, *ctx);
    mod->setDataLayout(jit_->getDataLayout());
    set_target_triple(*mod, jit_->getTargetTriple().str());
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
bool CodeLoader::defineEnter() {
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("bcore_enter", *ctx);
    mod->setDataLayout(jit_->getDataLayout());
    set_target_triple(*mod, jit_->getTargetTriple().str());
    auto* ptr = llvm::PointerType::getUnqual(*ctx);
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

BbFunc CodeLoader::load(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, const std::string& name) {
    JitWriteScope write;  // the lookup links it
    if (auto err = jit_->addObjectFile(std::move(object))) {
        llvm::errs() << "Failed to add object for PC=0x" << llvm::format_hex(pc, 8)
                     << ": " << err << "\n";
        return nullptr;
    }

    auto sym = jit_->lookup(name);
    if (!sym) {
        llvm::errs() << "Failed to lookup " << name << ": " << sym.takeError() << "\n";
        return nullptr;
    }
    return reinterpret_cast<BbFunc>(sym->getValue());
}

std::shared_ptr<CodeArena> CodeLoader::codeArena(uintptr_t code) {
    auto arena = pool_ ? pool_->arenaFor(code, 0) : nullptr;
    if (arena && (placed_.empty() || placed_.back() != arena)) placed_.push_back(arena);
    return arena;
}

bool CodeLoader::registerJITEventListener(llvm::JITEventListener& L) {
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

void CodeLoader::unregisterJITEventListener(llvm::JITEventListener& L) {
    if (auto* rtdyld = llvm::dyn_cast<llvm::orc::RTDyldObjectLinkingLayer>(&jit_->getObjLinkingLayer()))
        rtdyld->unregisterJITEventListener(L);
}

}  // namespace bcore
