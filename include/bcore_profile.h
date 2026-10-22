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
#include <string>

// One compiled guest basic block as loaded in host memory.
struct BcoreBlockInfo {
    const void* host_addr;  // first byte of emitted machine code
    uint64_t    host_size;  // exact byte size (0 = unknown; listener unavailable)
    uint32_t    guest_pc;   // guest address of the block entry point
    uint32_t    variant;    // 0 = default variant; reserved for multi-variant engines
    uint64_t    loaded_ns = 0;   // CLOCK_MONOTONIC; 0 = history unavailable
    uint64_t    unloaded_ns = 0; // exclusive lifetime end; 0 = still live
};

// Shape of one compiled block, for checking that a setting changed the code
// (a nonzero opt level once changed nothing for months) and for relating
// compile time to code size.
struct BcoreCompileMetrics {
    uint64_t ir_instructions = 0;  // the module as compiled: after the IR pipeline
    uint64_t ir_blocks       = 0;
    uint64_t object_bytes    = 0;  // the block function's machine code; 0 = unknown
    // FNV-1a over that machine code before relocation, its relocations
    // (offset, type, target symbol, addend) and the sections they point into
    // (constant pools, jump tables, function-pointer tables) with their own
    // relocations, transitively: the same for the same
    // code in any process, independent of where helpers are loaded. Not
    // across processes when the Memory has a nonzero fast_base(): the
    // fastmem and rawmem paths embed that host address in the code as a
    // constant, without a relocation. 0 = unknown (no code map).
    uint64_t object_hash     = 0;
    // All time spent collecting these metrics: counting the IR, copying the
    // code while LLVM loads it, and hashing it afterwards. The first two
    // happen during the translation and are taken out of its elapsed time
    // and translate_ns_total; the copy falls inside the "materialize" stage.
    // A host timing the whole translation itself can subtract collect_ns.
    uint64_t collect_ns      = 0;
    // Where decoding ended the block: the kind of its terminating
    // instruction ("jump", "call", "return", "return-from-event",
    // "jump-indirect", "call-indirect", "loop-setup", "raise", "exception",
    // "halt", "idle", "illegal", "unimplemented"), or "packet-cap" when it reached
    // the 256-packet block limit, or "step-budget" when it reached a
    // limited-mode budget of at most 256 packets. A static string; never
    // null.
    //
    // It classifies the block as decoded, not each run: a run can leave
    // earlier, through a taken conditional branch, a hardware loop, an
    // exception or the step budget, and still counts under this label. Run
    // counts summed by terminator are therefore a static classification of
    // the executed blocks, not their runtime exit reasons.
    const char* terminator   = "packet-cap";
    // Core::set_tiering: -1 for an untiered compilation, 0 for a tier-0
    // block (counting, cheap to compile), 1 for its promotion.
    int tier                 = -1;
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

    // Compile metrics, once per successful translation, after the stages and
    // just before onTranslateEnd. Only when wantsCompileMetrics() returns
    // true; it is asked before the translation is timed. Counting the IR and
    // copying the code cost a pass over each: that time (collect_ns) is
    // removed from the elapsed time, and the hash is computed after timing.
    virtual bool wantsCompileMetrics() const { return false; }

    // The LLVM IR of chosen blocks, as text: "lifted" straight from the
    // decoder, then "optimized" after the IR pipeline when opt_level != 0
    // (at opt_level 0 the lifted IR is what is compiled). Asked before the
    // translation is timed; the text is printed between stages, its time is
    // taken out of the elapsed time, and the callbacks fire after the
    // stages, before onCompileMetrics. Only for blocks the sink asks for:
    // printing a module costs about as much as lifting it.
    virtual bool wantsIrDump(uint32_t guest_pc) const { (void)guest_pc; return false; }
    virtual void onIrDump(uint32_t guest_pc, const char* stage, const std::string& ir) {
        (void)guest_pc; (void)stage; (void)ir;
    }
    virtual void onCompileMetrics(uint32_t guest_pc, const BcoreCompileMetrics& metrics) {
        (void)guest_pc; (void)metrics;
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
    // Blocks compiled again careful after their loop-bottom entry guard
    // failed (CpuState::lb_guard_miss); each is also in blocks_translated.
    uint64_t lb_guard_misses    = 0;
    // Of the blocks translated: those compiled as loop blocks
    // (Core::set_loop_blocks), and in them the loops given a copy with
    // their guest memory proven at entry (guest_memory.h), the copies given
    // a speculative copy of a saturating sum (guest_speculation.h), and the
    // loops the vectorizer then vectorized.
    uint64_t loop_blocks        = 0;
    uint64_t loops_proven       = 0;
    uint64_t loops_speculated   = 0;
    uint64_t loops_vectorized   = 0;
    // Tiered compilation (Core::set_tiering): tier-0 blocks compiled again
    // at tier 1 (each also in blocks_translated), promotions given up (the
    // tier-0 block keeps running), and the times more blocks asked to be
    // promoted after one run than TierTable holds requests for.
    uint64_t blocks_promoted    = 0;
    uint64_t promotions_dropped = 0;
    uint64_t tier_request_overflows = 0;
    // Loops given SteadyLoopSkip's test (Core::set_steady_skip).
    uint64_t loops_steady       = 0;
    // DAG registers loop blocks took to stay in their circular buffers
    // (LiftVisitor::Circular).
    uint64_t dag_circular       = 0;
    // Interrupts and exceptions dispatched per IVG level (0 = EMU, 1 = RST,
    // 2 = NMI, 3 = EVX, ... 15 = IVG15), counted where the CEC sets the
    // IPEND bit. Guest behavior, unlike the block counters above: with the
    // same inputs and guest clock they repeat exactly. The CEC state is
    // process-wide, so they count from the last CpuState reset.
    uint64_t interrupts_entered[16] = {};
};
