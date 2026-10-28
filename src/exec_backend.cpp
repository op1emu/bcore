#include "exec_backend.h"

#include <atomic>
#include <cstddef>

ExecBackend::ExecBackend() = default;
ExecBackend::~ExecBackend() = default;

void ExecBackend::makeTables() {
    // Pre-allocate hash buckets to avoid rehash overhead during execution.
    cache_.reserve(1024);
    static_assert(offsetof(ChainArea, tier) == kTierTableOffset, "generated code finds the tier table there");
    area_ = std::make_unique<ChainArea>();  // value-initialized: the tier table all zero
    chain_ = area_->slots;
    for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};
}

void ExecBackend::addCode(uint32_t pc, BbFunc fn, uint32_t fallthrough_pc) {
    cache_[pc] = BBInfo{fn, fallthrough_pc};
    publishChain(pc, fn);
}

// The function before the PC: generated code that matches the PC calls the
// function, so it must never see a new PC with the old function. Single
// threaded, so ordering the compiler's stores is enough.
void ExecBackend::publishChain(uint32_t pc, BbFunc fn) {
    if (chain_filter_ && !chain_filter_(pc)) return;
    ChainSlot& slot = chain_[chainIndex(pc)];
    slot.pc = kChainEmpty;
    std::atomic_signal_fence(std::memory_order_release);
    slot.fn = fn;
    std::atomic_signal_fence(std::memory_order_release);
    slot.pc = pc;
}

void ExecBackend::setChainFilter(std::function<bool(uint32_t)> filter) {
    chain_filter_ = std::move(filter);
    if (chain_)
        for (size_t i = 0; i < (size_t(1) << kChainBits); ++i) chain_[i] = {kChainEmpty, 0, nullptr};
}

BbFunc ExecBackend::lookup(uint32_t pc) {
    const ChainSlot& slot = chain_[chainIndex(pc)];
    if (slot.pc == pc) return slot.fn;
    auto it = cache_.find(pc);
    if (it == cache_.end())
        return nullptr;
    // Lost its slot to a block with the same index, or filtered.
    publishChain(pc, it->second.fn);
    return it->second.fn;
}
