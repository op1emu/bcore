#include "code_arena.h"

#include <sys/mman.h>

#include <llvm/Support/Memory.h>

namespace {
constexpr size_t kHalf = size_t(512) << 20;
constexpr size_t kAlign = size_t(2) << 20;  // a huge page
}

std::shared_ptr<CodeArena> CodeArena::create() {
    const size_t size = 2 * kHalf;
    void* raw = mmap(nullptr, size + kAlign, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    // Trim to a 2 MiB-aligned start.
    auto* begin = static_cast<uint8_t*>(raw);
    auto* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(begin) + kAlign - 1) & ~(kAlign - 1));
    if (base != begin) munmap(begin, base - begin);
    if (size_t tail = (begin + size + kAlign) - (base + size)) munmap(base + size, tail);
    if (mprotect(base, kHalf, PROT_READ | PROT_WRITE | PROT_EXEC) != 0 ||
        mprotect(base + kHalf, kHalf, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, size);
        return nullptr;
    }
    return std::shared_ptr<CodeArena>(new CodeArena(base, size));
}

CodeArena::CodeArena(uint8_t* base, size_t size)
    : base_(base), size_(size),
      hot_{base, base + kHalf / 4 * 3}, cold_{base + kHalf / 4 * 3, base + kHalf},
      data_{base + kHalf, base + size} {}

CodeArena::~CodeArena() { munmap(base_, size_); }

uint8_t* CodeArena::allocate(Region region, uintptr_t size, unsigned alignment) {
    Range& range = region == Region::Hot ? hot_ : region == Region::Cold ? cold_ : data_;
    const uintptr_t align = alignment ? alignment : 1;
    const uintptr_t at = (reinterpret_cast<uintptr_t>(range.next) + align - 1) & ~(align - 1);
    if (at > reinterpret_cast<uintptr_t>(range.end) || size > reinterpret_cast<uintptr_t>(range.end) - at)
        return nullptr;
    range.next = reinterpret_cast<uint8_t*>(at + size);
    return reinterpret_cast<uint8_t*>(at);
}

uint8_t* ArenaMemoryManager::allocateCodeSection(uintptr_t size, unsigned alignment, unsigned,
                                                 llvm::StringRef name) {
    const bool cold = name.startswith(".text.split");
    uint8_t* p = arena_->allocate(cold ? CodeArena::Region::Cold : CodeArena::Region::Hot, size, alignment);
    if (p) code_.emplace_back(p, size);
    return p;
}

uint8_t* ArenaMemoryManager::allocateDataSection(uintptr_t size, unsigned alignment, unsigned,
                                                 llvm::StringRef, bool) {
    return arena_->allocate(CodeArena::Region::Data, size, alignment);
}

// Permissions stay as mapped; only the instruction cache (a no-op on x86).
bool ArenaMemoryManager::finalizeMemory(std::string*) {
    for (const auto& [p, size] : code_) llvm::sys::Memory::InvalidateInstructionCache(p, size);
    code_.clear();
    return false;
}
