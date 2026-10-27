#include "code_arena.h"

#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#endif

#include <llvm/Support/Memory.h>

namespace {
constexpr size_t kAlign = size_t(2) << 20;
// Room an object needs beyond the sizes RuntimeDyld reports, for the padding
// that aligns each of its sections.
constexpr uintptr_t kSlack = 4096;
}

static_assert(CodeArena::kDefaultLayout == CodeArena::Layout::Split || CodeArena::kDefaultHalf <= CodeArena::kBranchReach,
              "a shared arena's code lies within B/BL's reach");

#if defined(__APPLE__) && defined(__aarch64__)
// No page there is writable and executable at once but in a MAP_JIT mapping,
// which a thread sees as one or the other (JitWriteScope). Both halves are
// mapped as one MAP_JIT range (MAP_JIT takes no MAP_FIXED, and a hint for
// the data half after it alone was not kept: every object then had pages
// of its own), the data half then replaced in place by a plain mapping,
// which this thread writes at any time.
std::shared_ptr<CodeArena> CodeArena::create(size_t half, Layout layout) {
    void* raw = mmap(nullptr, 2 * half, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    auto* base = static_cast<uint8_t*>(raw);
    void* data = mmap(base + half, half, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (data != base + half) {
        munmap(raw, 2 * half);
        return nullptr;
    }
    return std::shared_ptr<CodeArena>(new CodeArena(base, 2 * half, half, layout));
}

namespace {
thread_local unsigned write_depth = 0;
}

JitWriteScope::JitWriteScope() {
    if (write_depth++ == 0) pthread_jit_write_protect_np(0);
}

JitWriteScope::~JitWriteScope() {
    if (--write_depth == 0) pthread_jit_write_protect_np(1);
}
#else
std::shared_ptr<CodeArena> CodeArena::create(size_t half, Layout layout) {
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
    return std::shared_ptr<CodeArena>(new CodeArena(base, size, half, layout));
}

JitWriteScope::JitWriteScope() = default;
JitWriteScope::~JitWriteScope() = default;
#endif

CodeArena::CodeArena(uint8_t* base, size_t size, size_t half, Layout layout)
    : base_(base), size_(size), code_size_(half), layout_(layout),
      hot_{base, base + hotSize(half, layout)},
      cold_{layout == Layout::Split ? base + half / 4 * 3 : base + half, layout == Layout::Split ? base + half : base},
      data_{base + half, base + size} {}

CodeArena::~CodeArena() { munmap(base_, size_); }

bool CodeArena::fits(uintptr_t code, uintptr_t data) const {
    if (data + kSlack > data_.left()) return false;
    if (layout_ == Layout::Shared) return code + kSlack <= uintptr_t(cold_.next - hot_.next);
    return code + kSlack <= hot_.left() && code + kSlack <= cold_.left();
}

uint8_t* CodeArena::allocate(Region region, uintptr_t size, unsigned alignment) {
    const uintptr_t align = alignment ? alignment : 1;
    if (layout_ == Layout::Shared && region == Region::Cold) {
        // Down from the cold code placed last, as far as the hot code.
        const uintptr_t top = reinterpret_cast<uintptr_t>(cold_.next);
        const uintptr_t floor = reinterpret_cast<uintptr_t>(hot_.next);
        if (size > top - floor) return nullptr;
        const uintptr_t at = (top - size) & ~(align - 1);
        if (at < floor) return nullptr;
        cold_.next = reinterpret_cast<uint8_t*>(at);
        return cold_.next;
    }
    Range& range = region == Region::Hot ? hot_ : region == Region::Cold ? cold_ : data_;
    // Shared: the hot code goes up as far as the cold code.
    const uintptr_t end = reinterpret_cast<uintptr_t>(
        layout_ == Layout::Shared && region == Region::Hot ? cold_.next : range.end);
    const uintptr_t at = (reinterpret_cast<uintptr_t>(range.next) + align - 1) & ~(align - 1);
    if (at > end || size > end - at) return nullptr;
    range.next = reinterpret_cast<uint8_t*>(at + size);
    return reinterpret_cast<uint8_t*>(at);
}

std::shared_ptr<CodeArena> CodeArenaPool::arenaFor(uintptr_t code, uintptr_t data) {
    if (current_ && current_->fits(code, data)) return current_;
    // Larger than an empty arena (Split: its cold region is a quarter of the
    // code half).
    if (code + kSlack > CodeArena::largestCode(half_, layout_) || data + kSlack > half_) return nullptr;
    if (arenas_ == max_arenas_) return nullptr;
    auto arena = CodeArena::create(half_, layout_);
    if (!arena) return nullptr;
    ++arenas_;
    // The previous arena stays mapped while its objects live.
    current_ = std::move(arena);
    return current_;
}

// Each size is rounded up to its alignment once more, for the padding before
// the first section of each kind.
void ArenaMemoryManager::reserveAllocationSpace(uintptr_t code, bcore_llvm::SectionAlign code_align,
                                                uintptr_t ro_data, bcore_llvm::SectionAlign ro_align,
                                                uintptr_t rw_data, bcore_llvm::SectionAlign rw_align) {
    using bcore_llvm::align_value;
    arena_ = pool_->arenaFor(code + align_value(code_align),
                             ro_data + align_value(ro_align) + rw_data + align_value(rw_align));
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
    const bool cold = bcore_llvm::starts_with(name, ".text.split");
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
