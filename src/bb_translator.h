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

#include "lift_visitor.h"

namespace bcore {

class Memory;
struct CpuState;

struct BBTranslateResult {
    // The context first: members are destroyed last to first, and a module
    // must go before its context (found in a dropped promotion: Core
    // returned without taking the module, which then went after its
    // context).
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    uint32_t fallthrough_pc;
    // Where decoding ended the block (BcoreCompileMetrics::terminator): the
    // kind of its terminating instruction, or "packet-cap" (the 256-packet
    // block limit) or "step-budget" (a limited-mode budget of at most 256
    // packets). A conditional branch never ends a block: its taken path
    // exits and decoding continues. A static string.
    const char* exit_kind = "packet-cap";
    // A tier-0 block's threshold (Tier: tier_threshold of its packets).
    uint32_t tier_threshold = 0;
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
        uint32_t size_ref = 0;  // tier_threshold (tier_table.h)
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

    // Stencils for the copy-and-patch tier 0 (cp_translator.h), which
    // tools/stencilgen.cpp builds (bb_stencil.cpp). A stencil leaves holes
    // in its code, constant expressions of globals whose relocations say
    // where the translator fills them in.
    struct StencilHoles {
        llvm::Constant* pc = nullptr;      // i32: the packet's PC
        llvm::Constant* packet = nullptr;  // i32: its index in the block, from 1
        llvm::Constant* fields[LiftVisitor::kFieldHoles] = {};  // i32 (LiftVisitor::set_stencil)
        llvm::Constant* regs[8] = {};      // i64 offsets of data registers (LiftVisitor::set_stencil_regs)
        llvm::Function* cont = nullptr;    // the next packet's code
        llvm::Function* exit = nullptr;    // where every path that leaves the block goes
        llvm::Constant* lt[2] = {};        // i32: the hint's LT0 and LT1 (a pinned tail's)
    };
    struct StencilLift {
        llvm::Function* fn = nullptr;
        int bytes = 0;            // the packet's length
        bool terminated = false;  // no path goes on to the next packet
        bool cycles = false;      // reads or writes CYCLES (see emit_stencil_prologue)
        uint32_t field_values[LiftVisitor::kFieldHoles] = {};  // LiftVisitor::field_values
        unsigned fields_used = 0;
        bool reg_violation = false;  // LiftVisitor::reg_hole_violation
        // translate_stencil_body: whether it read a field after writing it
        // (LiftVisitor::reads_after_write), and left DISALGNEXCPT on.
        bool reads_after_write = false;
        bool dis_algn = false;
        // translate_slot_stencil: whether the slot takes the instruction,
        // and its writes (LiftVisitor::end_slot), whether they fit, whether
        // it wants the CEC check after the packet, and whether did_jump is
        // known clear on its way on.
        bool legal = true;
        std::vector<LiftVisitor::SlotWrite> writes;
        bool slot_conflict = false;
        bool checks_cec = false;
        bool did_jump_clear = true;
        // Whether it wrote LB or LT or called a helper that may
        // (LiftVisitor::loops_moved; a slot's writes too).
        bool loops_moved = false;
    };
    // A parallel packet's tail, by what the block's hint says of its loop
    // bottoms (translate_parallel_tail).
    enum class TailRole {
        kCareful,  // compares, and calls bfin_hwloop_step at a bottom
        kBottom,   // a bottom of the hint: the step inline, compares and all
        kAbsent,   // under the entry guard, no loop ends here: no compares
        kPinned0,  // under the entry guard, loop 0 ends here (LiftVisitor::pin_mask)
        kPinned1,  // loop 1
        kPinned01, // both
    };
    // The packet at pc, read through this translator's Memory, as the
    // function `name`, compiled as packet `holes.packet` of a careful block
    // with no hint would be (both hold whatever the guest state): the path
    // that goes on to the next packet ends in a musttail call of
    // holes.cont, every other in one of holes.exit. pc gives the offsets of
    // the PCs the code uses from the hole holes.pc.
    StencilLift translate_stencil(llvm::Module& mod, const char* name, uint32_t pc, const StencilHoles& holes);
    // A parallel packet's pieces (cp_translator.h), which run in the order
    // slot 1, slot 2, slot 0, slot 1's commit, slot 2's, the tail:
    //   - slot 0, the 32-bit instruction at pc, alone: its effect, with no
    //     packet state, epilog or exit (it calls no helper and cannot
    //     leave), then holes.cont. With parallel, its writes are made at
    //     its end, as a parallel packet's, else as it makes them, which is
    //     the same where it reads no field after writing one;
    //   - slot (1 or 2), the 16-bit instruction at pc + 2 + 2 * slot of the
    //     packet at pc, dis_algn as slot 0 left it, its register writes to
    //     CpuState::slot_scratch (LiftVisitor::begin_slot); its exits as the
    //     packet's;
    //   - a slot's commit: its writes from the scratch to the registers;
    //   - the packet's tail at pc: its epilog with the CEC check or not, by
    //     role: careful; or as a block whose hint has a loop end at pc lifts
    //     it careful (LiftVisitor::emit_epilog_clear: the step inline, its
    //     compares still made, so it holds whatever the loop registers
    //     are); or not careful, under the entry guard (emit_stencil_guard),
    //     as a hint with no bottom at pc or with loop 0, 1 or both pinned
    //     there (their tops holes.lt) lifts it.
    // A slot and the tail start with did_jump known clear, as a packet does
    // (a slot leaves it so, StencilLift::did_jump_clear: the build checks).
    StencilLift translate_stencil_body(llvm::Module& mod, const char* name, uint32_t pc, const StencilHoles& holes,
                                       bool parallel);
    StencilLift translate_slot_stencil(llvm::Module& mod, const char* name, uint32_t pc, const StencilHoles& holes,
                                       int slot, bool dis_algn);
    static llvm::Function* emit_slot_commit(llvm::Module& mod, const char* name, int slot,
                                            const std::vector<LiftVisitor::SlotWrite>& writes, llvm::Function* cont);
    StencilLift translate_parallel_tail(llvm::Module& mod, const char* name, uint32_t pc, const StencilHoles& holes,
                                        bool check_cec, TailRole role = TailRole::kCareful);
    // A copy-and-patch block's entry guard (emit_lb_guard's): each LB is
    // lb[i] or outside [block_pc, block_pc + span], and LB and LT are lb[i]
    // and lt[i] for a loop in `pinned` (bit i: loop i). Then a musttail call
    // of cont; otherwise PC the block's, CpuState::lb_guard_miss set, and a
    // return before the block's first packet.
    llvm::Function* emit_stencil_guard(llvm::Module& mod, const char* name, unsigned pinned, llvm::Constant* block_pc,
                                       llvm::Constant* span, llvm::Constant* const lb[2],
                                       llvm::Constant* const lt[2], llvm::Function* cont);
    // A copy-and-patch block's start: its tier counter (emit_tier_counter,
    // with holes for the counter, its count's offset and the threshold;
    // none for a null counter, a block that never asks to be promoted),
    // CYCLES with host_cycles (emit_host_cycles), then a musttail call of cont.
    llvm::Function* emit_stencil_prologue(llvm::Module& mod, const char* name, llvm::Constant* counter,
                                          llvm::Constant* count_offset, llvm::Constant* threshold, bool host_cycles,
                                          llvm::Function* cont);
    // The exit of a copy-and-patch block: its chain exit (set_chaining,
    // set_count_chained) with the next PC and the packet index the CpuState
    // holds, as a block's exit at a PC known only at run time; without
    // chaining a return. With block_pc and self (a value hole and a code
    // hole), the exit of one block, placed after it: a next PC of
    // block_pc goes on to self, the block's start, without the table.
    llvm::Function* emit_stencil_exit(llvm::Module& mod, const char* name, llvm::Constant* block_pc = nullptr,
                                      llvm::Function* self = nullptr);
    // A function of the blocks' type and convention (BbFunc, jit_engine.h).
    static llvm::Function* declare_block(llvm::Module& mod, const char* name);
    static void declare_externs(llvm::Module& mod);

private:
    static void emit_host_cycles(llvm::IRBuilder<>& b, llvm::Value* cpu, llvm::BasicBlock* first_entry);
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
    static void emit_tier_counter(llvm::IRBuilder<>& b, llvm::Value* cpu, llvm::BasicBlock* first_entry,
                                  llvm::Value* counter, llvm::Value* count_offset, llvm::Value* threshold);
    static bool expand_loop_syncs(llvm::Function* fn, llvm::AllocaInst* shadow, llvm::Value* real, bool steady_skip);
    // direct: a block whose return stays a return. entry_pc_value: the
    // entry PC a next PC known only at run time is compared with for
    // self_loop (entry_pc where null).
    void emit_chain_exit(llvm::IRBuilder<>& b, llvm::Function* fn, llvm::Value* cpu, llvm::Value* real,
                         llvm::Value* mem,
                         uint32_t entry_pc, llvm::BasicBlock* self_loop, llvm::BasicBlock* direct,
                         llvm::BasicBlock* first_entry, const std::set<llvm::ReturnInst*>* steady,
                         llvm::Value* entry_pc_value = nullptr);
    // A function's plain returns as one (tier 0).
    static void merge_returns(llvm::Function* fn);
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

}  // namespace bcore
