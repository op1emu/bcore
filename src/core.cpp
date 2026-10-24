#include "core.h"
#include "compile_worker.h"
#include "compiler.h"
#include "guest_memory.h"
#include "steady_skip.h"
#include "tier_memory.h"
#include "tier_table.h"

#include <optional>
#include <sstream>

#include "bb_translator.h"
#include "cec.h"
#include "decoder.h"
#include "disasm_visitor.h"
#include "jit_engine.h"
#include "mmr.h"
#include "profile_state.h"
// Also without BCORE_COPY_PATCH: Core holds the (then unused) translator.
#include "cp_translator.h"

#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

// What a tier-0 compilation saw, to compile the block again at tier 1 as
// an untiered compilation would have: its instructions, and the guest state
// it took its hints from.
struct Core::Tier0Record {
    uint32_t pc = 0;
    bool careful = false;
    uint32_t threshold = 0;  // its count's (tier_threshold)
    CpuState hint{};
    bcore_tier::CodeLog code;
};

Core::Core(CpuState* cpu, Memory* mem)
    : cpu_(cpu), mem_(mem) {
#if BCORE_ENABLE_PROFILE
    profile_ = std::make_unique<bcore_profile::ProfileState>();
#endif
}

Core::~Core() = default;

void Core::attachProfiler() {
#if BCORE_ENABLE_PROFILE
    profile_->listener_ok = bcore_profile::attach_listeners(*profile_, *jit_);
#endif
}

bool Core::init(int opt_level, int codegen_level) {
    opt_level_ = opt_level;
    codegen_level_ = codegen_level;

    compiler_ = std::make_unique<Compiler>();
    if (!compiler_->init(opt_level_, codegen_level_) || !compiler_->set_post_pipeline(post_pipeline_))
        return false;
    compiler_->set_steady_skip(steady_skip_);
    jit_ = std::make_shared<JitEngine>();
    if (!jit_->init())
        return false;
    jit_->setChainFilter(chain_filter_);
    attachProfiler();
    unlimited_ = cpu_->steps_remaining == 0;
    cp_.reset();
    set_copy_patch(copy_patch_);

    // Use the Memory interface's is_fastmem() to determine mode.
    const bool fastmem = mem_->is_fastmem();
    // In fastmem mode, fast_base is the mmap base; JIT uses direct loads.
    // In non-fastmem mode, fast_base = data_.data() - base_; pass it for rawmem inline loads.
    const uint64_t fast_base = mem_->fast_base();

    translator_ = std::make_shared<BBTranslator>(
        mem_,
        /*unlimited=*/(cpu_->steps_remaining == 0),
        fastmem,
        fast_base);
    if (cpu_->steps_remaining)
        translator_->set_max_packets(cpu_->steps_remaining);
    translator_->set_chaining(chaining_, chain_dry_);
    translator_->set_chain_filter(chain_filter_);
    translator_->set_host_cycles(host_cycles_);
    translator_->set_count_chained(count_chained_);
    translator_->set_lazy_state(lazy_state_);
    translator_->set_loop_blocks(loop_blocks_);
    translator_->set_verify(verify_);
    translator_->set_idle_waits(idle_waits_);
    translator_->set_steady_skip(steady_skip_ != 0);
    applyModuleTarget();
    if (tier_background_) startWorker();

    return true;
}

void Core::applyModuleTarget() {
    // The compiler's: IRBuilder and the IR pipeline read the layout, and the
    // backend compiles for that triple.
    translator_->setModuleTarget(compiler_->dataLayout(), compiler_->targetTriple());
}

// A translation inlines accesses through this table if there is one; the
// code finds it in CpuState (see Memory::page_table()), which Core::run sets
// from here. Before every lift, the worker's promotions' too.
void Core::note_page_table() {
    if (uint8_t* const* table = mem_->page_table())
        page_table_ = table;
}

BbFunc Core::compile(uint32_t pc, bool careful, Tier0Record* promote) {
    note_page_table();
    // Tiered (set_tiering): a promotion reads what its tier-0 compilation
    // read; a tier-0 compilation records it, while the engine has counters.
    BBTranslator::Tier tier;
    const BBTranslator::Tier* tiered = nullptr;
    const CpuState* hint = cpu_;
    std::unique_ptr<Tier0Record> record;
    std::optional<bcore_tier::RecordingMemory> recording;
    std::optional<bcore_tier::ReplayMemory> replay;
    if (promote) {
        replay.emplace(*mem_, promote->code);
        tier.level = 1;
        tier.code = &*replay;
        hint = &promote->hint;
        tiered = &tier;
    } else if (tier_threshold_ && tier0_.size() < TierTable::kCounters) {
        record = std::make_unique<Tier0Record>();
        record->pc = pc;
        record->careful = careful;
        record->hint = *cpu_;
        recording.emplace(*mem_, record->code);
        tier.level = 0;
        tier.counter = uint32_t(tier0_.size());
        tier.threshold = tier_threshold_;
        tier.size_ref = tier_size_ref_;
        tier.code = &*recording;
        tiered = &tier;
    }
    const bool tier0 = tiered && tier.level == 0;
#if BCORE_ENABLE_PROFILE
    BcoreEventSink* sink = profile_->sink;
    if (!promote) profile_->stats.cache_misses++;
    // Asked before timing starts: the sink's answer is no part of the
    // translation.
    const bool want_metrics = sink && sink->wantsCompileMetrics();
    const bool want_ir = sink && sink->wantsIrDump(pc);
    if (sink) sink->onTranslateBegin(pc);
    const uint64_t t0 = bcore_profile::monotonic_ns();
    // Balances onTranslateBegin when anything below throws (a host
    // Memory may: decoding reads guest memory through it). The exception
    // still propagates.
    struct FailOnUnwind {
        BcoreEventSink* sink;
        uint32_t pc;
        uint64_t t0;
        bool armed = true;
        ~FailOnUnwind() {
            if (armed && sink) sink->onTranslateFailure(pc, bcore_profile::monotonic_ns() - t0);
        }
    } fail_on_unwind{sink, pc, t0};
#endif
#if BCORE_COPY_PATCH
    // The copy-and-patch tier 0, where it can take the block: else the
    // block compiles with LLVM, from the instructions the copy-and-patch
    // translator read (no halfword is read from the host twice). Stencils
    // reach RAM through the page table: only while the Memory shows one now,
    // as a lift inlines through it only then (found in review: the table
    // noted earlier let them bypass a host that had hidden it to see every
    // access).
    std::optional<bcore_tier::PrefetchedMemory> prefetched;
    if (tier0 && copy_patch_ && cp_ && tier.code->page_table() && !mem_->is_fastmem() &&
        mem_->rawmem_limit() == 0 && unlimited_ &&
        lazy_state_ && !dump_ir_
#if BCORE_ENABLE_PROFILE
        && !want_ir
#endif
    ) {
        CpTranslator::Config config;
        config.chaining = chaining_;
        config.dry = chain_dry_;
        config.count_chained = count_chained_;
        config.host_cycles = host_cycles_;
        config.idle_waits = idle_waits_;
        config.threshold = tier.threshold;
        config.size_ref = tier.size_ref;
        // Its own exit goes back to its start without the table only where
        // the table would hold it: a PC the filter takes.
        config.self_loop = chaining_ && !chain_dry_ && (!chain_filter_ || chain_filter_(pc));
        const CpTranslator::Block block = cp_->translate(pc, *tier.code, tier.counter, config);
        if (block.fn) {
            jit_->addCode(pc, block.fn, block.fallthrough_pc);
            tier0_current_[pc] = tier.counter;
            tier0_latest_[pc] = tier.counter;
            record->threshold = block.threshold;
            tier0_.push_back(std::move(record));
#if BCORE_ENABLE_PROFILE
            const uint64_t t1 = bcore_profile::monotonic_ns();
            fail_on_unwind.armed = false;
            profile_->recordBlock((uint64_t(1) << 63) | reinterpret_cast<uintptr_t>(block.fn),
                                  reinterpret_cast<uintptr_t>(block.fn), block.size, pc, /*variant=*/1);
            {
                // Named as a tier-0 block compiled by LLVM is.
                char name[32];
                snprintf(name, sizeof name, "bb_0x%08x%s", pc, careful ? ".careful" : "");
                profile_->recordCodeLoad(name, reinterpret_cast<const void*>(block.fn), block.size);
                // Its cold code, apart in the arena, named as LLVM's split parts are.
                if (block.cold_size) {
                    profile_->recordBlock((uint64_t(1) << 62) | reinterpret_cast<uintptr_t>(block.cold),
                                          reinterpret_cast<uintptr_t>(block.cold), block.cold_size, pc, /*variant=*/1);
                    snprintf(name, sizeof name, "bb_0x%08x%s.cold", pc, careful ? ".careful" : "");
                    profile_->recordCodeLoad(name, block.cold, block.cold_size);
                }
            }
            profile_->stats.translate_ns_total += t1 - t0;
            profile_->stats.blocks_translated++;
            profile_->stats.copy_patch_blocks++;
            if (sink) {
                sink->onCompileStage(pc, "lift", t0, t1, true);
                sink->onCompileStage(pc, "materialize", t1, t1, true);
                if (want_metrics) {
                    BcoreCompileMetrics metrics;
                    metrics.object_bytes = block.size + block.cold_size;
                    metrics.object_hash = cp_->code_hash(tier.counter);
                    metrics.terminator = "copy-patch";
                    metrics.tier = 0;
                    metrics.tier_threshold = block.threshold;
                    metrics.copy_patch = true;
                    sink->onCompileMetrics(pc, metrics);
                }
                sink->onTranslateEnd(pc, t1 - t0);
            }
#endif
            return block.fn;
        }
#if BCORE_ENABLE_PROFILE
        profile_->stats.copy_patch_fallbacks++;
#endif
        prefetched.emplace(*tier.code, cp_->words());
        tier.code = &*prefetched;
    }
#endif
    auto result = translator_->translate(pc, hint, careful, tiered);
    if (replay && replay->missed()) {
        // Code the tier-0 compilation never read: not this block's.
#if BCORE_ENABLE_PROFILE
        fail_on_unwind.armed = false;
        if (sink) sink->onTranslateFailure(pc, bcore_profile::monotonic_ns() - t0);
#endif
        return nullptr;
    }
#if BCORE_ENABLE_PROFILE
    // Stage callbacks fire only after the translation is timed, so
    // nothing the sink does lands inside a stage or in translate_ns.
    const uint64_t lift_end = bcore_profile::monotonic_ns();
    uint64_t optimize_end = 0;
    // Printed between stages and taken out of the elapsed time.
    std::string lifted_ir, optimized_ir;
    uint64_t dump_ns = 0;
    auto print = [&](std::string& out) {
        const uint64_t begin = bcore_profile::monotonic_ns();
        llvm::raw_string_ostream stream(out);
        result.module->print(stream, nullptr);
        stream.flush();
        dump_ns += bcore_profile::monotonic_ns() - begin;
    };
    if (want_ir) print(lifted_ir);
    const uint64_t optimize_begin = bcore_profile::monotonic_ns();
#endif
    // The IR pipeline, once per module and before any dump, so a dump shows
    // the IR that is compiled. It used to run only inside the dump branch:
    // from the initial commit on, a nonzero opt_level changed the printed IR
    // and never the executed code.
    if (opt_level_ != 0 && !tier0) {
        compiler_->optimize(*result.module, promote && light_tier1_);
#if BCORE_ENABLE_PROFILE
        optimize_end = bcore_profile::monotonic_ns();
        if (want_ir) print(optimized_ir);
#endif
    } else {
        lower_guest_memory(*result.module);  // optimize_module's, without the IR pipeline
    }
    if (dump_ir_) {
        llvm::errs() << "=== IR BB @ " << llvm::format_hex(pc, 10) << " ===\n";
        result.module->print(llvm::errs(), nullptr);
        llvm::errs() << "=== end IR ===\n";
    }
#if BCORE_ENABLE_PROFILE
    BcoreCompileMetrics metrics;
    if (want_metrics) {
        // The module as compiled; addModule() takes it. Between stages.
        const uint64_t count_begin = bcore_profile::monotonic_ns();
        for (const llvm::Function& function : *result.module) {
            for (const llvm::BasicBlock& block : function) {
                ++metrics.ir_blocks;
                metrics.ir_instructions += block.size();
            }
        }
        metrics.collect_ns = bcore_profile::monotonic_ns() - count_begin;
        metrics.terminator = result.exit_kind;
        metrics.tier = tiered ? tier.level : -1;
        metrics.tier_threshold = tier0 ? result.tier_threshold : 0;
    }
    profile_->want_object_shapes = want_metrics;
    profile_->capture_ns = 0;
    const uint64_t materialize_begin = bcore_profile::monotonic_ns();
#endif
#if BCORE_ENABLE_PROFILE
    for (auto& f : *result.module) {
        if (f.isDeclaration()) continue;
        if (f.hasFnAttribute(BBTranslator::kLoopBlockAttr)) profile_->stats.loop_blocks++;
        profile_->stats.loops_proven += attribute_count(f, kLoopsProvenAttr);
        profile_->stats.loops_speculated += attribute_count(f, kLoopsSpeculatedAttr);
        profile_->stats.loops_steady += attribute_count(f, kLoopsSteadyAttr);
        profile_->stats.dag_circular += attribute_count(f, BBTranslator::kCircularAttr);
        // A vectorized loop: its latch says so (as does its scalar
        // remainder's) and does vector work.
        for (auto& block : f) {
            auto* loop = block.getTerminator() ? block.getTerminator()->getMetadata(llvm::LLVMContext::MD_loop) : nullptr;
            if (!loop) continue;
            bool vectorized = false, vector = false;
            for (auto& op : loop->operands())
                if (auto* node = llvm::dyn_cast<llvm::MDNode>(op.get()); node && node->getNumOperands() &&
                    llvm::isa<llvm::MDString>(node->getOperand(0)) &&
                    llvm::cast<llvm::MDString>(node->getOperand(0))->getString() == "llvm.loop.isvectorized")
                    vectorized = true;
            for (auto& inst : block) vector |= inst.getType()->isVectorTy();
            if (vectorized && vector) profile_->stats.loops_vectorized++;
        }
    }
#endif
    BbFunc fn = nullptr;
    // The backend, then the object into the JIT; the module and its context
    // are done with after emit.
    std::string name;
    for (const llvm::Function& function : *result.module)
        if (!function.isDeclaration()) name = function.getName().str();
    auto object = (tier0 ? *tier0_compiler_ : *compiler_).emit(*result.module);
    result.module.reset();  // before its context
    result.context.reset();
    if (object && jit_->addObject(pc, std::move(object), result.fallthrough_pc, name))
        fn = jit_->lookup(pc);
    if (fn && record) {
        tier0_current_[pc] = tier.counter;
        tier0_latest_[pc] = tier.counter;
        record->threshold = result.tier_threshold;
        tier0_.push_back(std::move(record));
    } else if (fn && !promote) {
        // Compiled anew without a tier-0 record (tiering since turned off,
        // or out of counters): no promotion queued from earlier code may
        // replace it.
        tier0_current_.erase(pc);
        tier0_latest_.erase(pc);
    }
#if BCORE_ENABLE_PROFILE
    const uint64_t t1 = bcore_profile::monotonic_ns();
    fail_on_unwind.armed = false;
    if (want_metrics) metrics.collect_ns += profile_->capture_ns;
    const uint64_t elapsed = t1 - t0 - metrics.collect_ns - dump_ns;
    profile_->stats.translate_ns_total += elapsed;
    if (fn) profile_->stats.blocks_translated++;
    if (sink) {
        sink->onCompileStage(pc, "lift", t0, lift_end, true);
        if (optimize_end) sink->onCompileStage(pc, "ir-optimize", optimize_begin, optimize_end, true);
        sink->onCompileStage(pc, "materialize", materialize_begin, t1, fn != nullptr);
        if (want_ir) {
            sink->onIrDump(pc, "lifted", lifted_ir);
            if (optimize_end) sink->onIrDump(pc, "optimized", optimized_ir);
        }
        if (fn && want_metrics) {
            auto [bytes, material] = profile_->takeObjectShape(pc);
            metrics.object_bytes = bytes;
            const uint64_t hash_begin = bcore_profile::monotonic_ns();
            metrics.object_hash = bcore_profile::hash_material(material);
            metrics.collect_ns += bcore_profile::monotonic_ns() - hash_begin;
            sink->onCompileMetrics(pc, metrics);
        }
        if (fn) sink->onTranslateEnd(pc, elapsed);
        else sink->onTranslateFailure(pc, elapsed);
    }
#endif
    return fn;
}

bool Core::run(uint32_t pc) {
    if (!pending_.empty()) collect_promotions();
    BbFunc fn = jit_->lookup(pc);
    if (!fn) {
        fn = compile(pc, false);
        if (!fn)
            return false;
    }
#if BCORE_ENABLE_PROFILE
    else {
        profile_->stats.cache_hits++;
    }
#endif

    // Set on every run, not once: the host may reset CpuState
    // (cpu_state_init) between runs of blocks that are already compiled.
    cpu_->page_table = page_table_;
    cpu_->cec_pending = &bcore_cec_pending;
    cpu_->idle_wake = idle_wake_;
    cpu_->chain_table = jit_->chainTable();
    cpu_->chain_packets = 0;
    cpu_->chain_blocks = 0;
    cpu_->chain_would = 0;
    cpu_->idle = 0;
    cpu_->cycles_base = uint64_t(cpu_->cycles[1]) << 32 | cpu_->cycles[0];
    cpu_->did_jump = false;
    cpu_->pc = pc;
    cpu_->packets = 0;
    jit_->set_executing(pc);
#if BCORE_COPY_PATCH
    // A host exception a copy-and-patch block's helper caught, thrown here
    // (cp_translator.h).
    CpTranslator::RunScope scope(cpu_);
#endif
    jit_->enter(fn, cpu_, mem_);
#if BCORE_COPY_PATCH
    scope.rethrow();
#endif
    jit_->clear_executing();
    while (cpu_->lb_guard_miss) {
        // A block met a loop bottom where its compilation expected none
        // (BBTranslator's lb_guard) and returned before its first packet, at
        // its own PC, with the run's chain state as it found it: from now on
        // that PC runs a careful compilation, which takes its place in this
        // run too. A careful block has no guard, but it may chain into
        // another block whose guard misses: that one returns here the same
        // way, so the run goes on until no block misses. Each PC misses at
        // most once (its guarded compilation is replaced).
        cpu_->lb_guard_miss = 0;
        // No packet of the block retired, and a chained block leaves the
        // index of its own last packet behind (BBTranslator's chain exit):
        // the host's callbacks during the compilation (the code fetches)
        // read the run's packets as chain_packets plus this.
        cpu_->packets = 0;
#if BCORE_ENABLE_PROFILE
        profile_->stats.lb_guard_misses++;
#endif
        const uint32_t at = cpu_->pc;
        BbFunc careful = compile(at, true);
        if (!careful)
            return false;
        jit_->set_executing(at);
        jit_->enter(careful, cpu_, mem_);
#if BCORE_COPY_PATCH
        scope.rethrow();
#endif
        jit_->clear_executing();
    }
#if BCORE_ENABLE_PROFILE
    profile_->stats.blocks_executed += 1 + cpu_->chain_blocks;
#endif
    // The run's packets: those of its chained blocks and the last block's.
    packets_run_ += uint64_t(cpu_->chain_packets) + cpu_->packets;
    if (tier_threshold_ && jit_->tierTable()->requests) promote();
    return true;
}

void Core::promote() {
    TierTable& table = *jit_->tierTable();
    const uint32_t filed = table.requests;
    table.requests = 0;
    std::vector<uint32_t> counters;
    if (filed <= TierTable::kRequests) {
        counters.assign(table.request, table.request + filed);
    } else {
        // More than the table holds: every count at its block's threshold
        // (tier_threshold) or past it (those promoted or replaced already
        // are passed over below).
#if BCORE_ENABLE_PROFILE
        profile_->stats.tier_request_overflows++;
#endif
        for (uint32_t counter = 0; counter < tier0_.size(); ++counter)
            if (tier0_[counter] && table.count[counter] >= tier0_[counter]->threshold) counters.push_back(counter);
    }
    for (uint32_t counter : counters) {
        if (counter >= tier0_.size() || !tier0_[counter]) continue;
        std::unique_ptr<Tier0Record> record = std::move(tier0_[counter]);
        // Its code may have been replaced since (a careful compilation after
        // a loop-bottom guard miss): that one counts for itself.
        auto current = tier0_current_.find(record->pc);
        if (current == tier0_current_.end() || current->second != counter) continue;
        tier0_current_.erase(current);
        if (worker_) {
            queue_promotion(*record, counter);
            continue;
        }
        // Failing, the tier-0 block keeps running; its count goes on, unread.
        const bool promoted = compile(record->pc, record->careful, record.get()) != nullptr;
#if BCORE_ENABLE_PROFILE
        if (promoted) profile_->stats.blocks_promoted++;
        else profile_->stats.promotions_dropped++;
#else
        (void)promoted;
#endif
    }
}

void Core::queue_promotion(Tier0Record& record, uint32_t counter) {
    const uint32_t pc = record.pc;
    note_page_table();  // its code may inline through it, as compile()'s
    bcore_tier::ReplayMemory replay(*mem_, record.code);
    BBTranslator::Tier tier;
    tier.level = 1;
    tier.code = &replay;
#if BCORE_ENABLE_PROFILE
    BcoreEventSink* sink = profile_->sink;
    const bool want_metrics = sink && sink->wantsCompileMetrics();
    if (sink) sink->onTranslateBegin(pc);
    const uint64_t t0 = bcore_profile::monotonic_ns();
    struct FailOnUnwind {
        BcoreEventSink* sink;
        uint32_t pc;
        uint64_t t0;
        bool armed = true;
        ~FailOnUnwind() {
            if (armed && sink) sink->onTranslateFailure(pc, bcore_profile::monotonic_ns() - t0);
        }
    } fail_on_unwind{sink, pc, t0};
#endif
    auto result = translator_->translate(pc, &record.hint, record.careful, &tier);
    const bool ok = !replay.missed() && result.module;
    CompileWorker::Job job;
    if (ok) {
        job.pc = pc;
        job.counter = counter;
        job.generation = generation_;
        job.fallthrough_pc = result.fallthrough_pc;
        job.exit_kind = result.exit_kind;
        job.light = light_tier1_;
        for (const llvm::Function& function : *result.module)
            if (!function.isDeclaration()) job.name = function.getName().str();
    }
#if BCORE_ENABLE_PROFILE
    const uint64_t t1 = bcore_profile::monotonic_ns();
    fail_on_unwind.armed = false;
    BcoreCompileMetrics metrics;
    if (ok && want_metrics) {
        const ModuleCounts counts = count_module(*result.module);
        metrics.ir_instructions = counts.ir_instructions;
        metrics.ir_blocks = counts.ir_blocks;
        metrics.collect_ns = bcore_profile::monotonic_ns() - t1;
        metrics.terminator = result.exit_kind;
        metrics.tier = 1;
        metrics.background = 1;
    }
    if (!ok) profile_->stats.promotions_dropped++;
    profile_->stats.translate_ns_total += t1 - t0;
    if (sink) {
        sink->onCompileStage(pc, "lift", t0, t1, ok);
        if (ok && want_metrics) sink->onCompileMetrics(pc, metrics);
        if (ok) sink->onTranslateEnd(pc, t1 - t0);
        else sink->onTranslateFailure(pc, t1 - t0);
    }
#endif
    if (!ok) return;
    job.context = std::move(result.context);
    job.module = std::move(result.module);
    pending_.push_back({pc, counter, packets_run_ + publish_after_});
    worker_->submit(std::move(job));
}

void Core::collect_promotions(bool all) {
    while (!pending_.empty()) {
        const bool deterministic = all || publish_after_ != 0;
        if (!all && deterministic && packets_run_ < pending_.front().due) return;
        CompileWorker::Result result;
#if BCORE_ENABLE_PROFILE
        const uint64_t wait_begin = bcore_profile::monotonic_ns();
#endif
        if (!worker_->take(result, deterministic)) return;
#if BCORE_ENABLE_PROFILE
        if (deterministic) profile_->stats.tier_wait_ns += bcore_profile::monotonic_ns() - wait_begin;
#endif
        pending_.pop_front();
        const uint32_t pc = result.job.pc;
        // Stale: the engine was replaced (invalidate takes those), or the
        // block compiled again at tier 0 since (careful, after a guard miss;
        // that one promotes itself).
        auto latest = tier0_latest_.find(pc);
        const bool use = result.object && result.job.generation == generation_ && latest != tier0_latest_.end() &&
                         latest->second == result.job.counter;
#if BCORE_ENABLE_PROFILE
        BcoreEventSink* sink = profile_->sink;
        if (sink) {
            BcoreBackgroundCompile work;
            work.begin_ns = result.begin_ns;
            work.end_ns = result.end_ns;
            work.cpu_ns = result.cpu_ns;
            work.ok = result.object != nullptr;
            work.discarded = !use;
            sink->onBackgroundCompile(pc, work);
        }
        if (!use) profile_->stats.promotions_discarded++;
#endif
        if (!use) continue;
#if BCORE_ENABLE_PROFILE
        const bool want_metrics = sink && sink->wantsCompileMetrics();
        if (sink) sink->onTranslateBegin(pc);
        const uint64_t t0 = bcore_profile::monotonic_ns();
        profile_->want_object_shapes = want_metrics;
        profile_->capture_ns = 0;
#endif
        BbFunc fn = nullptr;
        if (jit_->addObject(pc, std::move(result.object), result.job.fallthrough_pc, result.job.name))
            fn = jit_->lookup(pc);
#if BCORE_ENABLE_PROFILE
        const uint64_t t1 = bcore_profile::monotonic_ns();
        auto& stats = profile_->stats;
        stats.loop_blocks += result.loop_blocks;
        stats.loops_proven += result.loops_proven;
        stats.loops_speculated += result.loops_speculated;
        stats.loops_vectorized += result.loops_vectorized;
        stats.loops_steady += result.loops_steady;
        stats.dag_circular += result.dag_circular;
        BcoreCompileMetrics metrics;
        metrics.collect_ns = profile_->capture_ns;
        const uint64_t elapsed = t1 - t0 - metrics.collect_ns;
        stats.translate_ns_total += elapsed;
        if (fn) {
            stats.blocks_translated++;
            stats.blocks_promoted++;
        } else {
            stats.promotions_dropped++;
        }
        if (sink) {
            sink->onCompileStage(pc, "materialize", t0, t1, fn != nullptr);
            if (fn && want_metrics) {
                metrics.ir_instructions = result.ir_instructions;
                metrics.ir_blocks = result.ir_blocks;
                metrics.terminator = result.job.exit_kind;
                metrics.tier = 1;
                metrics.background = 2;
                auto [bytes, material] = profile_->takeObjectShape(pc);
                metrics.object_bytes = bytes;
                const uint64_t hash_begin = bcore_profile::monotonic_ns();
                metrics.object_hash = bcore_profile::hash_material(material);
                metrics.collect_ns += bcore_profile::monotonic_ns() - hash_begin;
                sink->onCompileMetrics(pc, metrics);
            }
            if (fn) sink->onTranslateEnd(pc, elapsed);
            else sink->onTranslateFailure(pc, elapsed);
        }
#else
        (void)fn;
#endif
    }
}

void Core::set_copy_patch(bool enable) {
    copy_patch_ = enable;
#if BCORE_COPY_PATCH
    // One translator per engine (init, invalidate): turning stencils off
    // keeps it, and the blocks already made of them run on.
    if (enable && jit_ && !cp_) {
        cp_ = std::make_unique<CpTranslator>(*jit_);
#if BCORE_ENABLE_PROFILE
        cp_->set_code_listener([this](const char* name, const void* code, size_t size) {
            profile_->recordCodeLoad(name, code, size);
        });
#endif
    }
#endif
}

void Core::invalidate() {
    // The worker's promotions were lifted for the old engine: their code is
    // not used (and the worker stops before the backend of one in flight).
    if (worker_) {
        worker_->set_generation(++generation_);
        collect_promotions(/*all=*/true);
    }
    jit_ = std::make_shared<JitEngine>();
    // Init cannot fail here unless it failed in Core::init already (same host,
    // same levels); if it ever does, stop rather than dereference a null LLJIT.
    if (!jit_->init())
        llvm::report_fatal_error("bcore: JIT re-initialization failed in invalidate()");
    jit_->setChainFilter(chain_filter_);
    applyModuleTarget();
    cp_.reset();  // the old engine's arenas are gone
    set_copy_patch(copy_patch_);
    // The new engine's tier table starts at zero, with none of these blocks.
    tier0_.clear();
    tier0_current_.clear();
    tier0_latest_.clear();
#if BCORE_ENABLE_PROFILE
    // The old engine's objects are gone; drop their map entries before the
    // host can observe stale addresses, then re-arm the listeners (including
    // jitdump) on the new engine: one registered on the old engine silently
    // misses everything compiled after this point.
    profile_->clearBlocks();
    attachProfiler();
#endif
}

std::tuple<std::string, uint32_t> Core::disassemble(uint32_t pc) {
    DisasmVisitor visitor;
    visitor.mem = mem_;

    std::ostringstream oss;
    visitor.set_out(&oss);

    int insn_len = decodeInstruction(visitor, pc);

    std::string text = oss.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    uint32_t next_pc = pc + static_cast<uint32_t>(insn_len);
    return {text, next_pc};
}

void Core::set_chain_filter(std::function<bool(uint32_t)> filter) {
    chain_filter_ = std::move(filter);
    if (translator_) translator_->set_chain_filter(chain_filter_);
    if (!jit_) return;
    jit_->setChainFilter(chain_filter_);
    // A compiled block's native loop holds the answer the old filter gave
    // for its PC when it was translated (BBTranslator::set_chain_filter).
    if (!jit_->empty()) invalidate();
}

void Core::set_chaining(bool chaining, bool dry) {
    chaining_ = chaining;
    chain_dry_ = dry;
    if (translator_) translator_->set_chaining(chaining_, chain_dry_);
}

void Core::set_count_chained(bool count) {
    count_chained_ = count;
    if (translator_) translator_->set_count_chained(count_chained_);
}

void Core::set_host_cycles(bool host_cycles) {
    host_cycles_ = host_cycles;
    if (translator_) translator_->set_host_cycles(host_cycles_);
}

// The default IR pipeline at O1 has no dead store elimination: a flag or
// register the next instruction overwrites stays stored. Measured on the boot
// to op1emu's main display: -0.25% host instructions in generated code with
// DSE; adding early-cse<memssa> and gvn bought 0.06% more for 13% more
// compile time, type-based alias tags on CpuState and guest RAM nothing.
const char* const Core::kDefaultPostPipeline = "function(dse)";

bool Core::set_post_pipeline(const std::string& pipeline) {
    if (!compiler_ || !compiler_->set_post_pipeline(pipeline)) return false;
    post_pipeline_ = pipeline;
    if (worker_) {
        stopWorker();  // its compiler has the old one
        startWorker();
    }
    return true;
}

void Core::set_lazy_state(bool lazy) {
    lazy_state_ = lazy;
    if (translator_) translator_->set_lazy_state(lazy_state_);
}

void Core::set_loop_blocks(bool loop) {
    loop_blocks_ = loop;
    if (translator_) translator_->set_loop_blocks(loop_blocks_);
}

void Core::set_tiering(uint32_t threshold, uint32_t size_ref) {
    if (threshold && !tier0_compiler_) {
        auto compiler = std::make_unique<Compiler>();
        // Neither the IR pipeline nor the splitter: tier-0 code runs too
        // rarely for its layout to matter.
        if (!compiler->init(/*opt_level=*/0, /*codegen_level=*/0, /*split=*/false)) return;
        tier0_compiler_ = std::move(compiler);
    }
    tier_threshold_ = threshold;
    tier_size_ref_ = size_ref;
}

void Core::set_tier_background(bool background, uint64_t publish_after, std::function<void()> on_start) {
    stopWorker();
    tier_background_ = background;
    publish_after_ = publish_after;
    worker_start_ = std::move(on_start);
    if (tier_background_ && compiler_) startWorker();
}

// Publishes what is underway, then stops the worker.
void Core::stopWorker() {
    if (!worker_) return;
    collect_promotions(/*all=*/true);
    worker_.reset();
}

void Core::startWorker() {
    auto compiler = std::make_unique<Compiler>();
    if (!compiler->init(opt_level_, codegen_level_) || !compiler->set_post_pipeline(post_pipeline_)) return;
    compiler->set_steady_skip(steady_skip_);
    worker_ = std::make_unique<CompileWorker>(std::move(compiler), worker_start_);
    worker_->set_generation(generation_);
}

void Core::set_verify(bool verify) {
    verify_ = verify;
    if (translator_) translator_->set_verify(verify_);
}

void Core::set_steady_skip(int mode) {
    steady_skip_ = mode;
    if (compiler_) compiler_->set_steady_skip(steady_skip_);
    if (worker_) {
        stopWorker();
        startWorker();
    }
    if (translator_) translator_->set_steady_skip(steady_skip_ != 0);
}

void Core::set_idle_waits(bool waits, const uint8_t* wake) {
    idle_waits_ = waits;
    idle_wake_ = wake;
    if (translator_) translator_->set_idle_waits(idle_waits_);
}

void Core::set_dump_ir(bool enable) {
    dump_ir_ = enable;
}


// ---------------------------------------------------------------------------
// Observability API
// ---------------------------------------------------------------------------

void Core::forEachCompiledBlock(
    const std::function<void(const BcoreBlockInfo&)>& fn) const {
#if BCORE_ENABLE_PROFILE
    if (profile_) {
        std::lock_guard<std::mutex> lk(profile_->blocks_mutex);
        for (const auto& [addr, b] : profile_->blocks) {
            fn(BcoreBlockInfo{reinterpret_cast<const void*>(addr), b.size,
                              b.guest_pc, b.variant, b.loaded_ns, 0});
        }
        if (profile_->listener_ok) return;
    }
#endif
    // Fallback (profiling disabled at build time or listener unavailable):
    // lookup-derived addresses, size unknown.
    if (jit_)
        jit_->forEachCompiled([&fn](const void* addr, uint32_t pc) {
            fn(BcoreBlockInfo{addr, /*host_size=*/0, pc, /*variant=*/0});
        });
}

void Core::setEventSink(BcoreEventSink* sink) {
#if BCORE_ENABLE_PROFILE
    profile_->sink = sink;
#else
    (void)sink;
#endif
}

BcoreStats Core::stats() const {
#if BCORE_ENABLE_PROFILE
    if (profile_) {
        BcoreStats stats = profile_->stats;  // plain reads; dispatch is single-threaded
        cec_entry_counts(stats.interrupts_entered);
        return stats;
    }
#endif
    return {};
}

bool Core::profileHasBlockSizes() const {
#if BCORE_ENABLE_PROFILE
    return profile_ && profile_->listener_ok;
#else
    return false;
#endif
}

void Core::set_perf_jitdump(bool enable) {
#if BCORE_ENABLE_PROFILE
    profile_->want_perf_jitdump = enable;
    if (!jit_) return;
    if (enable)
        bcore_profile::attach_perf_listener(*profile_, *jit_);
    else
        bcore_profile::detach_perf_listener(*profile_, *jit_);
#else
    (void)enable;
#endif
}

void Core::setProfileHistory(size_t capacity) {
#if BCORE_ENABLE_PROFILE
    std::lock_guard<std::mutex> lk(profile_->blocks_mutex);
    profile_->history_capacity = capacity;
    profile_->retired.clear();
    profile_->retired.reserve(capacity);
    profile_->history_dropped = 0;
    const auto now = capacity ? bcore_profile::monotonic_ns() : 0;
    for (auto& [addr, b] : profile_->blocks) b.loaded_ns = now;
#else
    (void)capacity;
#endif
}

void Core::forEachProfileBlock(const std::function<void(const BcoreBlockInfo&)>& fn) const {
#if BCORE_ENABLE_PROFILE
    std::lock_guard<std::mutex> lk(profile_->blocks_mutex);
    for (const auto& b : profile_->retired) fn(b);
    for (const auto& [addr, b] : profile_->blocks)
        fn({reinterpret_cast<const void*>(addr), b.size, b.guest_pc, b.variant, b.loaded_ns, 0});
    if (profile_->listener_ok) return;
#endif
    // No load listener (or profiling compiled out): live code from the
    // lookup table, sizes and lifetimes unknown, as in forEachCompiledBlock.
    if (jit_)
        jit_->forEachCompiled([&fn](const void* addr, uint32_t pc) {
            fn(BcoreBlockInfo{addr, /*host_size=*/0, pc, /*variant=*/0});
        });
}

uint64_t Core::profileHistoryDropped() const {
#if BCORE_ENABLE_PROFILE
    std::lock_guard<std::mutex> lk(profile_->blocks_mutex);
    return profile_->history_dropped;
#else
    return 0;
#endif
}
