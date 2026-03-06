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

private:
    Memory* mem_;
    bool unlimited_;
    bool fastmem_ = false;
    uint64_t fast_base_ = 0;
};
