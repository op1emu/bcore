#pragma once

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

    // opt_level: the IR pass pipeline, 0=None, 1=O1, 2=O2, 3=O3.
    // codegen_level: the backend on the same scale (0=None, 1=Less, 2=Default,
    // 3=Aggressive); it follows opt_level when negative.
    bool init(int opt_level = 2, int codegen_level = -1);
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
    // (Re)registers the code-map listener on jit_. Called by init/invalidate.
    void attachProfiler();

    CpuState* cpu_;
    Memory* mem_;
    // Declared before jit_ on purpose: a JitEngine being destroyed frees its
    // objects through the registered listener, which writes into profile_ --
    // so profile_ must outlive any engine.
    std::unique_ptr<bcore_profile::ProfileState> profile_;
    std::shared_ptr<JitEngine> jit_;
    std::shared_ptr<BBTranslator> translator_;
    int opt_level_ = 2;
    int codegen_level_ = -1;
    bool dump_ir_ = false;
};
