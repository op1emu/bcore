#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

class Memory;
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
    BBTranslateResult translate(uint32_t pc, const CpuState* hint = nullptr, bool careful = false);
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
    // See Core::set_lazy_state.
    void set_lazy_state(bool lazy) { lazy_state_ = lazy; }

private:
    void emit_host_cycles(llvm::IRBuilder<>& b, llvm::Value* cpu, llvm::BasicBlock* first_entry);
    llvm::BasicBlock* emit_lb_guard(llvm::IRBuilder<>& b, llvm::Function* fn, llvm::Value* cpu,
                                    llvm::BasicBlock* first_entry, uint32_t first_pc, uint32_t last_pc,
                                    const uint32_t hint_lb[2]);
    // direct: a block whose return stays a return.
    void emit_chain_exit(llvm::IRBuilder<>& b, llvm::Function* fn, llvm::Value* cpu, llvm::Value* mem,
                         uint32_t entry_pc, llvm::BasicBlock* self_loop, llvm::BasicBlock* direct);
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
    bool host_cycles_ = false;
    bool lazy_state_ = true;
    std::string data_layout_;
    std::string target_triple_;
};
