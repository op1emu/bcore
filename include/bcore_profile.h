#pragma once

// bcore JIT observability API.
//
// This header is the *only* profiling interface a host application needs.
// Design rules:
//
//  * bcore is a library: it never reads environment variables, never does
//    I/O, and never prints on the success path. The host owns all sinks
//    and destinations.
//  * Everything here is (near) zero-cost when not armed: an unarmed
//    block-cost probe is a single predictable branch in the dispatch loop,
//    counters are one relaxed increment each, and the event sink is a null
//    check.
//  * The code map reports *exact* host byte sizes taken from the
//    object-file symbol table at JIT load time (via a JITEventListener).
//    Never approximate block sizes with a "distance to the next block"
//    heuristic: samples landing beyond the cap get silently misattributed.
//
// Threading: arm/disarm/query from the thread that runs Core::run(), or
// while that thread is stopped. The code map itself is mutex-protected
// because LLVM JIT load/free notifications and host iteration may race.

#include <cstdint>

// One compiled guest basic block as loaded in host memory.
struct BcoreBlockInfo {
    const void* host_addr;  // first byte of emitted machine code
    uint64_t    host_size;  // exact byte size (0 = unknown; listener unavailable)
    uint32_t    guest_pc;   // guest address of the block entry point
    uint32_t    variant;    // 0 = default variant; reserved for multi-variant engines
};

// Typed event sink. All callbacks default to no-ops; override what you need.
// Callbacks fire synchronously on the dispatching thread.
class BcoreEventSink {
public:
    virtual ~BcoreEventSink() = default;

    virtual void onCacheHit(uint32_t guest_pc) { (void)guest_pc; }
    virtual void onCacheMiss(uint32_t guest_pc) { (void)guest_pc; }
    virtual void onTranslateBegin(uint32_t guest_pc) { (void)guest_pc; }
    virtual void onTranslateEnd(uint32_t guest_pc, uint64_t elapsed_ns) {
        (void)guest_pc; (void)elapsed_ns;
    }

    // Per-execution dispatch notification. This fires once per executed
    // basic block — a virtual call per block is *not* free. Core only calls
    // it when wantsDispatch() returns true, so sinks that don't need it pay
    // nothing beyond the shared null check.
    virtual void onDispatch(uint32_t guest_pc) { (void)guest_pc; }
    virtual bool wantsDispatch() const { return false; }
};

// Monotonic counters, cheap enough to keep always on (one relaxed increment
// per event on the dispatch path). Returned by value from Core::stats().
struct BcoreStats {
    uint64_t blocks_translated  = 0;
    uint64_t blocks_executed    = 0;
    uint64_t cache_hits         = 0;
    uint64_t cache_misses       = 0;
    uint64_t translate_ns_total = 0;
};
