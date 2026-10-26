#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/MemoryBuffer.h>

#include "chain_table.h"
#include "tier_table.h"
#include "cpu_state.h"
#include "mem.h"

namespace llvm { class JITEventListener; }

// A JIT'd block: void bb_0x<pc>(CpuState*, Memory*) in the blocks' calling
// convention (block_abi.h), in which a block that runs the next one itself
// (a musttail call, see CpuState::chain_table) is always a tail call. C++
// cannot call one directly: JitEngine::enter() does.
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

class BBTranslator;
class CodeArena;
class CodeArenaPool;


class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    bool init();
    // The object's function name (bb_0x<pc>, or a variant such as
    // bb_0x<pc>.careful: BBTranslator::translate) replaces whatever was
    // compiled for pc.
    bool addObject(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object,
                   uint32_t fallthrough_pc, const std::string& name);
    // Code placed in an arena of this engine (codeArena) for pc, as
    // addObject's: it replaces whatever was compiled for pc.
    void addCode(uint32_t pc, BbFunc fn, uint32_t fallthrough_pc);
    // An arena of this engine's with room for `code` bytes of code beside
    // the code it holds (null if there is none), for code placed in it
    // directly (the copy-and-patch tier, cp_translator.h). The engine keeps
    // it, so that such code lives as long as the engine does, as objects'
    // code does (found in review: an arena only the copy-and-patch
    // translator held went with it, under blocks still in the cache).
    std::shared_ptr<CodeArena> codeArena(uintptr_t code);
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

    // Register L on the underlying object-linking layer for object load/free
    // notifications (the profiling code map builds itself from these).
    // Behavior-neutral on x86-64 Linux where LLJIT defaults to
    // RTDyldObjectLinkingLayer; returns false (and registers nothing) on any
    // other layer so callers can degrade gracefully.
    bool registerJITEventListener(llvm::JITEventListener& L);
    // Undo registerJITEventListener (no-op on other linking layers).
    void unregisterJITEventListener(llvm::JITEventListener& L);

    // Iterate all cached blocks as fn(host_addr, guest_pc). Addresses come
    // from the symbol lookup, sizes are not tracked here -- use the load
    // listener (via Core::forEachCompiledBlock) when exact sizes are needed.
    template <typename Fn>
    void forEachCompiled(Fn&& fn) const {
        for (const auto& kv : cache_)
            fn(reinterpret_cast<const void*>(kv.second.fn), kv.first);
    }

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::shared_ptr<CodeArenaPool> pool_;
    std::vector<std::shared_ptr<CodeArena>> placed_;  // codeArena's arenas
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
    bool defineEnter();
    EnterFunc enter_ = nullptr;
    uint32_t executing_bb_pc_ = 0;  // PC of currently-executing BB (0 = none)
};
