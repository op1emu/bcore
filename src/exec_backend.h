#pragma once

// What Core runs blocks on: an engine that makes a block's object file
// (Compiler::emit) into a function it can call, and beside the functions
// the tables generated code reads, the same for every engine: the chain
// table and the tier table after it (CpuState::chain_table, TierTable).
// The native engine (JitEngine, jit_engine.h) links objects into this
// process; another loads them as its platform runs code.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "chain_table.h"
#include "cpu_state.h"
#include "mem.h"
#include "tier_table.h"

namespace llvm { class MemoryBuffer; }

// A compiled block: void bb_0x<pc>(CpuState*, Memory*) in the blocks'
// calling convention (block_abi.h), in which a block that runs the next one
// itself (a musttail call, see CpuState::chain_table) is always a tail
// call. C++ cannot call one directly where that convention is not C's:
// ExecBackend::enter() does.
using BbFunc = void (*)(CpuState*, Memory*);
// The C-ABI function enter() calls: it calls the block, with its arguments
// in the registers the blocks' convention takes them in.
using EnterFunc = void (*)(BbFunc, CpuState*, Memory*);

struct BBInfo {
    BbFunc fn = nullptr;
    uint32_t fallthrough_pc = 0;
    BBInfo() = default;
    BBInfo(BbFunc f, uint32_t fpc) : fn(f), fallthrough_pc(fpc) {}
};

class JitEngine;

class ExecBackend {
public:
    virtual ~ExecBackend();

    virtual bool init() = 0;
    // The object's function name (bb_0x<pc>, or a variant such as
    // bb_0x<pc>.careful: BBTranslator::translate) replaces whatever was
    // compiled for pc.
    virtual bool addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, uint32_t fallthrough_pc,
                           const std::string& name) = 0;
    // The native engine, for what it alone does (code placed in its arenas,
    // trampolines, the profiling listeners); null for another.
    virtual JitEngine* native() { return nullptr; }

    // Code made elsewhere for pc, as addObject's: it replaces whatever was
    // compiled for pc.
    void addCode(uint32_t pc, BbFunc fn, uint32_t fallthrough_pc);
    BbFunc lookup(uint32_t pc);
    bool empty() const { return cache_.empty(); }
    // Runs fn (from lookup()) on cpu and mem.
    void enter(BbFunc fn, CpuState* cpu, Memory* mem) const { enter_(fn, cpu, mem); }
    // The chain table (CpuState::chain_table). A block is in it from when it
    // is compiled, unless the filter rejects its PC; a block that loses its
    // slot to another with the same index gets it back when the host looks
    // it up. A new engine (Core::invalidate) starts with an empty table.
    const ChainSlot* chainTable() const { return chain_; }
    // The tier-0 counters (TierTable), after the chain table.
    TierTable* tierTable() const { return area_ ? &area_->tier : nullptr; }
    // PCs the filter rejects are never in the table, so generated code never
    // runs them itself: the host runs every block that starts there. Setting
    // it empties the table; a block is published again when the host next
    // looks it up, if the filter accepts it.
    void setChainFilter(std::function<bool(uint32_t)> filter);
    void set_executing(uint32_t pc) { executing_bb_pc_ = pc; }
    void clear_executing()          { executing_bb_pc_ = 0; }

    // Iterate all cached blocks as fn(host_addr, guest_pc). Addresses are
    // the functions' (table indices on WebAssembly), sizes are not tracked here -- use the load listener
    // (via Core::forEachCompiledBlock) when exact sizes are needed.
    template <typename Fn>
    void forEachCompiled(Fn&& fn) const {
        for (const auto& kv : cache_)
            fn(reinterpret_cast<const void*>(kv.second.fn), kv.first);
    }

protected:
    ExecBackend();
    // The tables, empty; an engine's init makes them.
    void makeTables();
    EnterFunc enter_ = nullptr;

private:
    std::unordered_map<uint32_t, BBInfo> cache_;
    // The chain table, and the tier table after it (kTierTableOffset).
    struct ChainArea {
        ChainSlot slots[size_t(1) << kChainBits];
        TierTable tier;
    };
    std::unique_ptr<ChainArea> area_;
    ChainSlot* chain_ = nullptr;
    std::function<bool(uint32_t)> chain_filter_;
    void publishChain(uint32_t pc, BbFunc fn);
    uint32_t executing_bb_pc_ = 0;  // PC of currently-executing BB (0 = none)
};
