#include "core.h"

#include <sstream>

#include "bb_translator.h"
#include "decoder.h"
#include "disasm_visitor.h"
#include "jit_engine.h"
#include "profile_state.h"

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
    if (!jit_->init(opt_level_, codegen_level_))
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
    if (cpu_->steps_remaining)
        translator_->set_max_packets(cpu_->steps_remaining);
    applyModuleTarget();

    return true;
}

void Core::applyModuleTarget() {
    // Same source as the compiler: LLJIT's addIRModule rejects a module whose
    // non-default layout differs from its own.
    translator_->setModuleTarget(jit_->dataLayout(), jit_->targetTriple());
}

bool Core::run(uint32_t pc) {
    BbFunc fn = jit_->lookup(pc);
    if (!fn) {
#if BCORE_ENABLE_PROFILE
        BcoreEventSink* sink = profile_->sink;
        profile_->stats.cache_misses++;
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
        auto result = translator_->translate(pc);
#if BCORE_ENABLE_PROFILE
        // Stage callbacks fire only after the translation is timed, so
        // nothing the sink does lands inside a stage or in translate_ns.
        const uint64_t lift_end = bcore_profile::monotonic_ns();
        uint64_t optimize_end = 0;
#endif
        // The IR pipeline, once per module and before any dump, so a dump shows
        // the IR that is compiled. It used to run only inside the dump branch:
        // from the initial commit on, a nonzero opt_level changed the printed IR
        // and never the executed code.
        if (opt_level_ != 0) {
            jit_->optimize_module(*result.module);
#if BCORE_ENABLE_PROFILE
            optimize_end = bcore_profile::monotonic_ns();
#endif
        }
        if (dump_ir_) {
            llvm::errs() << "=== IR BB @ " << llvm::format_hex(pc, 10) << " ===\n";
            result.module->print(llvm::errs(), nullptr);
            llvm::errs() << "=== end IR ===\n";
        }
#if BCORE_ENABLE_PROFILE
        const uint64_t materialize_begin = bcore_profile::monotonic_ns();
#endif
        if (jit_->addModule(pc,
                            std::move(result.module),
                            std::move(result.context),
                            result.fallthrough_pc))
            fn = jit_->lookup(pc);
#if BCORE_ENABLE_PROFILE
        const uint64_t t1 = bcore_profile::monotonic_ns();
        fail_on_unwind.armed = false;
        profile_->stats.translate_ns_total += t1 - t0;
        if (fn) profile_->stats.blocks_translated++;
        if (sink) {
            sink->onCompileStage(pc, "lift", t0, lift_end, true);
            if (optimize_end) sink->onCompileStage(pc, "ir-optimize", lift_end, optimize_end, true);
            sink->onCompileStage(pc, "materialize", materialize_begin, t1, fn != nullptr);
            if (fn) sink->onTranslateEnd(pc, t1 - t0);
            else sink->onTranslateFailure(pc, t1 - t0);
        }
#endif
        if (!fn)
            return false;
    }
#if BCORE_ENABLE_PROFILE
    else {
        profile_->stats.cache_hits++;
    }
#endif

    cpu_->did_jump = false;
    cpu_->pc = pc;
    cpu_->packets = 0;
    jit_->set_executing(pc);
    fn(cpu_, mem_);
    jit_->clear_executing();
#if BCORE_ENABLE_PROFILE
    profile_->stats.blocks_executed++;
#endif
    return true;
}

void Core::invalidate() {
    jit_ = std::make_shared<JitEngine>();
    // Init cannot fail here unless it failed in Core::init already (same host,
    // same levels); if it ever does, stop rather than dereference a null LLJIT.
    if (!jit_->init(opt_level_, codegen_level_))
        llvm::report_fatal_error("bcore: JIT re-initialization failed in invalidate()");
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
    if (profile_)
        return profile_->stats;  // plain reads; dispatch is single-threaded
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
