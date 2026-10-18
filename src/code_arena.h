#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>

// One mapping that all of a JIT's objects are placed in, code next to code.
// LLJIT's default gives every object a SectionMemoryManager of its own, which
// maps each section separately and rounds it up to whole pages: every compiled
// block then started at offset 0 of a page of its own, so their entries
// competed for the same L1I and L2 sets and every block cost an iTLB entry.
//
// The first half of the reservation holds code, readable, writable and
// executable for as long as the arena lives (no permission changes as blocks
// are added, which would split its huge pages); the second half holds data.
// Both are within 2 GiB of each other, as the small code model's PC-relative
// references between an object's sections need. Pages are touched, not
// committed, as they fill.
//
// Single-threaded, like all of bcore's compilation.
class CodeArena {
public:
    // Null when the reservation fails.
    static std::shared_ptr<CodeArena> create();
    ~CodeArena();
    CodeArena(const CodeArena&) = delete;
    CodeArena& operator=(const CodeArena&) = delete;

    // Size bytes aligned to alignment, or null when that half is full
    // (RuntimeDyld then stops the process: the boot to the main display
    // compiles about 40 MB of code).
    uint8_t* allocate(bool code, uintptr_t size, unsigned alignment);
    // Whether p is in the code half.
    bool holdsCode(const void* p) const;

private:
    struct Half {
        uint8_t* next;
        uint8_t* end;
    };
    CodeArena(uint8_t* base, size_t size);
    uint8_t* base_;
    size_t size_;
    Half code_;
    Half data_;
};

// The memory manager of one object, placing its sections in the arena. The
// arena outlives it: it stays mapped until the last object of the JIT that
// shares it is gone. Memory is never handed back before then: LLJIT removes
// no object while it lives (Core::invalidate replaces the whole engine).
class ArenaMemoryManager : public llvm::RTDyldMemoryManager {
public:
    explicit ArenaMemoryManager(std::shared_ptr<CodeArena> arena) : arena_(std::move(arena)) {}

    uint8_t* allocateCodeSection(uintptr_t size, unsigned alignment, unsigned id,
                                 llvm::StringRef name) override;
    uint8_t* allocateDataSection(uintptr_t size, unsigned alignment, unsigned id,
                                 llvm::StringRef name, bool read_only) override;
    bool finalizeMemory(std::string* error = nullptr) override;

private:
    std::shared_ptr<CodeArena> arena_;
    std::vector<std::pair<uint8_t*, uintptr_t>> code_;  // to flush at finalizeMemory
};
