#include "jit_engine.h"

#include <atomic>
#include <cstddef>

namespace bcore {

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() = default;

bool JitEngine::init() {
    if (!loader_.init()) return false;
    // Pre-allocate hash buckets to avoid rehash overhead during execution.
    cache_.reserve(1024);
    static_assert(offsetof(ChainArea, tier) == kTierTableOffset, "generated code finds the tier table there");
    area_ = std::make_unique<ChainArea>();  // value-initialized: the tier table all zero
    chain_ = area_->slots;
    for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};
    return true;
}

bool JitEngine::addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object,
                          uint32_t fallthrough_pc, const std::string& name) {
    BbFunc fn = loader_.load(pc, std::move(object), name);
    if (!fn) return false;
    addCode(pc, fn, fallthrough_pc);
    return true;
}

// The old function released after the new one is published: the table's
// slot of pc then has the new one (or, where the filter rejects pc, never
// had the old: setChainFilter emptied the table).
void JitEngine::addCode(uint32_t pc, BbFunc fn, uint32_t fallthrough_pc) {
    BBInfo& info = cache_[pc];
    const BbFunc old = info.fn;
    info = BBInfo{fn, fallthrough_pc};
    publishChain(pc, fn);
    if (old && old != fn) loader_.release(old);
}

// The function before the PC: generated code that matches the PC calls the
// function, so it must never see a new PC with the old function. Single
// threaded, so ordering the compiler's stores is enough.
void JitEngine::publishChain(uint32_t pc, BbFunc fn) {
    if (chain_filter_ && !chain_filter_(pc)) return;
    ChainSlot& slot = chain_[chainIndex(pc)];
    slot.pc = kChainEmpty;
    std::atomic_signal_fence(std::memory_order_release);
    slot.fn = fn;
    std::atomic_signal_fence(std::memory_order_release);
    slot.pc = pc;
}

void JitEngine::setChainFilter(std::function<bool(uint32_t)> filter) {
    chain_filter_ = std::move(filter);
    if (chain_)
        for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};
}

BbFunc JitEngine::lookup(uint32_t pc) {
    const ChainSlot& slot = chain_[chainIndex(pc)];
    if (slot.pc == pc) return slot.fn;
    auto it = cache_.find(pc);
    if (it == cache_.end())
        return nullptr;
    // Lost its slot to a block with the same index, or filtered.
    publishChain(pc, it->second.fn);
    return it->second.fn;
}

}  // namespace bcore
