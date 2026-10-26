#include "host_bridge.h"

#include <cstring>
#include <exception>

#include "cec.h"
#include "syscall_emu.h"

namespace bcore_bridge {

namespace {

thread_local CpuState* t_cpu = nullptr;
thread_local std::exception_ptr t_caught;

// In a catch block: keep the exception and stop the run.
void caught() noexcept {
    if (!t_caught) t_caught = std::current_exception();
    if (t_cpu) {
        t_cpu->did_jump = true;
        t_cpu->chain_budget = 0;
    }
}

}  // namespace

uint8_t mem_read8(Memory* mem, uint32_t addr) noexcept {
    try { return ::mem_read8(mem, addr); } catch (...) { caught(); return 0; }
}
uint16_t mem_read16(Memory* mem, uint32_t addr) noexcept {
    try { return ::mem_read16(mem, addr); } catch (...) { caught(); return 0; }
}
uint32_t mem_read32(Memory* mem, uint32_t addr) noexcept {
    try { return ::mem_read32(mem, addr); } catch (...) { caught(); return 0; }
}
void mem_write8(Memory* mem, uint32_t addr, uint8_t value) noexcept {
    try { ::mem_write8(mem, addr, value); } catch (...) { caught(); }
}
void mem_write16(Memory* mem, uint32_t addr, uint16_t value) noexcept {
    try { ::mem_write16(mem, addr, value); } catch (...) { caught(); }
}
void mem_write32(Memory* mem, uint32_t addr, uint32_t value) noexcept {
    try { ::mem_write32(mem, addr, value); } catch (...) { caught(); }
}
void cec_data_alignment(CpuState* cpu, Memory* mem, uint32_t pc, uint32_t addr, uint32_t flags) noexcept {
    try { ::cec_data_alignment(cpu, mem, pc, addr, flags); } catch (...) { caught(); }
}
void cec_exception(CpuState* cpu, uint32_t excp, Memory* mem) noexcept {
    try { ::cec_exception(cpu, excp, mem); } catch (...) { caught(); }
}
void bfin_syscall(CpuState* cpu, Memory* mem) noexcept {
    try { ::bfin_syscall(cpu, mem); } catch (...) { caught(); }
}

void* wrapper(const char* name) {
    static const struct { const char* name; void* fn; } kWrappers[] = {
        {"mem_read8", reinterpret_cast<void*>(&mem_read8)},
        {"mem_read16", reinterpret_cast<void*>(&mem_read16)},
        {"mem_read32", reinterpret_cast<void*>(&mem_read32)},
        {"mem_write8", reinterpret_cast<void*>(&mem_write8)},
        {"mem_write16", reinterpret_cast<void*>(&mem_write16)},
        {"mem_write32", reinterpret_cast<void*>(&mem_write32)},
        {"cec_data_alignment", reinterpret_cast<void*>(&cec_data_alignment)},
        {"cec_exception", reinterpret_cast<void*>(&cec_exception)},
        {"bfin_syscall", reinterpret_cast<void*>(&bfin_syscall)},
    };
    for (const auto& w : kWrappers)
        if (std::strcmp(name, w.name) == 0) return w.fn;
    return nullptr;
}

RunScope::RunScope(CpuState* cpu) : previous_(t_cpu) { t_cpu = cpu; }

RunScope::~RunScope() { t_cpu = previous_; }

void RunScope::rethrow() {
    if (!t_caught) return;
    std::exception_ptr e = t_caught;
    t_caught = nullptr;
    std::rethrow_exception(e);
}

}  // namespace bcore_bridge
