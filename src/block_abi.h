#pragma once

// How generated code calls generated code: the type and calling convention
// of every block and stencil (BbFunc, jit_engine.h) and the tail call by
// which one runs the next. Every block function is made by make_block and
// every call of one by call_block, so that the convention and the
// arguments' attributes, which a musttail call needs the same on both
// sides, are decided here alone.
//
// GHC's convention: the callee keeps no register for its caller, so a block
// that runs the next one itself restores nothing first.

#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

namespace bcore_abi {

inline constexpr llvm::CallingConv::ID kBlockCC = llvm::CallingConv::GHC;
inline constexpr unsigned kCpuArg = 0;
inline constexpr unsigned kMemArg = 1;

// void (CpuState*, Memory*), in kCpuArg and kMemArg.
inline llvm::FunctionType* block_type(llvm::LLVMContext& ctx) {
    auto* ptr = llvm::Type::getInt8PtrTy(ctx);
    return llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptr, ptr}, false);
}

// A block function named `name` in `mod`, of block_type, in the blocks'
// convention.
inline llvm::Function* make_block(llvm::Module& mod, const llvm::Twine& name) {
    auto* fn = llvm::Function::Create(block_type(mod.getContext()), llvm::Function::ExternalLinkage, name, mod);
    fn->setCallingConv(kBlockCC);
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
    call->setCallingConv(kBlockCC);
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
