#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/Support/MemoryBuffer.h>

#include "chain_table.h"
#include "code_loader.h"
#include "tier_table.h"
#include "cpu_state.h"
#include "mem.h"

namespace bcore {

struct BBInfo {
    BbFunc fn = nullptr;
    uint32_t fallthrough_pc = 0;
    BBInfo() = default;
    BBInfo(BbFunc f, uint32_t fpc) : fn(f), fallthrough_pc(fpc) {}
};

// Which block each PC has, and the tables generated code reads (the chain
// table, the tier table), over a CodeLoader (code_loader.h) that loads the
// blocks for the host.
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
    // Code made otherwise for pc (placed in the loader's arena: the
    // copy-and-patch tier), as addObject's: it replaces whatever was
    // compiled for pc.
    void addCode(uint32_t pc, BbFunc fn, uint32_t fallthrough_pc);
    CodeLoader& loader() { return loader_; }
    BbFunc lookup(uint32_t pc);
    bool empty() const { return cache_.empty(); }
    // Runs fn (from lookup()) on cpu and mem.
    void enter(BbFunc fn, CpuState* cpu, Memory* mem) const { loader_.enter(fn, cpu, mem); }
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

    // Iterate all cached blocks as fn(host_addr, guest_pc). Addresses come
    // from the symbol lookup, sizes are not tracked here -- use the load
    // listener (via Core::forEachCompiledBlock) when exact sizes are needed.
    template <typename Fn>
    void forEachCompiled(Fn&& fn) const {
        for (const auto& kv : cache_)
            fn(reinterpret_cast<const void*>(kv.second.fn), kv.first);
    }

private:
    CodeLoader loader_;  // first, so that it goes last: after every table of its code
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

}  // namespace bcore
