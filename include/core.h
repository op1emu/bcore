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
    // PCs a chained block may run next, itself included (see
    // CpuState::chain_budget); the host runs every block starting at a PC
    // the filter rejects. Setting it empties the chain table, so it applies
    // to every block from now on; it persists across invalidate(). The
    // filter is called on the run() thread only, when a block is compiled or
    // looked up: its answer for a PC may change from false to true (the
    // block is published at its next lookup), but to reject a PC it accepted
    // before, set the filter again.
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
    // Blocks compiled from now on store pc, insn_len and packets only where
    // a helper call, a return or a load can read them (on by default; off
    // stores them at every packet, as before). Either way every reader sees
    // the same values.
    void set_lazy_state(bool lazy);

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
    // Translates, optimizes and adds the block at pc (careful: see
    // BBTranslator::translate); null if that fails.
    void (*compile(uint32_t pc, bool careful))(CpuState*, Memory*);
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
    std::shared_ptr<BBTranslator> translator_;
    int opt_level_ = 2;
    int codegen_level_ = -1;
    bool dump_ir_ = false;
    std::function<bool(uint32_t)> chain_filter_;
    bool chaining_ = false;
    bool chain_dry_ = false;
    bool host_cycles_ = false;
    bool lazy_state_ = true;
};
