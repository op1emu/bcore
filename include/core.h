#pragma once

#include <cstdint>
#include <memory>
#include <tuple>

#include "cpu_state.h"
#include "mem.h"

class JitEngine;
class BBTranslator;

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

private:
    void applyModuleTarget();

    CpuState* cpu_;
    Memory* mem_;
    std::shared_ptr<JitEngine> jit_;
    std::shared_ptr<BBTranslator> translator_;
    int opt_level_ = 2;
    int codegen_level_ = -1;
    bool dump_ir_ = false;
};