#pragma once
// What differs between the LLVM versions bcore builds with: 15 (the native
// hosts) and 19 (the WebAssembly build, whose LLVM is emscripten-forge's).
// Each difference is a helper here, so that the code that uses it reads the
// same against either; llvm::Triple comes from here too (TargetParser/ since
// LLVM 17).

#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/CodeGen.h>
#if LLVM_VERSION_MAJOR >= 17
#include <llvm/TargetParser/Triple.h>
#else
#include <llvm/ADT/Triple.h>
#endif

#include <cstdint>

// InstCombine in a pass pipeline's text, as the default pipelines run it:
// since LLVM 18 it otherwise checks that one run reached a fixed point and
// stops the process where it did not (LLVM 15 has no such check, nor the
// parameter).
#if LLVM_VERSION_MAJOR >= 18
#define BCORE_INSTCOMBINE "instcombine<no-verify-fixpoint>"
#else
#define BCORE_INSTCOMBINE "instcombine"
#endif

namespace bcore_llvm {

// StringRef::startswith, starts_with since LLVM 16 (the old name gone in 19).
inline bool starts_with(llvm::StringRef s, llvm::StringRef prefix) {
#if LLVM_VERSION_MAJOR >= 16
    return s.starts_with(prefix);
#else
    return s.startswith(prefix);
#endif
}

// The backend's optimization level: llvm::CodeGenOpt::Level, an enum class
// llvm::CodeGenOptLevel since LLVM 18.
#if LLVM_VERSION_MAJOR >= 18
using CodeGenOptLevel = llvm::CodeGenOptLevel;
#else
using CodeGenOptLevel = llvm::CodeGenOpt::Level;
#endif

inline CodeGenOptLevel codegen_opt_level(int level) {
#if LLVM_VERSION_MAJOR >= 18
    using L = llvm::CodeGenOptLevel;
#else
    namespace L = llvm::CodeGenOpt;
#endif
    return level <= 0 ? L::None : level == 1 ? L::Less : level == 3 ? L::Aggressive : L::Default;
}

// An object file as what a TargetMachine emits: llvm::CGFT_ObjectFile,
// llvm::CodeGenFileType::ObjectFile since LLVM 18.
#if LLVM_VERSION_MAJOR >= 18
constexpr auto kObjectFile = llvm::CodeGenFileType::ObjectFile;
#else
constexpr auto kObjectFile = llvm::CGFT_ObjectFile;
#endif

// RuntimeDyld's section alignments: uint32_t, llvm::Align since LLVM 16.
#if LLVM_VERSION_MAJOR >= 16
using SectionAlign = llvm::Align;
inline uint64_t align_value(llvm::Align a) { return a.value(); }
#else
using SectionAlign = uint32_t;
inline uint64_t align_value(uint32_t a) { return a; }
#endif

}  // namespace bcore_llvm
