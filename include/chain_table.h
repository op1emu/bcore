#pragma once

#include <cstdint>

struct CpuState;
class Memory;

// One slot of the table generated code reads to run the next block itself
// (CpuState::chain_table): direct-mapped by PC, a slot holds the block whose
// entry is `pc`, or kChainEmpty.
struct ChainSlot {
    uint32_t pc;
    uint32_t pad;
    void (*fn)(CpuState*, Memory*);
};
static constexpr unsigned kChainBits = 16;
static constexpr uint32_t kChainEmpty = UINT32_MAX;  // odd: no block starts there
// Instructions are halfword aligned, so bit 0 carries nothing; bit 1 does.
inline uint32_t chainIndex(uint32_t pc) { return (pc >> 1) & ((1u << kChainBits) - 1); }
