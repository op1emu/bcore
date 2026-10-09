#pragma once

// bcore JIT observability API.
//
// This header is the *only* profiling interface a host application needs.
// Design rules:
//
//  * bcore is a library: it never reads environment variables, never does
//    I/O, and never prints on the success path. The host owns all sinks
//    and destinations. The one exception is opt-in: set_perf_jitdump(true)
//    (with BCORE_PERF_JIT_EVENTS=ON) has LLVM's listener create and write
//    jit-<pid>.dump under $JITDUMPDIR/.debug/jit/ (or $HOME/.debug/jit/);
//    LLVM, not bcore, reads those variables.
//  * BCORE_ENABLE_PROFILE=0 compiles every instrumentation point out of
//    Core::run(). Enabled builds keep a null-sink check and owner-thread
//    counter increments on the dispatch path; that is near zero, not zero.
//  * The code map reports *exact* host byte sizes taken from the
//    object-file symbol table at JIT load time (via a JITEventListener).
//    Never approximate block sizes with a "distance to the next block"
//    heuristic: samples landing beyond the cap get silently misattributed.
//
// Threading: call the Core profiling methods from the thread that runs
// Core::run(), or while that thread is stopped. The code map itself is
// mutex-protected because JIT load/free notifications and host iteration may
// race.

#include <cstdint>

// One compiled guest basic block as loaded in host memory.
struct BcoreBlockInfo {
    const void* host_addr;  // first byte of emitted machine code
    uint64_t    host_size;  // exact byte size (0 = unknown; listener unavailable)
    uint32_t    guest_pc;   // guest address of the block entry point
    uint32_t    variant;    // 0 = default variant; reserved for multi-variant engines
    uint64_t    loaded_ns = 0;   // CLOCK_MONOTONIC; 0 = history unavailable
    uint64_t    unloaded_ns = 0; // exclusive lifetime end; 0 = still live
};

// Typed event sink for the translation (cache-miss) path. All callbacks
// default to no-ops and fire synchronously on the dispatching thread, from
// inside Core::run(). None fires on a cache hit, so attaching a sink costs
// nothing per executed block.
//
// Callbacks must not throw, and must not call back into the Core that is
// reporting (no run(), invalidate(), init(), setEventSink() or profiling
// queries): Core::run() is about to execute the block it just compiled, and
// invalidate() would destroy it. Record what you need and act afterwards.
class BcoreEventSink {
public:
    virtual ~BcoreEventSink() = default;

    // Brackets one translation, end to end. Exactly one of onTranslateEnd
    // or onTranslateFailure follows every onTranslateBegin, also when the
    // translation throws (the failure is reported, then the exception
    // propagates).
    virtual void onTranslateBegin(uint32_t guest_pc) { (void)guest_pc; }
    virtual void onTranslateEnd(uint32_t guest_pc, uint64_t elapsed_ns) {
        (void)guest_pc; (void)elapsed_ns;
    }
    virtual void onTranslateFailure(uint32_t guest_pc, uint64_t elapsed_ns) {
        (void)guest_pc; (void)elapsed_ns;
    }

    // Stages inside a translation, in order and on the CLOCK_MONOTONIC clock,
    // reported after the translation has finished (just before
    // onTranslateEnd/onTranslateFailure), so time spent in these callbacks
    // is in no stage and not in the elapsed time:
    //   "lift"         decode and emit the block's LLVM IR
    //   "ir-optimize"  the IR pass pipeline (only when opt_level != 0)
    //   "materialize"  add the module to the JIT and look the symbol up:
    //                  native code generation, object linking, symbol lookup
    // They do not cover the whole translation (an IR dump between stages is
    // excluded), so never add the stages to their parent or to each other's
    // parent; use onTranslateEnd for the total.
    virtual void onCompileStage(uint32_t guest_pc, const char* stage,
                                uint64_t begin_ns, uint64_t end_ns, bool ok) {
        (void)guest_pc; (void)stage; (void)begin_ns; (void)end_ns; (void)ok;
    }
};

// Monotonic counters in profiling builds (one owner-thread increment
// per event on the dispatch path). Returned by value from Core::stats().
struct BcoreStats {
    uint64_t blocks_translated  = 0;
    uint64_t blocks_executed    = 0;
    uint64_t cache_hits         = 0;
    uint64_t cache_misses       = 0;
    uint64_t translate_ns_total = 0;
};
