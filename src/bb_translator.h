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
        max_packets_ = max_packets < 1 ? 1 : (max_packets > 256 ? 256 : max_packets);
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
    uint32_t max_packets_ = 256;
    bool fastmem_ = false;
    uint64_t fast_base_ = 0;
    std::string data_layout_;
    std::string target_triple_;
};
