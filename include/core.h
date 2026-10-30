#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "bcore_profile.h"
#include "cpu_state.h"
#include "mem.h"

namespace bcore {

class JitEngine;
class Compiler;
class CompileWorker;
class BBTranslator;
class CpTranslator;
namespace bcore_profile { struct ProfileState; }

class Core {
public:
    Core(CpuState* cpu, Memory* mem);
    ~Core();

    // opt_level: the IR pass pipeline, 0=None, 1=O1, 2=O2, 3=O3.
    // codegen_level: the backend on the same scale (0=None, 1=Less, 2=Default,
    // 3=Aggressive); it follows opt_level when negative.
    bool init(int opt_level = 2, int codegen_level = -1);
    // LLVM (the Compiler, the lifter) made when a block first needs it, one
    // copy-and-patch does not take or a promotion, not by init (off by
    // default): on WebAssembly a host may load LLVM's code only then. Its
    // failure is then that block's, not init's. Before init().
    void set_llvm_on_demand(bool on_demand) { llvm_on_demand_ = on_demand; }
    void invalidate();
    bool run(uint32_t pc);
    std::tuple<std::string, uint32_t> disassemble(uint32_t pc);
    void set_dump_ir(bool enable);
    // PCs a chained block may run next, itself included (see
    // CpuState::chain_budget); the host runs every block starting at a PC
    // the filter rejects. Setting it drops every compiled block
    // (invalidate()), so it applies to every block from now on; it persists
    // across invalidate(). The filter is called on the run() thread only,
    // when a block is compiled or looked up: its answer for a PC may change
    // from false to true (the block is published at its next lookup, and
    // runs itself again natively only once compiled anew), but to reject a
    // PC it accepted before, set the filter again.
    void set_chain_filter(std::function<bool(uint32_t)> filter);
    // Blocks compiled from now on may run the next block themselves instead
    // of returning, while CpuState::chain_budget lasts (never in limited
    // mode). With dry, such a block sets CpuState::chain_would and returns
    // instead (to check a host's budget against what it then does).
    void set_chaining(bool chaining, bool dry = false);
    // The host writes CYCLES before every run (as op1emu does): a chained
    // block that accesses CYCLES then writes it first, to
    // CpuState::cycles_base plus the packets chained before it, as such a
    // host would have. Off by default (bcore's emu leaves CYCLES alone).
    void set_host_cycles(bool host_cycles);
    // Whether blocks compiled from now on count CpuState::chain_blocks (on
    // by default). Off, a run leaves it 0: one memory increment less per
    // block run, for a host that does not count block runs.
    void set_count_chained(bool count);
    // Blocks compiled from now on store pc, insn_len and packets only where
    // a helper call, a return or a load can read them (on by default; off
    // stores them at every packet, as before). Either way every reader sees
    // the same values.
    void set_lazy_state(bool lazy);
    // Blocks compiled from now on that run themselves again in a native loop
    // (a hardware loop, a jump to their start) keep the guest state in SSA
    // values across the loop instead of in CpuState, copying it out before
    // each helper call and exit (BBTranslator::set_loop_blocks; on by
    // default). Either way every reader sees the same values.
    void set_loop_blocks(bool loop);
    // Every lifted module is checked with LLVM's verifier, which reports a
    // broken one on stderr (on by default). It costs about 1% of
    // compilation; a host that has its lifter tested elsewhere (bcore's
    // tests and ISA suite verify) can turn it off.
    void set_verify(bool verify);
    // Tiered compilation (0, the default, for none). Every block is first
    // compiled at tier 0: the plain block (never a loop block), with no IR
    // pipeline and the backend at CodeGenOpt::None, counting its starts.
    // One that starts threshold times is compiled again after that run as
    // an untiered compilation would compile it (tier 1), from the
    // instructions tier 0 read and with the guest state tier 0 saw, and
    // replaces it. The guest does the same work either way; most blocks run
    // too rarely for what the full pipeline costs to pay. A threshold takes
    // effect for blocks compiled from then on. With size_ref, a block of
    // more packets than that starts as many times more before it is
    // compiled again (tier_threshold in tier_table.h).
    void set_tiering(uint32_t threshold, uint32_t size_ref = 0);
    // Promotions of blocks other than loop blocks optimized with
    // Compiler::kLightPipeline rather than the default pipeline: cheaper to
    // compile, for code that runs about as fast. Off by default; untiered
    // compilations are not affected.
    void set_light_tier1(bool light) { light_tier1_ = light; }
    // Tier 0 copies its blocks together from stencils built with the core
    // (cp_translator.h) where it can, microseconds where LLVM takes a
    // millisecond: any block with a page table (no fastmem, no raw memory)
    // and no step limit, but one with a packet the build has no stencil for
    // (an illegal parallel packet; a parallel one where the Memory's
    // callbacks may change registers), which it compiles with LLVM as
    // before. Exact either way. Off by default; on x86-64 and AArch64 only
    // (elsewhere this does nothing).
    void set_copy_patch(bool enable);
    // Copy-and-patch blocks take the CPU state they are translated in as a
    // hint, as LLVM's tier 0 does (CpTranslator::Config::hint): a parallel
    // packet the hint puts at no loop bottom compares none, one at a bottom
    // steps the loop inline (to the hint's top behind the block's entry
    // guard), instead of calling bfin_hwloop_step. Exact either way. On by
    // default.
    void set_copy_patch_hints(bool enable) { copy_patch_hints_ = enable; }
    // WebAssembly, copy-and-patch and tiering on (a threshold, if only
    // UINT32_MAX): a block's first `threshold` starts make no module of its
    // own. Its pieces run one at a time, as functions every block shares
    // (cp_cold_wasm.h), careful and without chaining (a chained block that
    // goes to it returns to the host). At its next start after that it is
    // compiled as copy-and-patch would have at its first, with the CPU
    // state of that start as its hint, and goes on counting its starts
    // towards set_tiering's threshold. Exact either way: the same stencils.
    // 0 (the default) for none; false, and nothing set, where there are
    // none (natively a block costs microseconds).
    bool set_cold_threshold(uint32_t threshold);
    // Blocks leaving the cold tier are compiled `blocks` at a time into one
    // module (1, the default: each at once), running cold while they wait,
    // but for no more than `age` packets after the first of them asked
    // (0: no limit). For blocks from now on.
    void set_cold_batch(uint32_t blocks, uint64_t age = 0);
    // For tests: how far from a block (bytes) its arena's helper jumps and
    // exits may lie, 0 for as far as the host's calls reach (all of an arena
    // on x86-64; under 128 MiB on AArch64). A short distance makes a set for
    // every few blocks, as a full arena on AArch64 does; the number made so
    // far, for this engine (copy_patch_support_sets).
    void set_copy_patch_reach(size_t bytes);
    unsigned copy_patch_support_sets() const;
    // Promotions compiled on a thread of their own (off by default: on the
    // dispatching thread, after the run that asked). The dispatching thread
    // still lifts each one (reading the guest's code may call the host) and
    // loads the code; the worker runs the IR pipeline and the backend. A
    // promotion's code takes the tier-0 block's place at the start of a run:
    // with publish_after 0, at the first run after the worker finished; with
    // publish_after N, at the first run after N more packets ran since the
    // promotion was asked for, waiting for the worker if it has not finished,
    // so that which code runs when does not depend on the host's speed
    // (deterministic guest time needs that: SteadyLoopSkip, for one, skips
    // a loop's device reads only in its tier-1 code). on_start runs first on
    // the worker thread. Before init() or between runs. False, and nothing
    // set, for a worker where this build has no threads (WebAssembly without
    // pthreads, kHasThreads).
    static constexpr bool kHasThreads =
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        false;
#else
        true;
#endif
    bool set_tier_background(bool background, uint64_t publish_after = 0,
                             std::function<void()> on_start = {});
    // Blocks compiled from now on end at an IDLE and return to the host with
    // CpuState::idle set, so it can let guest time pass until a wakeup (off
    // by default: IDLE does nothing, as for a host with nothing to wait for).
    // While *wake (if given) or an interrupt may be pending, the wakeup is now
    // and an IDLE does nothing either: the host sets it where a wakeup source
    // is already asserted.
    void set_idle_waits(bool waits, const uint8_t* wake = nullptr);
    // Loop blocks compiled from now on skip from a steady iteration to their
    // last (steady_skip.h): 0 off (the default), 1 dry (they count the
    // iterations they would skip in CpuState::steady_iterations), 2 on. Only
    // for a host whose memory callbacks count the reads they cannot promise
    // would repeat in CpuState::unstable_reads: without that every read
    // callback would be taken to repeat.
    void set_steady_skip(int mode);
    // Passes (PassBuilder text) run on every block after the default IR
    // pipeline, from the next compilation on; empty for none. The default is
    // kDefaultPostPipeline. False, and unchanged, if it does not parse or init
    // has not run.
    bool set_post_pipeline(const std::string& pipeline);
    static const char* const kDefaultPostPipeline;

    // ---- JIT observability (contract in bcore_profile.h) ----

    // Iterate every currently-compiled block with its host address and exact
    // size. The map is rebuilt by the JIT load listener; after invalidate()
    // only blocks compiled since then appear.
    void forEachCompiledBlock(const std::function<void(const BcoreBlockInfo&)>& fn) const;

    // Attach a typed event sink. Not owned; must outlive the Core or be
    // detached with setEventSink(nullptr) first.
    void setEventSink(BcoreEventSink* sink);

    // Counter snapshot for hosts that want numbers without per-event cost.
    BcoreStats stats() const;

    // Enable before capture. Retain at most capacity unloaded code ranges;
    // live ranges are always available. Loss is explicit, never reattributed.
    void setProfileHistory(size_t capacity);
    void forEachProfileBlock(const std::function<void(const BcoreBlockInfo&)>& fn) const;
    uint64_t profileHistoryDropped() const;

    // False when the object linking layer doesn't support JIT event
    // listeners; the code map then reports host_size == 0 (lookup-derived
    // addresses only).
    bool profileHasBlockSizes() const;

    // Runtime switch for jitdump emission (`perf inject --jit`). Takes effect
    // immediately on the current engine (safe any time after init(),
    // including from a running boot) and persists across invalidate():
    // enabling writes every block compiled from then on, disabling stops
    // that; records already written stay in the dump. Only has an effect in
    // builds with BCORE_PERF_JIT_EVENTS=ON.
    void set_perf_jitdump(bool enable);

private:
    void applyModuleTarget();
    // LLVM's part of init (set_llvm_on_demand); the tier-0 Compiler.
    bool init_llvm();
    bool init_tier0_compiler();
    // Translates, optimizes and adds the block at pc (careful: see
    // BBTranslator::translate); null if that fails.
    // promote: the tier-0 compilation to compile again at tier 1.
    struct Tier0Record;
    void (*compile(uint32_t pc, bool careful, Tier0Record* promote = nullptr))(CpuState*, Memory*);
    // Compiles again at tier 1 the blocks that asked to be (set_tiering).
    void promote();
    void note_page_table();
    // Lifts a promotion for the worker (set_tier_background).
    void queue_promotion(Tier0Record& record, uint32_t counter);
    // Loads and publishes the worker's promotions that are due (all: every
    // one sent, waiting for the worker).
    void collect_promotions(bool all = false);
    void startWorker();
    void stopWorker();
    // The cold tier (set_cold_threshold): pc's cold block, made where it has
    // none yet (null if it cannot be one); runs it; compiles the blocks that
    // left it (all those queued, or pc's alone); whether one may be made now.
    struct Cold;
    Cold* cold_block(uint32_t pc);
    bool run_cold(uint32_t pc, Cold& cold);
    void leave_cold(const std::vector<uint32_t>& pcs);
    bool can_copy_patch(Memory& code) const;
    // A run's CpuState before its first block, and its accounting after
    // its last (the packets, promotions).
    void begin_run(uint32_t pc);
    void end_run();
    // (Re)registers the code-map listener on jit_. Called by init/invalidate.
    void attachProfiler();

    CpuState* cpu_;
    Memory* mem_;
    // Memory::page_table() as last seen non-null at a translation; Core::run
    // copies it into CpuState::page_table.
    uint8_t* const* page_table_ = nullptr;
    // Declared before jit_ on purpose: a JitEngine being destroyed frees its
    // objects through the registered listener, which writes into profile_ --
    // so profile_ must outlive any engine.
    std::unique_ptr<bcore_profile::ProfileState> profile_;
    std::shared_ptr<JitEngine> jit_;
    // Kept across invalidate(): it holds no compiled code.
    std::unique_ptr<Compiler> compiler_;
    std::shared_ptr<BBTranslator> translator_;
    int opt_level_ = 2;
    int codegen_level_ = -1;
    bool dump_ir_ = false;
    bool llvm_on_demand_ = false;
    std::function<bool(uint32_t)> chain_filter_;
    bool chaining_ = false;
    bool chain_dry_ = false;
    bool host_cycles_ = false;
    bool count_chained_ = true;
    bool lazy_state_ = true;
    bool loop_blocks_ = true;
    bool verify_ = true;
    uint32_t tier_threshold_ = 0;
    uint32_t tier_size_ref_ = 0;
    bool light_tier1_ = false;
    bool copy_patch_ = false;
    bool copy_patch_hints_ = true;  // set_copy_patch_hints
    size_t copy_patch_reach_ = 0;  // set_copy_patch_reach
    std::unique_ptr<CpTranslator> cp_;  // with jit_ (set_copy_patch)
    bool unlimited_ = true;             // no step limit at init
    std::unique_ptr<Compiler> tier0_compiler_;
    // The tier-0 compilations of this engine, by counter (TierTable), until
    // promoted; and for each PC the counter of the tier-0 code compiled for
    // it now, until it is promoted or replaced.
    std::vector<std::unique_ptr<Tier0Record>> tier0_;
    std::unordered_map<uint32_t, uint32_t> tier0_current_;
    // For each PC the counter of its latest tier-0 compilation, promoted or
    // not: a promotion from an earlier one is not to replace it.
    std::unordered_map<uint32_t, uint32_t> tier0_latest_;
    // set_tier_background: the worker, the engine generation its jobs are
    // for (invalidate() makes a new one), the packets run so far, and the
    // promotions sent and not yet taken back, oldest first.
    bool tier_background_ = false;
    uint64_t publish_after_ = 0;
    std::function<void()> worker_start_;
    std::unique_ptr<CompileWorker> worker_;
    uint64_t generation_ = 0;
    uint64_t packets_run_ = 0;
    struct Pending {
        uint32_t pc, counter;
        uint64_t due;  // packets_run_ at which it is published (publish_after)
    };
    std::deque<Pending> pending_;
    // The cold tier: its threshold, the blocks in it, those that left it
    // and wait for a batch (oldest first), and packets_run_ when the first
    // of them asked.
    uint32_t cold_threshold_ = 0;
    uint32_t cold_batch_ = 1;
    uint64_t cold_age_ = 0;
    std::unordered_map<uint32_t, std::unique_ptr<Cold>> cold_;
    std::vector<uint32_t> cold_queue_;
    uint64_t cold_queued_at_ = 0;
    bool idle_waits_ = false;
    int steady_skip_ = 0;
    const uint8_t* idle_wake_ = nullptr;
    std::string post_pipeline_ = kDefaultPostPipeline;
};

}  // namespace bcore
