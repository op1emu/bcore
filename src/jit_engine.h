#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/MemoryBuffer.h>

#include "exec_backend.h"

namespace llvm { class JITEventListener; }

class CodeArena;
class CodeArenaPool;

// The native engine: blocks' objects linked into this process by LLVM's
// ORC JIT, their code in the engine's arenas (code_arena.h).
class JitEngine : public ExecBackend {
public:
    JitEngine();
    ~JitEngine() override;

    bool init() override;
    bool addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, uint32_t fallthrough_pc,
                   const std::string& name) override;
    JitEngine* native() override { return this; }
    // An arena of this engine's with room for `code` bytes of code beside
    // the code it holds (null if there is none), for code placed in it
    // directly (the copy-and-patch tier, cp_translator.h). The engine keeps
    // it, so that such code lives as long as the engine does, as objects'
    // code does (found in review: an arena only the copy-and-patch
    // translator held went with it, under blocks still in the cache).
    std::shared_ptr<CodeArena> codeArena(uintptr_t code);
    // For each of the n helpers names[i] (helper_abi.h): what generated
    // code calls for it, in the helpers' convention, which calls targets[i],
    // a C function of the helper's signature, in out[i]: targets[i] itself
    // where that convention is C's, else a function prefix + names[i] of
    // this engine's (init makes them for its blocks with
    // "bcore_trampoline."; the copy-and-patch tier, for its wrapped helpers,
    // with a prefix of its own).
    bool makeTrampolines(size_t n, const char* const* names, void* const* targets, void** out,
                         const std::string& prefix = "bcore_trampoline.");

    // Register L on the underlying object-linking layer for object load/free
    // notifications (the profiling code map builds itself from these).
    // Behavior-neutral on x86-64 Linux where LLJIT defaults to
    // RTDyldObjectLinkingLayer; returns false (and registers nothing) on any
    // other layer so callers can degrade gracefully.
    bool registerJITEventListener(llvm::JITEventListener& L);
    // Undo registerJITEventListener (no-op on other linking layers).
    void unregisterJITEventListener(llvm::JITEventListener& L);

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::shared_ptr<CodeArenaPool> pool_;
    std::vector<std::shared_ptr<CodeArena>> placed_;  // codeArena's arenas
    bool defineEnter();
};
