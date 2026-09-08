#include "core.h"

#include <chrono>
#include <sstream>

#include "bb_translator.h"
#include "decoder.h"
#include "disasm_visitor.h"
#include "jit_engine.h"
#include "profile_state.h"

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

bool Core::init(int opt_level) {
    opt_level_ = opt_level;

    jit_ = std::make_shared<JitEngine>();
    if (!jit_->init(opt_level_))
        return false;
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

    return true;
}

bool Core::run(uint32_t pc) {
#if BCORE_ENABLE_PROFILE
    BcoreEventSink* sink = profile_->sink;
#endif
    BbFunc fn = jit_->lookup(pc);
    if (!fn) {
#if BCORE_ENABLE_PROFILE
        profile_->stats.cache_misses++;
        if (sink) {
            sink->onCacheMiss(pc);
            sink->onTranslateBegin(pc);
        }
        const auto t0 = bcore_profile::monotonic_ns();
#endif
        auto result = translator_->translate(pc);
        if (dump_ir_) {
            jit_->optimize_module(*result.module);
            llvm::errs() << "=== IR BB @ " << llvm::format_hex(pc, 10) << " ===\n";
            result.module->print(llvm::errs(), nullptr);
            llvm::errs() << "=== end IR ===\n";
        }
#if BCORE_ENABLE_PROFILE
        const auto lift_end = bcore_profile::monotonic_ns();
        if (sink) sink->onCompileStage(pc, "lift", t0, lift_end, true);
        const auto native_begin = bcore_profile::monotonic_ns();
#endif
        const bool added = jit_->addModule(pc,
                             std::move(result.module),
                             std::move(result.context),
                             result.fallthrough_pc);
        fn = added ? jit_->lookup(pc) : nullptr;
#if BCORE_ENABLE_PROFILE
        const auto native_end = bcore_profile::monotonic_ns();
        const uint64_t ns = native_end - t0;
        profile_->stats.translate_ns_total += ns;
        if (sink) sink->onCompileStage(pc, "materialize", native_begin, native_end, fn != nullptr);
        if (fn) {
            profile_->stats.blocks_translated++;
            if (sink) sink->onTranslateEnd(pc, ns);
        } else if (sink) sink->onTranslateFailure(pc, ns);
#endif
        if (!fn) return false;
    }
#if BCORE_ENABLE_PROFILE
    else {
        profile_->stats.cache_hits++;
        if (sink && profile_->sink_wants_cache_hits)
            sink->onCacheHit(pc);
    }
#endif

    cpu_->did_jump = false;
    cpu_->pc = pc;
    jit_->set_executing(pc);
#if BCORE_ENABLE_PROFILE
    if (probe_enabled_ && probe_.guest_pc == pc) {
        ++probe_.executions;
        probe_.elapsed_ns += bcore_profile::spin_ns(probe_.requested_ns);
    }
#endif
    fn(cpu_, mem_);
    jit_->clear_executing();
#if BCORE_ENABLE_PROFILE
    profile_->stats.blocks_executed++;
    if (sink && profile_->sink_wants_dispatch)
        sink->onDispatch(pc);
#endif
    return true;
}

void Core::invalidate() {
    jit_ = std::make_shared<JitEngine>();
    jit_->init(opt_level_);
#if BCORE_ENABLE_PROFILE
    // The old engine's objects are gone; drop their map entries before the
    // host can observe stale addresses, then re-arm listeners on the new
    // engine.
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
    profile_->sink_wants_dispatch = sink && sink->wantsDispatch();
    profile_->sink_wants_cache_hits = sink && sink->wantsCacheHits();
#else
    (void)sink;
#endif
}

BcoreStats Core::stats() const {
#if BCORE_ENABLE_PROFILE
    if (profile_)
        return profile_->stats;  // plain reads; dispatch is single-threaded
#endif
    return {};
}

void Core::setBlockCostProbe(uint32_t guest_pc, uint64_t nanos) {
#if BCORE_ENABLE_PROFILE
    probe_ = {guest_pc, nanos, 0, 0};
    probe_enabled_ = true;
#else
    (void)guest_pc;
    (void)nanos;
#endif
}

void Core::clearBlockCostProbe() {
#if BCORE_ENABLE_PROFILE
    probe_enabled_ = false;
#endif
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
    if (enable && jit_)
        bcore_profile::attach_perf_listener(*profile_, *jit_);
#else
    (void)enable;
#endif
}

BcoreProbeStats Core::blockCostProbeStats() const {
#if BCORE_ENABLE_PROFILE
    return probe_;
#else
    return {};
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
#else
    (void)fn;
#endif
}
uint64_t Core::profileHistoryDropped() const {
#if BCORE_ENABLE_PROFILE
    std::lock_guard<std::mutex> lk(profile_->blocks_mutex);
    return profile_->history_dropped;
#else
    return 0;
#endif
}
