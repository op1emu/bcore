#pragma once

#include <cstddef>
#include <cstdint>

#include "chain_table.h"

// What tier-0 blocks count (Core::set_tiering). It lies right after the
// chain table, in the same allocation: generated code finds it at
// CpuState::chain_table + kTierTableOffset, so the code holds no host
// address, as with the chain table itself. A new engine (Core::invalidate)
// starts with a new table, all zero.
struct TierTable {
    static constexpr unsigned kCounterBits = 16;
    static constexpr uint32_t kCounters = 1u << kCounterBits;
    static constexpr uint32_t kRequests = 256;
    // Entries of each tier-0 compilation, by its counter: every start of the
    // block, a run of its native loop's included.
    uint32_t count[kCounters];
    // Requests since the host last took them (Core::run, after a run): a
    // block whose count reached the threshold asks once, with its counter.
    // There may be more than kRequests; request[] holds the first of them.
    uint32_t requests;
    uint32_t request[kRequests];
};

static constexpr size_t kTierTableOffset = sizeof(ChainSlot) << kChainBits;

// A tier-0 block's threshold (Core::set_tiering): the threshold for a block
// of at most size_ref packets, and as many times more as a larger block has
// packets more (size_ref 0: the threshold for every block). Compiling a
// block again costs in proportion to its size, and most large blocks that
// reach the threshold during the boot run little after it.
inline uint32_t tier_threshold(uint32_t threshold, uint32_t size_ref, uint32_t packets) {
    if (!size_ref || packets <= size_ref) return threshold;
    const uint64_t scaled = uint64_t(threshold) * packets / size_ref;
    return scaled > UINT32_MAX ? UINT32_MAX : uint32_t(scaled);
}
