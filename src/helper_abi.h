#pragma once

// The helpers generated code calls (JitEngine registers each by its
// address under its name here, a name no linker sees: they are C++
// functions in namespace bcore; cp_format.h's kHelpers names the same for
// the stencils), with the LLVM signature of each and the convention blocks
// and stencils call them in.
//
// On AArch64, LLVM's preserve_most: the callee keeps every register but
// X16-X18, so that a block keeps its values in registers across a call on
// a rare path, whichever registers they are, and saves nothing for it; a
// block there keeps C's callee-saved registers (block_abi.h), and C's own
// would cost it a save at its entry for every value it keeps across a
// call. The helpers are in C's convention: each is called through a
// trampoline in preserve_most (JitEngine::makeTrampolines), which saves
// what C may change around the call.
//
// On x86-64, C's convention, called directly: a block there keeps no
// callee-saved register (block_abi.h), and so, as with GHC's convention,
// keeps its values in the ones C keeps across a call. Trampolines made the
// boot's host work 2.4% larger and its compilation 4.6% slower (FastISel
// selects no call in another convention). LLVM 15's x86 preserve_most
// also restores RAX over the value a function returns.
//
// A parameter narrower than 32 bits (mem_write8's and mem_write16's value)
// is zeroext on every declaration and call: the caller extends it, as
// Apple's arm64 ABI requires and clang's code on x86-64 relies on (a store
// of R0 = 0x12345678's byte gave the callback 0x12345678). A narrow return
// value has no attribute: the caller narrows it itself.
//
// memset, memcpy and memmove are not among the helpers: the backend calls
// those for an intrinsic, in the C convention.
//
// On macOS a host's exception cannot unwind through generated code:
// RuntimeDyld registers no usable unwind information for Mach-O AArch64
// objects (a test's exception through a block ended in std::terminate).
// There the helpers that reach host code catch it (host_bridge.h,
// kHelpersCatch); BCORE_HELPERS_CATCH=1 does the same on any host.

#include <cstring>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

#include "llvm_compat.h"

namespace bcore {

namespace bcore_abi {

// sig: the return type, then the parameters' types, a letter each: v void,
// b i1, c i8, s i16, i i32, p a pointer.
struct Helper {
    const char* name;
    const char* sig;
};

inline constexpr Helper kHelpers[] = {
    {"mem_read8", "cpi"},           {"mem_read16", "spi"},       {"mem_read32", "ipi"},
    {"mem_write8", "vpic"},         {"mem_write16", "vpis"},     {"mem_write32", "vpii"},
    {"cec_data_alignment", "vppiii"}, {"cec_exception", "vpip"}, {"cec_raise", "vpi"},
    {"cec_return_rti", "ip"},       {"cec_return_rtx", "ip"},    {"cec_return_rtn", "ip"},
    {"cec_return_rte", "ip"},       {"cec_cli", "ip"},           {"cec_sti", "vpi"},
    {"cec_push_reti", "vp"},        {"cec_pop_reti", "vp"},      {"cec_check_sup", "vp"},
    {"cec_is_user_mode", "b"},      {"cec_check_pending", "vp"}, {"bfin_hwloop_step", "vpii"},
    {"bfin_putchar", "vi"},         {"bfin_syscall", "vpp"},
};

inline const Helper* find_helper(llvm::StringRef name) {
    for (const Helper& h : kHelpers)
        if (name == h.name) return &h;
    return nullptr;
}

inline llvm::Type* helper_letter_type(llvm::LLVMContext& ctx, char c) {
    switch (c) {
    case 'v': return llvm::Type::getVoidTy(ctx);
    case 'b': return llvm::Type::getInt1Ty(ctx);
    case 'c': return llvm::Type::getInt8Ty(ctx);
    case 's': return llvm::Type::getInt16Ty(ctx);
    case 'i': return llvm::Type::getInt32Ty(ctx);
    case 'p': return llvm::PointerType::getUnqual(ctx);
    }
    llvm::report_fatal_error("bad helper signature letter");
}

#ifndef BCORE_HELPERS_CATCH
#if defined(__APPLE__)
#define BCORE_HELPERS_CATCH 1
#else
#define BCORE_HELPERS_CATCH 0
#endif
#endif
inline constexpr bool kHelpersCatch = BCORE_HELPERS_CATCH;

// The convention a helper is called in (see above).
inline constexpr llvm::CallingConv::ID kHelperCC =
#if defined(__x86_64__) || defined(_M_X64)
    llvm::CallingConv::C;
#else
    llvm::CallingConv::PreserveMost;
#endif

// The convention a helper is called in on triple's architecture: kHelperCC
// where that is this host's.
inline llvm::CallingConv::ID helper_cc(const llvm::Triple& triple) {
    return triple.isX86() ? llvm::CallingConv::C : llvm::CallingConv::PreserveMost;
}

inline llvm::FunctionType* helper_type(llvm::LLVMContext& ctx, const Helper& h) {
    std::vector<llvm::Type*> params;
    for (const char* c = h.sig + 1; *c; ++c) params.push_back(helper_letter_type(ctx, *c));
    return llvm::FunctionType::get(helper_letter_type(ctx, h.sig[0]), params, false);
}

// zeroext on fn's (a function's or a call's) parameters narrower than 32
// bits, as `h` has them (see above).
template <typename F>
inline void extend_narrow_params(const Helper& h, F& fn) {
    for (unsigned i = 0; h.sig[i + 1]; ++i)
        if (h.sig[i + 1] == 'b' || h.sig[i + 1] == 'c' || h.sig[i + 1] == 's')
            fn.addParamAttr(i, llvm::Attribute::ZExt);
}

// The declaration of the helper `name` in mod, of type ft, in the helpers'
// convention: what generated code calls. ft must be the helper's own type.
inline llvm::Function* declare_helper(llvm::Module& mod, const char* name, llvm::FunctionType* ft) {
    const Helper* h = find_helper(name);
    if (!h) llvm::report_fatal_error(llvm::Twine("not a helper: ") + name);
    if (ft != helper_type(mod.getContext(), *h))
        llvm::report_fatal_error(llvm::Twine("a call of ") + name + " with another signature than its own");
    auto* fn = llvm::cast<llvm::Function>(mod.getOrInsertFunction(name, ft).getCallee());
    fn->setCallingConv(kHelperCC);
    extend_narrow_params(*h, *fn);
    return fn;
}

// A call of fn, a helper's declaration (declare_helper), with its
// convention and its parameters' attributes.
inline void mark_helper_call(llvm::CallInst& call, llvm::Function& fn) {
    call.setCallingConv(fn.getCallingConv());
    if (const Helper* h = find_helper(fn.getName())) extend_narrow_params(*h, call);
}

// Every helper's declaration in mod and every call of one in convention
// cc: a module lifted here (in kHelperCC) and compiled for another
// architecture (tools/stencilgen.cpp --target).
inline void set_helper_cc(llvm::Module& mod, llvm::CallingConv::ID cc) {
    for (llvm::Function& fn : mod) {
        if (!fn.isDeclaration() || !find_helper(fn.getName())) continue;
        fn.setCallingConv(cc);
        for (llvm::User* user : fn.users())
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(user))
                if (call->getCalledFunction() == &fn) call->setCallingConv(cc);
    }
}

// The same with the helper's own type.
inline llvm::Function* declare_helper(llvm::Module& mod, const char* name) {
    const Helper* h = find_helper(name);
    if (!h) llvm::report_fatal_error(llvm::Twine("not a helper: ") + name);
    return declare_helper(mod, name, helper_type(mod.getContext(), *h));
}

}  // namespace bcore_abi

}  // namespace bcore
