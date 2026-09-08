#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>

#include "bcore_profile.h"
#include "cpu_state.h"
#include "mem.h"

class JitEngine;
class BBTranslator;

namespace bcore_profile { struct ProfileState; }

class Core {
public:
    Core(CpuState* cpu, Memory* mem);
    ~Core();

    // opt_level: 0=None, 1=Less, 2=Default, 3=Aggressive
    bool init(int opt_level = 2);
    void invalidate();
    bool run(uint32_t pc);
    std::tuple<std::string, uint32_t> disassemble(uint32_t pc);
    void set_dump_ir(bool enable);

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

    // Arm a calibrated cost probe: each dispatch of `guest_pc` burns an
    // extra `nanos` nanoseconds of CPU time (TSC busy-wait — never sleeps,
    // so guest-visible state and scheduling are untouched). The cost is
    // charged per execution, which is exactly the unit causal profiling
    // needs. nanos == 0 disarms. guest_pc must be != 0.
    void setBlockCostProbe(uint32_t guest_pc, uint64_t nanos);
    void clearBlockCostProbe();

    // False when the object linking layer doesn't support JIT event
    // listeners; the code map then reports host_size == 0 (lookup-derived
    // addresses only).
    bool profileHasBlockSizes() const;

    // Runtime opt-in for jitdump emission (`perf inject --jit`). Attaches
    // immediately (safe to call any time after init(), including from a
    // running boot) and persists across invalidate(). Only has an effect in
    // builds with BCORE_PERF_JIT_EVENTS=ON.
    void set_perf_jitdump(bool enable);

private:
    // (Re)registers the code-map listener on jit_. Called by init/invalidate.
    void attachProfiler();

    CpuState* cpu_;
    Memory* mem_;
    // Declared before jit_ on purpose: a JitEngine being destroyed frees its
    // objects through the registered listener, which writes into profile_ —
    // so profile_ must outlive any engine.
    std::unique_ptr<bcore_profile::ProfileState> profile_;
    std::shared_ptr<JitEngine> jit_;
    std::shared_ptr<BBTranslator> translator_;
    int opt_level_ = 2;
    bool dump_ir_ = false;

    // Block cost probe dispatch state. probe_pc_ == 0 means unarmed, so the
    // dispatch loop pays one predictable load+branch while it stays 0.
    std::atomic<uint32_t> probe_pc_{0};
    std::atomic<uint64_t> probe_nanos_{0};
};
