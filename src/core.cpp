#include "core.h"

#include <sstream>

#include "bb_translator.h"
#include "cec.h"
#include "decoder.h"
#include "disasm_visitor.h"
#include "jit_engine.h"
#include "mmr.h"
#include "profile_state.h"

#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

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

    jit_ = std::make_shared<JitEngine>();
    if (!jit_->init(opt_level_, codegen_level_) || !jit_->set_post_pipeline(post_pipeline_))
        return false;
    jit_->setChainFilter(chain_filter_);
    attachProfiler();

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
    applyModuleTarget();

    return true;
}

void Core::applyModuleTarget() {
    // Same source as the compiler: LLJIT's addIRModule rejects a module whose
    // non-default layout differs from its own.
    translator_->setModuleTarget(jit_->dataLayout(), jit_->targetTriple());
}

BbFunc Core::compile(uint32_t pc, bool careful) {
    // The translation below inlines accesses through this table if there
    // is one; the code finds it in CpuState (see Memory::page_table()).
    if (uint8_t* const* table = mem_->page_table())
        page_table_ = table;
#if BCORE_ENABLE_PROFILE
    BcoreEventSink* sink = profile_->sink;
    profile_->stats.cache_misses++;
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
    auto result = translator_->translate(pc, cpu_, careful);
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
    if (opt_level_ != 0) {
        jit_->optimize_module(*result.module);
#if BCORE_ENABLE_PROFILE
        optimize_end = bcore_profile::monotonic_ns();
        if (want_ir) print(optimized_ir);
#endif
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
    }
    profile_->want_object_shapes = want_metrics;
    profile_->capture_ns = 0;
    const uint64_t materialize_begin = bcore_profile::monotonic_ns();
#endif
#if BCORE_ENABLE_PROFILE
    for (auto& f : *result.module)
        if (!f.isDeclaration() && f.hasFnAttribute(BBTranslator::kLoopBlockAttr)) profile_->stats.loop_blocks++;
#endif
    BbFunc fn = nullptr;
    if (jit_->addModule(pc,
                        std::move(result.module),
                        std::move(result.context),
                        result.fallthrough_pc, careful))
        fn = jit_->lookup(pc);
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
    cpu_->chain_table = jit_->chainTable();
    cpu_->chain_packets = 0;
    cpu_->chain_blocks = 0;
    cpu_->chain_would = 0;
    cpu_->cycles_base = uint64_t(cpu_->cycles[1]) << 32 | cpu_->cycles[0];
    cpu_->did_jump = false;
    cpu_->pc = pc;
    cpu_->packets = 0;
    jit_->set_executing(pc);
    jit_->enter(fn, cpu_, mem_);
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
        jit_->clear_executing();
    }
#if BCORE_ENABLE_PROFILE
    profile_->stats.blocks_executed += 1 + cpu_->chain_blocks;
#endif
    return true;
}

void Core::invalidate() {
    jit_ = std::make_shared<JitEngine>();
    // Init cannot fail here unless it failed in Core::init already (same host,
    // same levels); if it ever does, stop rather than dereference a null LLJIT.
    if (!jit_->init(opt_level_, codegen_level_) || !jit_->set_post_pipeline(post_pipeline_))
        llvm::report_fatal_error("bcore: JIT re-initialization failed in invalidate()");
    jit_->setChainFilter(chain_filter_);
    applyModuleTarget();
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
    if (!jit_ || !jit_->set_post_pipeline(pipeline)) return false;
    post_pipeline_ = pipeline;
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
