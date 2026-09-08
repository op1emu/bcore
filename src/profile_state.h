#pragma once

// Internal state behind Core's observability API (see include/bcore_profile.h
// for the public contract). Not installed; only src/ files include this.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "bcore_profile.h"

namespace llvm { class JITEventListener; }
class JitEngine;

namespace bcore_profile {

// Measured CLOCK_MONOTONIC spin; same envelope for sham and nonzero doses.
// No sleeping, scheduling/real-time coupling may still change by design.
uint64_t spin_ns(uint64_t ns);
uint64_t monotonic_ns();

struct ProfileState {
    ProfileState();
    ~ProfileState();  // out-of-line: owns llvm::JITEventListener (incomplete here)

    struct Block {
        uint64_t size;
        uint32_t guest_pc;
        uint32_t variant;
        uint64_t loaded_ns;
    };
    // host address -> block, sorted by address so hosts can also do their
    // own range lookups. Protected by blocks_mutex (JIT load/free
    // notifications and host-side iteration may race with dispatch).
    std::map<uint64_t, Block> blocks;
    // JIT object key -> block addresses that object contributed, so
    // notifyFreeingObject can remove exactly those entries.
    std::map<uint64_t, std::vector<uint64_t>> blocks_by_object;
    std::mutex blocks_mutex;
    // False when the linking layer doesn't support JITEventListener (the
    // map then only has entries with size 0 from the lookup fallback).
    bool listener_ok = false;

    BcoreEventSink* sink = nullptr;   // not owned
    bool sink_wants_dispatch = false;
    bool sink_wants_cache_hits = false;
    bool perf_attached = false;
    size_t history_capacity = 0;
    uint64_t history_dropped = 0;
    std::vector<BcoreBlockInfo> retired;
    void retire(uint64_t addr, const Block& block, uint64_t now);
    BcoreStats stats{};

    // Runtime opt-in for LLVM's PerfJITEventListener (jitdump output for
    // `perf inject --jit`). Set by Core::set_perf_jitdump(), which also
    // attaches immediately via attach_perf_listener() below.
    bool want_perf_jitdump = false;

    void clearBlocks();
    void recordBlock(uint64_t object_key, uint64_t addr, uint64_t size,
                     uint32_t guest_pc);
    void freeObject(uint64_t object_key);

    std::unique_ptr<llvm::JITEventListener> map_listener;  // owns BlockMapListener
    // NON-owning: llvm::JITEventListener::createPerfJITEventListener()
    // returns a pointer to a function-local `static` object (confirmed via
    // nm: a .bss symbol with its own initialization guard variable), not a
    // heap allocation. LLVM manages its lifetime as a process-wide
    // singleton; wrapping it in unique_ptr and letting that delete it at
    // Core teardown is a free() on a non-heap pointer (reproduced: glibc
    // "free(): invalid pointer", SIGABRT, during shutdown).
    llvm::JITEventListener* perf_listener = nullptr;
};

// (Re)creates and registers listeners on a fresh JitEngine. Called by
// Core::init()/invalidate(). Returns false when the linking layer does not
// support JIT event listeners (host should then treat sizes as unavailable).
bool attach_listeners(ProfileState& state, JitEngine& jit);

// Registers (creating if needed) the LLVM perf jitdump listener on a JIT
// engine that is already live. Split out from attach_listeners() so
// Core::set_perf_jitdump() can attach immediately without touching
// map_listener: replacing that unique_ptr while its old raw pointer may
// still be registered inside RTDyldObjectLinkingLayer's listener list (it
// does not take ownership) would be a use-after-free. No-op, returns false,
// if !want_perf_jitdump or the build lacks BCORE_PERF_JIT_EVENTS.
bool attach_perf_listener(ProfileState& state, JitEngine& jit);

} // namespace bcore_profile
