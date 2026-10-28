#pragma once

// The native CodeLoader (code_loader.h, which includes this): LLJIT links
// the objects into this process, their sections in code arenas
// (code_arena.h).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/MemoryBuffer.h>

namespace llvm { class JITEventListener; }

namespace bcore {

class CodeArena;
class CodeArenaPool;

class CodeLoader {
public:
    CodeLoader();
    ~CodeLoader();

    bool init();
    BbFunc load(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, const std::string& name);
    // Its code stays: LLJIT removes no object while it lives, and no arena
    // is unmapped before the loader goes (Core::invalidate replaces the whole
    // engine). So the profiling code map's addresses stay the blocks' too.
    void release(BbFunc) {}
    void enter(BbFunc fn, CpuState* cpu, Memory* mem) const { enter_(fn, cpu, mem); }

    // An arena of this loader's with room for `code` bytes of code beside
    // the code it holds (null if there is none), for code placed in it
    // directly (the copy-and-patch tier, cp_translator.h). The loader keeps
    // it, so that such code lives as long as the engine does, as objects'
    // code does (found in review: an arena only the copy-and-patch
    // translator held went with it, under blocks still in the cache).
    std::shared_ptr<CodeArena> codeArena(uintptr_t code);
    // For each of the n helpers names[i] (helper_abi.h): what generated
    // code calls for it, in the helpers' convention, which calls targets[i],
    // a C function of the helper's signature, in out[i]: targets[i] itself
    // where that convention is C's, else a function prefix + names[i] of
    // this loader's (init makes them for its blocks with
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
    // The C-ABI function enter() calls: it calls the block, with its
    // arguments in the registers the blocks' convention takes them in.
    using EnterFunc = void (*)(BbFunc, CpuState*, Memory*);

    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::shared_ptr<CodeArenaPool> pool_;
    std::vector<std::shared_ptr<CodeArena>> placed_;  // codeArena's arenas
    bool defineEnter();
    EnterFunc enter_ = nullptr;
};

}  // namespace bcore
