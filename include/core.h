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

    // opt_level: 0=None, 1=Less, 2=Default, 3=Aggressive
    bool init(int opt_level = 2);
    void invalidate(uint32_t addr, uint32_t size);
    bool run(uint32_t pc);
    std::tuple<std::string, uint32_t> disassemble(uint32_t pc);
    void set_dump_ir(bool enable);

private:
    CpuState* cpu_;
    Memory* mem_;
    std::shared_ptr<JitEngine> jit_;
    std::shared_ptr<BBTranslator> translator_;
    int opt_level_ = 2;
    bool dump_ir_ = false;
};