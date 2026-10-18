#include "code_arena.h"

#include <sys/mman.h>

#include <llvm/Support/Memory.h>

namespace {
constexpr size_t kAlign = size_t(2) << 20;
// Room an object needs beyond the sizes RuntimeDyld reports, for the padding
// that aligns each of its sections.
constexpr uintptr_t kSlack = 4096;
}

std::shared_ptr<CodeArena> CodeArena::create(size_t half) {
    const size_t size = 2 * half;
    void* raw = mmap(nullptr, size + kAlign, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    // Trim to a 2 MiB-aligned start.
    auto* begin = static_cast<uint8_t*>(raw);
    auto* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(begin) + kAlign - 1) & ~(kAlign - 1));
    if (base != begin) munmap(begin, base - begin);
    if (size_t tail = (begin + size + kAlign) - (base + size)) munmap(base + size, tail);
    if (mprotect(base, half, PROT_READ | PROT_WRITE | PROT_EXEC) != 0 ||
        mprotect(base + half, half, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, size);
        return nullptr;
    }
    return std::shared_ptr<CodeArena>(new CodeArena(base, size, half));
}

CodeArena::CodeArena(uint8_t* base, size_t size, size_t half)
    : base_(base), size_(size),
      hot_{base, base + half / 4 * 3}, cold_{base + half / 4 * 3, base + half},
      data_{base + half, base + size} {}

CodeArena::~CodeArena() { munmap(base_, size_); }

bool CodeArena::fits(uintptr_t code, uintptr_t data) const {
    return code + kSlack <= hot_.left() && code + kSlack <= cold_.left() && data + kSlack <= data_.left();
}

uint8_t* CodeArena::allocate(Region region, uintptr_t size, unsigned alignment) {
    Range& range = region == Region::Hot ? hot_ : region == Region::Cold ? cold_ : data_;
    const uintptr_t align = alignment ? alignment : 1;
    const uintptr_t at = (reinterpret_cast<uintptr_t>(range.next) + align - 1) & ~(align - 1);
    if (at > reinterpret_cast<uintptr_t>(range.end) || size > reinterpret_cast<uintptr_t>(range.end) - at)
        return nullptr;
    range.next = reinterpret_cast<uint8_t*>(at + size);
    return reinterpret_cast<uint8_t*>(at);
}

std::shared_ptr<CodeArena> CodeArenaPool::arenaFor(uintptr_t code, uintptr_t data) {
    if (current_ && current_->fits(code, data)) return current_;
    // Larger than an empty arena (its cold region is a quarter of the code half).
    if (code + kSlack > half_ / 4 || data + kSlack > half_) return nullptr;
    if (arenas_ == max_arenas_) return nullptr;
    auto arena = CodeArena::create(half_);
    if (!arena) return nullptr;
    ++arenas_;
    // The previous arena stays mapped while its objects live.
    current_ = std::move(arena);
    return current_;
}

// Each size is rounded up to its alignment once more, for the padding before
// the first section of each kind.
void ArenaMemoryManager::reserveAllocationSpace(uintptr_t code, uint32_t code_align, uintptr_t ro_data,
                                                uint32_t ro_align, uintptr_t rw_data, uint32_t rw_align) {
    arena_ = pool_->arenaFor(code + code_align, ro_data + ro_align + rw_data + rw_align);
    if (!arena_) own_ = std::make_unique<llvm::SectionMemoryManager>();
}

// Unless RuntimeDyld allocates without reserving first, which it does not:
// then the object gets its own manager too.
bool ArenaMemoryManager::ownManager() {
    if (!arena_ && !own_) own_ = std::make_unique<llvm::SectionMemoryManager>();
    return own_ != nullptr;
}

// After fits(), a section the arena still refused would stop the process.
uint8_t* ArenaMemoryManager::allocateCodeSection(uintptr_t size, unsigned alignment, unsigned id,
                                                 llvm::StringRef name) {
    if (ownManager()) return own_->allocateCodeSection(size, alignment, id, name);
    const bool cold = name.startswith(".text.split");
    uint8_t* p = arena_->allocate(cold ? CodeArena::Region::Cold : CodeArena::Region::Hot, size, alignment);
    if (p) code_.emplace_back(p, size);
    return p;
}

uint8_t* ArenaMemoryManager::allocateDataSection(uintptr_t size, unsigned alignment, unsigned id,
                                                 llvm::StringRef name, bool read_only) {
    if (ownManager()) return own_->allocateDataSection(size, alignment, id, name, read_only);
    return arena_->allocate(CodeArena::Region::Data, size, alignment);
}

// In an arena permissions stay as mapped; only the instruction cache (a no-op
// on x86). Its own manager applies them as the default one does.
bool ArenaMemoryManager::finalizeMemory(std::string* error) {
    if (own_) return own_->finalizeMemory(error);
    for (const auto& [p, size] : code_) llvm::sys::Memory::InvalidateInstructionCache(p, size);
    code_.clear();
    return false;
}
