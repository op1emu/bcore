#pragma once

// How generated code calls generated code: the type and calling convention
// of every block and stencil (BbFunc, code_loader.h) and the tail call by
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
// From LLVM 19 on, preserve_none: the callee keeps no register for its
// caller (but the frame pointer), so no block saves one, on AArch64 too;
// its first arguments are R12, R13 (X20, X21), which C keeps, so the
// Memory and the CpuState stay where they are across the helpers. The
// Memory comes first: as a base register R12 takes a SIB byte in every
// access, R13 none. swifttailcc would not do there: LLVM 18's
// SelectionDAGISel gives a function with a swiftasync argument no FastISel
// (dontUseFastISelFor), and at O0 a block took the whole SelectionDAG,
// twice LLVM 15's backend time; without swiftasync the Memory came in RDI
// and a loop block kept it on the stack. FastISel selects neither a
// preserve_none return nor a musttail call: at O0 they are put in a block
// of their own (Compiler::emit).
//
// On WebAssembly, whose functions keep their values in locals and take
// their arguments as they are, C's convention: a musttail call is a
// return_call (with the target's tail-call feature), and C++ calls a block
// itself. A block there is exported under its name (wasm-export-name), the
// function the loader finds in its module, and has no table of constants
// (no-jump-tables). Which a module's blocks are is
// its target triple's (the host's, for a module with none).

#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "llvm_compat.h"

namespace bcore {

namespace bcore_abi {

// Whether mod is compiled for WebAssembly: by its target triple, or with
// none, by the host's.
inline bool is_wasm(const llvm::Module& mod) {
    const llvm::Triple triple(mod.getTargetTriple());
    if (triple.getArch() != llvm::Triple::UnknownArch) return triple.isWasm();
#if defined(__wasm__)
    return true;
#else
    return false;
#endif
}

inline constexpr unsigned kCpuArg = 0;
inline constexpr unsigned kMemArg = 1;
inline constexpr const char* kNoCalleeSaved = "no_callee_saved_registers";
inline constexpr bool kPreserveNone = LLVM_VERSION_MAJOR >= 19;

// Whether mod's blocks are preserve_none's (LLVM 19 on, not on WebAssembly).
inline bool preserve_none(const llvm::Module& mod) { return kPreserveNone && !is_wasm(mod); }

// The blocks' convention in mod (C's on WebAssembly, which make_block and
// call_block leave as it is).
inline llvm::CallingConv::ID block_cc(const llvm::Module& mod) {
#if LLVM_VERSION_MAJOR >= 19
    if (preserve_none(mod)) return llvm::CallingConv::PreserveNone;
#endif
    return llvm::CallingConv::SwiftTail;
}

// Where mod's blocks take their CpuState* and Memory*: kCpuArg and kMemArg,
// or swapped in preserve_none (see above).
inline unsigned cpu_arg(const llvm::Module& mod) { return preserve_none(mod) ? kMemArg : kCpuArg; }
inline unsigned mem_arg(const llvm::Module& mod) { return preserve_none(mod) ? kCpuArg : kMemArg; }

// void (ptr, ptr): a CpuState* and a Memory*, in cpu_arg's and mem_arg's places.
inline llvm::FunctionType* block_type(llvm::LLVMContext& ctx) {
    auto* ptr = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptr, ptr}, false);
}

// A block function named `name` in `mod`, of block_type, in the blocks'
// convention.
inline llvm::Function* make_block(llvm::Module& mod, const llvm::Twine& name) {
    auto* fn = llvm::Function::Create(block_type(mod.getContext()), llvm::Function::ExternalLinkage, name, mod);
    if (is_wasm(mod)) {
        fn->addFnAttr("wasm-export-name", fn->getName());
        // No switch as a table of constants (SimplifyCFG's lookup tables):
        // data, which the loader cannot place (wasm_object.h).
        fn->addFnAttr("no-jump-tables", "true");
        return fn;
    }
    fn->setCallingConv(block_cc(mod));
    if (preserve_none(mod)) return fn;
    fn->addParamAttr(kCpuArg, llvm::Attribute::SwiftSelf);
    fn->addParamAttr(kMemArg, llvm::Attribute::SwiftAsync);
    fn->addFnAttr(kNoCalleeSaved);
    return fn;
}

// A block function's arguments: its CpuState* and its Memory*.
inline llvm::Argument* block_cpu(llvm::Function* fn) { return fn->getArg(cpu_arg(*fn->getParent())); }
inline llvm::Argument* block_mem(llvm::Function* fn) { return fn->getArg(mem_arg(*fn->getParent())); }

// A call of `callee`, a block function, with the block's arguments.
inline llvm::CallInst* call_block(llvm::IRBuilder<>& b, llvm::Value* callee, llvm::Value* cpu, llvm::Value* mem) {
    const llvm::Module& mod = *b.GetInsertBlock()->getModule();
    llvm::Value* args[2];
    args[cpu_arg(mod)] = cpu;
    args[mem_arg(mod)] = mem;
    llvm::CallInst* call = b.CreateCall(block_type(b.getContext()), callee, args);
    if (is_wasm(mod)) return call;
    call->setCallingConv(block_cc(mod));
    if (preserve_none(mod)) return call;
    call->addParamAttr(kCpuArg, llvm::Attribute::SwiftSelf);
    call->addParamAttr(kMemArg, llvm::Attribute::SwiftAsync);
    call->addFnAttr(llvm::Attribute::get(b.getContext(), kNoCalleeSaved));
    return call;
}

// Whether the module b inserts into is compiled for WebAssembly.
inline bool is_wasm(const llvm::IRBuilderBase& b) { return is_wasm(*b.GetInsertBlock()->getModule()); }

// A field offset bytes into the object at base (the CpuState, the chain
// table, the tier table), in a module compiled for WebAssembly (wasm,
// is_wasm) or not. In bounds on WebAssembly: a load or store there takes a
// constant offset only from an address that cannot wrap, and without it each
// field's address is an add of its own, which V8 keeps in a register (and
// spills around a call). Elsewhere a plain GEP, as it always was.
inline llvm::Value* object_field(llvm::IRBuilderBase& b, bool wasm, llvm::Value* base, llvm::Value* offset,
                                 const llvm::Twine& name = "") {
    return wasm ? b.CreateInBoundsGEP(b.getInt8Ty(), base, offset, name)
                : b.CreateGEP(b.getInt8Ty(), base, offset, name);
}

// The same as a musttail call; the caller returns right after it.
inline llvm::CallInst* tail_call_block(llvm::IRBuilder<>& b, llvm::Value* callee, llvm::Value* cpu,
                                       llvm::Value* mem) {
    llvm::CallInst* call = call_block(b, callee, cpu, mem);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    return call;
}

}  // namespace bcore_abi

}  // namespace bcore
