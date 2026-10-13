#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

class Memory;

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

    BBTranslateResult translate(uint32_t pc);
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

private:
    Memory* mem_;
    bool unlimited_;
    static constexpr uint32_t kMaxPackets = 256;
    uint32_t max_packets_ = kMaxPackets;
    bool budget_limited_ = false;
    bool fastmem_ = false;
    uint64_t fast_base_ = 0;
    std::string data_layout_;
    std::string target_triple_;
};
