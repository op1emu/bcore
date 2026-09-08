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
        const auto t0 = std::chrono::steady_clock::now();
#endif
        auto result = translator_->translate(pc);
        if (dump_ir_) {
            jit_->optimize_module(*result.module);
            llvm::errs() << "=== IR BB @ " << llvm::format_hex(pc, 10) << " ===\n";
            result.module->print(llvm::errs(), nullptr);
            llvm::errs() << "=== end IR ===\n";
        }
        if (!jit_->addModule(pc,
                             std::move(result.module),
                             std::move(result.context),
                             result.fallthrough_pc))
            return false;
        fn = jit_->lookup(pc);
        if (!fn)
            return false;
#if BCORE_ENABLE_PROFILE
        profile_->stats.blocks_translated++;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        profile_->stats.translate_ns_total += ns;
        if (sink)
            sink->onTranslateEnd(pc, ns);
#endif
    }
#if BCORE_ENABLE_PROFILE
    else {
        profile_->stats.cache_hits++;
        if (sink)
            sink->onCacheHit(pc);
    }
#endif

    cpu_->did_jump = false;
    cpu_->pc = pc;
    jit_->set_executing(pc);
#if BCORE_ENABLE_PROFILE
    // Block cost probe: burn the armed per-execution cost on this thread
    // before dispatching. Unarmed (probe_pc_ == 0) this is one predictable
    // load+branch.
    if (const uint32_t probed = probe_pc_.load(std::memory_order_relaxed)) {
        if (probed == pc)
            bcore_profile::spin_ns(probe_nanos_.load(std::memory_order_relaxed));
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
                              b.guest_pc, b.variant});
        }
        return;
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
    if (guest_pc == 0 || nanos == 0) {
        clearBlockCostProbe();
        return;
    }
    (void)bcore_profile::tsc_hz();  // calibrate before arming, not on first hit
    probe_nanos_.store(nanos, std::memory_order_relaxed);
    probe_pc_.store(guest_pc, std::memory_order_release);
#else
    (void)guest_pc;
    (void)nanos;
#endif
}

void Core::clearBlockCostProbe() {
#if BCORE_ENABLE_PROFILE
    probe_pc_.store(0, std::memory_order_release);
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
