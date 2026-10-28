#pragma once

// What bcore writes differently for the two LLVMs it builds with: 15, and
// 23 (the WebAssembly build's). Where both accept one spelling the code uses
// it (IRBuilder::getPtrTy, PointerType::getUnqual); what follows has none.

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#if LLVM_VERSION_MAJOR >= 20
#include <llvm/IR/CmpPredicate.h>
#endif
#if LLVM_VERSION_MAJOR >= 17
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#else
#include <llvm/ADT/Triple.h>
#include <llvm/Support/Host.h>
#endif

namespace bcore {

inline bool starts_with(llvm::StringRef s, llvm::StringRef prefix) {
#if LLVM_VERSION_MAJOR >= 16
    return s.starts_with(prefix);
#else
    return s.startswith(prefix);
#endif
}

#if LLVM_VERSION_MAJOR >= 18
using CodeGenLevel = llvm::CodeGenOptLevel;
inline constexpr CodeGenLevel kCodeGenNone = CodeGenLevel::None;
inline constexpr CodeGenLevel kCodeGenLess = CodeGenLevel::Less;
inline constexpr CodeGenLevel kCodeGenDefault = CodeGenLevel::Default;
inline constexpr CodeGenLevel kCodeGenAggressive = CodeGenLevel::Aggressive;
inline constexpr llvm::CodeGenFileType kObjectFile = llvm::CodeGenFileType::ObjectFile;
#else
using CodeGenLevel = llvm::CodeGenOpt::Level;
inline constexpr CodeGenLevel kCodeGenNone = llvm::CodeGenOpt::None;
inline constexpr CodeGenLevel kCodeGenLess = llvm::CodeGenOpt::Less;
inline constexpr CodeGenLevel kCodeGenDefault = llvm::CodeGenOpt::Default;
inline constexpr CodeGenLevel kCodeGenAggressive = llvm::CodeGenOpt::Aggressive;
inline constexpr llvm::CodeGenFileType kObjectFile = llvm::CGFT_ObjectFile;
#endif

// InstCombine in a textual pipeline, as the default pipelines run it: since
// LLVM 18 it checks that one run reached a fixed point, and stops the
// process where it did not.
#if LLVM_VERSION_MAJOR >= 18
#define BCORE_INSTCOMBINE "instcombine<no-verify-fixpoint>"
#else
#define BCORE_INSTCOMBINE "instcombine"
#endif

// What PatternMatch's m_ICmp binds a predicate to.
#if LLVM_VERSION_MAJOR >= 20
using CmpPred = llvm::CmpPredicate;
#else
using CmpPred = llvm::CmpInst::Predicate;
#endif

// The declaration of intrinsic id (overloaded on tys) in mod.
inline llvm::Function* intrinsic(llvm::Module* mod, llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> tys = {}) {
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(mod, id, tys);
#else
    return llvm::Intrinsic::getDeclaration(mod, id, tys);
#endif
}

// block's terminator, or null while it has none: what LLVM 15's
// getTerminator returns, where 23's assumes there is one.
inline llvm::Instruction* terminator(llvm::BasicBlock& block) {
    return !block.empty() && block.back().isTerminator() ? &block.back() : nullptr;
}
inline const llvm::Instruction* terminator(const llvm::BasicBlock& block) {
    return !block.empty() && block.back().isTerminator() ? &block.back() : nullptr;
}

inline void set_target_triple(llvm::Module& mod, const std::string& triple) {
#if LLVM_VERSION_MAJOR >= 21
    mod.setTargetTriple(llvm::Triple(triple));
#else
    mod.setTargetTriple(triple);
#endif
}

// The target of triple, or null with why in error.
inline const llvm::Target* lookup_target(const std::string& triple, std::string& error) {
#if LLVM_VERSION_MAJOR >= 21
    return llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), error);
#else
    return llvm::TargetRegistry::lookupTarget(triple, error);
#endif
}

// createTargetMachine's code model, none to take the target's own.
#if LLVM_VERSION_MAJOR >= 16
using CodeModelChoice = std::optional<llvm::CodeModel::Model>;
#else
using CodeModelChoice = llvm::Optional<llvm::CodeModel::Model>;
#endif

// target's TargetMachine for triple; args are createTargetMachine's after
// the triple.
template <typename... Args>
llvm::TargetMachine* create_target_machine(const llvm::Target& target, const std::string& triple, Args&&... args) {
#if LLVM_VERSION_MAJOR >= 21
    return target.createTargetMachine(llvm::Triple(triple), std::forward<Args>(args)...);
#else
    return target.createTargetMachine(triple, std::forward<Args>(args)...);
#endif
}

// ce as an instruction, inserted before at.
inline llvm::Instruction* as_instruction(llvm::ConstantExpr* ce, llvm::Instruction* at) {
#if LLVM_VERSION_MAJOR >= 19
    llvm::Instruction* inst = ce->getAsInstruction();
    inst->insertBefore(at->getIterator());
    return inst;
#else
    return ce->getAsInstruction(at);
#endif
}

// The alignment RuntimeDyld's reserveAllocationSpace is given per section.
#if LLVM_VERSION_MAJOR >= 16
using SectionAlign = llvm::Align;
inline uint64_t align_bytes(SectionAlign a) { return a.value(); }
#else
using SectionAlign = uint32_t;
inline uint64_t align_bytes(SectionAlign a) { return a; }
#endif

}  // namespace bcore
