#pragma once

// How generated code calls generated code: the type and calling convention
// of every block and stencil (BbFunc, exec_backend.h) and the tail call by
// which one runs the next. Every block function is made by make_block and
// every call of one by call_block, so that the convention and the
// arguments' attributes, which a musttail call needs the same on both
// sides, are decided here alone.
//
// Swift's tail-calling convention, swifttailcc, on every host LLVM 15 has
// it for (x86-64 and AArch64): a musttail call between two such functions
// is always a tail call, the CpuState is `swiftself` (R13, X20) and the
// Memory `swiftasync` (R14, X22), registers the C convention keeps, so they
// stay where they are across a block's helper calls. GHC's convention,
// which bcore used before and which keeps no register for the caller,
// works on x86-64 only: on AArch64 LLVM gives a GHC function no prologue,
// saves no return address and makes no tail call of one (a musttail call
// is a fatal error).
//
// swifttailcc keeps the C convention's callee-saved registers, which a block
// that uses them would save and restore before its tail call: on x86-64,
// "no_callee_saved_registers" on every block and every call of one says it
// keeps none, as GHC's convention did, and the one C-ABI caller,
// bcore_enter, saves them all once per entry. LLVM's AArch64 backend knows
// no such attribute: there a block keeps them.
//
// On WebAssembly (a module whose triple is wasm32's) the C convention: a
// musttail call there is a return_call, which the engine's tail calls
// feature makes, and LLVM's WebAssembly backend has no swifttailcc (LLVM
// 19's). A block is exported under its own name, which the engine looks it
// up by. A module with no triple is the host's: WebAssembly in a
// WebAssembly build.

#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "llvm_compat.h"

namespace bcore_abi {

inline constexpr llvm::CallingConv::ID kBlockCC = llvm::CallingConv::SwiftTail;
inline constexpr unsigned kCpuArg = 0;
inline constexpr unsigned kMemArg = 1;
inline constexpr const char* kNoCalleeSaved = "no_callee_saved_registers";
inline constexpr const char* kWasmExportName = "wasm-export-name";

// Whether mod is compiled for WebAssembly (see above): its triple's, or
// with none, the host's.
inline bool is_wasm(const llvm::Module& mod) {
#ifdef __EMSCRIPTEN__
    if (mod.getTargetTriple().empty()) return true;
#endif
    return llvm::Triple(mod.getTargetTriple()).isWasm();
}

// void (CpuState*, Memory*), in kCpuArg and kMemArg.
inline llvm::FunctionType* block_type(llvm::LLVMContext& ctx) {
    auto* ptr = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptr, ptr}, false);
}

// A block function named `name` in `mod`, of block_type, in the blocks'
// convention.
inline llvm::Function* make_block(llvm::Module& mod, const llvm::Twine& name) {
    auto* fn = llvm::Function::Create(block_type(mod.getContext()), llvm::Function::ExternalLinkage, name, mod);
    if (is_wasm(mod)) {
        fn->addFnAttr(kWasmExportName, fn->getName());
        return fn;
    }
    fn->setCallingConv(kBlockCC);
    fn->addParamAttr(kCpuArg, llvm::Attribute::SwiftSelf);
    fn->addParamAttr(kMemArg, llvm::Attribute::SwiftAsync);
    fn->addFnAttr(kNoCalleeSaved);
    return fn;
}

// A block function's arguments: its CpuState* and its Memory*.
inline llvm::Argument* block_cpu(llvm::Function* fn) { return fn->getArg(kCpuArg); }
inline llvm::Argument* block_mem(llvm::Function* fn) { return fn->getArg(kMemArg); }

// A call of `callee`, a block function, with the block's arguments.
inline llvm::CallInst* call_block(llvm::IRBuilder<>& b, llvm::Value* callee, llvm::Value* cpu, llvm::Value* mem) {
    llvm::Value* args[2];
    args[kCpuArg] = cpu;
    args[kMemArg] = mem;
    llvm::CallInst* call = b.CreateCall(block_type(b.getContext()), callee, args);
    if (is_wasm(*b.GetInsertBlock()->getModule())) return call;
    call->setCallingConv(kBlockCC);
    call->addParamAttr(kCpuArg, llvm::Attribute::SwiftSelf);
    call->addParamAttr(kMemArg, llvm::Attribute::SwiftAsync);
    call->addFnAttr(llvm::Attribute::get(b.getContext(), kNoCalleeSaved));
    return call;
}

// The same as a musttail call; the caller returns right after it.
inline llvm::CallInst* tail_call_block(llvm::IRBuilder<>& b, llvm::Value* callee, llvm::Value* cpu,
                                       llvm::Value* mem) {
    llvm::CallInst* call = call_block(b, callee, cpu, mem);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    return call;
}

}  // namespace bcore_abi
