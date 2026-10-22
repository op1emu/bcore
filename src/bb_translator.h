#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

class Memory;
#include "lift_visitor.h"
struct CpuState;

struct BBTranslateResult {
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::LLVMContext> context;
    uint32_t fallthrough_pc;
    // Where decoding ended the block (BcoreCompileMetrics::terminator): the
    // kind of its terminating instruction, or "packet-cap" (the 256-packet
    // block limit) or "step-budget" (a limited-mode budget of at most 256
    // packets). A conditional branch never ends a block: its taken path
    // exits and decoding continues. A static string.
    const char* exit_kind = "packet-cap";
};

class BBTranslator {
public:
    BBTranslator(Memory* mem,
                 bool unlimited = true,
                 bool fastmem = false, uint64_t fast_base = 0);

    // hint: the guest state now, for heuristics only (LiftVisitor::set_hint).
    // careful: the block compares the loop bottoms at every packet, with no
    // entry guard (LiftVisitor::set_careful); its function is
    // bb_0x<pc>.careful.
    // Tiered compilation (Core::set_tiering). Tier 0: the plain block only,
    // never a loop block, counting its starts in TierTable::count[counter]
    // and asking to be compiled again (TierTable::request) when the count
    // reaches threshold. Tier 1: the block as an untiered translation makes
    // it, its function named bb_0x<pc>[.careful].t1. Either reads its
    // instructions through code when that is set.
    struct Tier {
        int level = 0;
        uint32_t counter = 0;
        uint32_t threshold = 0;
        Memory* code = nullptr;
    };
    BBTranslateResult translate(uint32_t pc, const CpuState* hint = nullptr, bool careful = false,
                                const Tier* tier = nullptr);
    // Loop blocks: a block that runs itself again in a native loop keeps the
    // guest state in an alloca, which the IR pipeline turns into SSA values,
    // so a loop's registers stay in host registers across its back edge
    // instead of going through CpuState at every packet. It copies the state
    // to the real CpuState before each helper call and each exit, and back
    // after each call that may have changed it. Exact either way.
    void set_loop_blocks(bool loop) { loop_blocks_ = loop; }
    // See Core::set_verify.
    void set_verify(bool verify) { verify_ = verify; }
    // An IDLE ends its block (Core::set_idle_waits).
    void set_idle_waits(bool waits) { idle_waits_ = waits; }
    // Loop blocks may skip iterations (Core::set_steady_skip): their copies
    // to the real state then write every field the block stores anywhere.
    void set_steady_skip(bool steady) { steady_skip_ = steady; }
    // The declaration a loop block's exits call where they copy the shadow
    // out (replaced by the copies, as LiftVisitor::kSyncOut is).
    static constexpr const char* kCommit = "bcore.loop.commit";
    // The function attribute a loop block carries.
    static constexpr const char* kLoopBlockAttr = "bcore-loop-block";
    // A loop block's DAG registers taken to stay in their circular buffers.
    static constexpr const char* kCircularAttr = "bcore-dag-circular";
    // Upper bound on packets per block (default and maximum 256). Limited
    // mode sets it to the step budget, since a run never executes more:
    // translating the rest of the block would only be compiled again from
    // the next packet, so single-stepping an N-packet block compiled O(N^2)
    // IR.
    void set_max_packets(uint32_t max_packets) {
        max_packets_ = max_packets < 1 ? 1 : (max_packets > kMaxPackets ? kMaxPackets : max_packets);
        // Which limit a block that runs out of packets hits: the step budget
        // only when it is no larger than the block cap.
        budget_limited_ = max_packets <= kMaxPackets;
    }
    // DataLayout / triple stamped on every module at creation (the JIT's own,
    // so LLJIT's addIRModule consistency check passes). Empty strings leave
    // the module at LLVM's defaults for LLJIT to fill in.
    void setModuleTarget(std::string data_layout, std::string triple) {
        data_layout_ = std::move(data_layout);
        target_triple_ = std::move(triple);
    }
    // See Core::set_chaining, set_chain_dry and set_host_cycles.
    void set_chaining(bool chaining, bool dry) { chaining_ = chaining; chain_dry_ = dry; }
    // See Core::set_chain_filter: a block runs itself again in a native loop
    // only if the filter accepts its PC when it is translated.
    void set_chain_filter(std::function<bool(uint32_t)> filter) { chain_filter_ = std::move(filter); }
    void set_host_cycles(bool host_cycles) { host_cycles_ = host_cycles; }
    // See Core::set_count_chained.
    void set_count_chained(bool count) { count_chained_ = count; }
    // See Core::set_lazy_state.
    void set_lazy_state(bool lazy) { lazy_state_ = lazy; }

private:
    void emit_host_cycles(llvm::IRBuilder<>& b, llvm::Value* cpu, llvm::BasicBlock* first_entry);
    llvm::BasicBlock* emit_lb_guard(llvm::IRBuilder<>& b, llvm::Function* fn, llvm::Value* cpu, llvm::Value* real,
                                    const LiftVisitor& visitor, llvm::BasicBlock* first_entry, uint32_t first_pc, uint32_t last_pc,
                                    const uint32_t hint_lb[2], const uint32_t hint_lt[2], unsigned pinned,
                                    unsigned l_zero, unsigned circular = 0,
                                    const LiftVisitor::Circular* circ_use = nullptr);
    // What a loop block keeps in its shadow (LiftVisitor::set_loop_mode).
    struct LoopPlan {
        std::set<size_t> fields;
        unsigned l_zero = 0;
        unsigned circular = 0;            // LiftVisitor::Circular: bit i, I[i]'s buffer
        LiftVisitor::Circular circ_use;   // its steps, for the entry guard
    };
    BBTranslateResult translate_as(uint32_t pc, const CpuState* hint, bool careful,
                                   const LoopPlan* loop, LoopPlan* plan, const Tier* tier);
    static void emit_tier_counter(llvm::IRBuilder<>& b, llvm::Value* cpu, llvm::BasicBlock* first_entry,
                                  uint32_t counter, uint32_t threshold);
    static bool expand_loop_syncs(llvm::Function* fn, llvm::AllocaInst* shadow, llvm::Value* real, bool steady_skip);
    // direct: a block whose return stays a return.
    void emit_chain_exit(llvm::IRBuilder<>& b, llvm::Function* fn, llvm::Value* cpu, llvm::Value* real,
                         llvm::Value* mem,
                         uint32_t entry_pc, llvm::BasicBlock* self_loop, llvm::BasicBlock* direct,
                         llvm::BasicBlock* first_entry, const std::set<llvm::ReturnInst*>* steady);
    static llvm::BasicBlock* lb_guard_miss_block(llvm::BasicBlock* guard);

    Memory* mem_;
    bool unlimited_;
    static constexpr uint32_t kMaxPackets = 256;
    uint32_t max_packets_ = kMaxPackets;
    bool budget_limited_ = false;
    bool fastmem_ = false;
    uint64_t fast_base_ = 0;
    bool chaining_ = false;
    bool chain_dry_ = false;
    std::function<bool(uint32_t)> chain_filter_;
    bool loop_blocks_ = false;
    bool verify_ = true;
    bool idle_waits_ = false;
    bool steady_skip_ = false;
    bool host_cycles_ = false;
    bool count_chained_ = true;
    bool lazy_state_ = true;
    std::string data_layout_;
    std::string target_triple_;
};
