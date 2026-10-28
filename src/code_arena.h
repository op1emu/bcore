#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>

namespace bcore {

// One mapping that a JIT's objects are placed in, code next to code. LLJIT's
// default gives every object a SectionMemoryManager of its own, which maps
// each section separately and rounds it up to whole pages: every compiled
// block then started at offset 0 of a page of its own, so their entries
// competed for the same L1I and L2 sets and every block cost an iTLB entry.
//
// The first half of the reservation holds code, readable, writable and
// executable for as long as the arena lives (no permission changes as blocks
// are added), the blocks' hot code apart from their cold parts (the machine
// function splitter's .text.split.* sections, see JitEngine::init, and the
// copy-and-patch stencils' cold code), so that hot code lies densely:
//  - Split (x86-64): hot code in its first three quarters, cold in the last.
//  - Shared (AArch64): hot code from its start up, cold code from its end
//    down, until they meet. The code half is 128 MiB there, B/BL's reach: a
//    branch between any two places of an arena's code reaches, cold or hot,
//    however much of either there is.
// The second half holds data. All of it is within 2 GiB, as the small code
// model's PC-relative references between an object's sections need. Pages
// are touched, not committed, as they fill.
//
// On Apple silicon the code half is a MAP_JIT mapping, which a thread may
// write only within a JitWriteScope and run only outside one.
//
// Single-threaded, like all of bcore's compilation.
class CodeArena {
public:
    enum class Layout { Split, Shared };
#if defined(__x86_64__)
    // Each half of the default reservation: the boot to op1emu's main display
    // compiles about 53 MB of code into it.
    static constexpr size_t kDefaultHalf = size_t(512) << 20;
    static constexpr Layout kDefaultLayout = Layout::Split;
    static constexpr size_t kDefaultArenas = 16;
#else
    static constexpr size_t kDefaultHalf = size_t(128) << 20;  // the code within B/BL's reach
    static constexpr Layout kDefaultLayout = Layout::Shared;
    static constexpr size_t kDefaultArenas = 64;
#endif
    // A branch's reach on AArch64 (B, BL: +-128 MiB), which Shared code fits.
    static constexpr size_t kBranchReach = size_t(128) << 20;

    // Null when the reservation fails.
    static std::shared_ptr<CodeArena> create(size_t half = kDefaultHalf, Layout layout = kDefaultLayout);
    // The hot code an empty arena takes, and the largest object's code (all
    // of it in either region, however the splitter divides it).
    static uintptr_t hotSize(size_t half, Layout layout) { return layout == Layout::Split ? half / 4 * 3 : half; }
    static uintptr_t largestCode(size_t half, Layout layout) { return layout == Layout::Split ? half / 4 : half; }
    ~CodeArena();
    CodeArena(const CodeArena&) = delete;
    CodeArena& operator=(const CodeArena&) = delete;

    enum class Region { Hot, Cold, Data };
    // Whether an object of this much code and data fits whole: its code in
    // the hot region and, however the splitter divides it, in the cold one.
    bool fits(uintptr_t code, uintptr_t data) const;
    // Size bytes aligned to alignment, or null when that region is full.
    uint8_t* allocate(Region region, uintptr_t size, unsigned alignment);
    // From the first byte of its code to the last, hot or cold.
    size_t codeSpan() const { return code_size_; }
    Layout layout() const { return layout_; }

private:
    struct Range {
        uint8_t* next;
        uint8_t* end;
        uintptr_t left() const { return uintptr_t(end - next); }
    };
    CodeArena(uint8_t* base, size_t size, size_t half, Layout layout);
    uint8_t* base_;
    size_t size_;
    size_t code_size_;
    Layout layout_;
    Range hot_;
    Range cold_;  // Shared: next is its lowest byte, end the hot code's start
    Range data_;
};

// Writes to an arena's code by this thread, for as long as it lives (scopes
// nest): what loads code into an engine, or copies code into an arena, makes
// one. Nothing elsewhere: a MAP_JIT page is writable or executable per
// thread, and no code runs while a thread may write.
class JitWriteScope {
public:
    JitWriteScope();
    ~JitWriteScope();
    JitWriteScope(const JitWriteScope&) = delete;
    JitWriteScope& operator=(const JitWriteScope&) = delete;
};

// The arenas of one JIT engine. An object goes whole into the current arena
// or, when it does not fit, into a new one, so that its sections stay within
// reach of each other and no section is ever refused (RuntimeDyld would stop
// the process). An object no arena can take -- larger than an arena, or when
// no further one can be mapped -- gets a memory manager of its own.
class CodeArenaPool {
public:
    explicit CodeArenaPool(size_t half = CodeArena::kDefaultHalf, size_t max_arenas = CodeArena::kDefaultArenas,
                           CodeArena::Layout layout = CodeArena::kDefaultLayout)
        : half_(half), max_arenas_(max_arenas), layout_(layout) {}

    // The arena an object of this much code and data goes into, or null.
    std::shared_ptr<CodeArena> arenaFor(uintptr_t code, uintptr_t data);
    // Arenas mapped so far.
    size_t arenas() const { return arenas_; }

private:
    size_t half_;
    size_t max_arenas_;
    CodeArena::Layout layout_;
    size_t arenas_ = 0;
    std::shared_ptr<CodeArena> current_;
};

// The memory manager of one object. RuntimeDyld tells it the object's sizes
// before it allocates a section (reserveAllocationSpace), and it places the
// whole object in the arena the pool gives it, or else in a
// SectionMemoryManager of its own. The arena outlives it: it stays mapped
// until the last object placed in it is gone. Memory is never handed back
// before then: LLJIT removes no object while it lives (Core::invalidate
// replaces the whole engine).
class ArenaMemoryManager : public llvm::RTDyldMemoryManager {
public:
    explicit ArenaMemoryManager(std::shared_ptr<CodeArenaPool> pool) : pool_(std::move(pool)) {}

    bool needsToReserveAllocationSpace() override { return true; }
    void reserveAllocationSpace(uintptr_t code, uint32_t code_align, uintptr_t ro_data,
                                uint32_t ro_align, uintptr_t rw_data, uint32_t rw_align) override;
    uint8_t* allocateCodeSection(uintptr_t size, unsigned alignment, unsigned id,
                                 llvm::StringRef name) override;
    uint8_t* allocateDataSection(uintptr_t size, unsigned alignment, unsigned id,
                                 llvm::StringRef name, bool read_only) override;
    bool finalizeMemory(std::string* error = nullptr) override;

    // Whether the object went into an arena (else into its own manager).
    bool inArena() const { return arena_ != nullptr; }

private:
    bool ownManager();
    std::shared_ptr<CodeArenaPool> pool_;
    std::shared_ptr<CodeArena> arena_;
    std::unique_ptr<llvm::SectionMemoryManager> own_;
    std::vector<std::pair<uint8_t*, uintptr_t>> code_;  // to flush at finalizeMemory
};

}  // namespace bcore
