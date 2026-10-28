#pragma once

// Host exceptions and generated code. A host's memory callback may throw;
// where the exception cannot unwind through generated code, the helpers
// that may reach the host's code (the memory callbacks, and the exceptions
// and system calls that read guest memory) are called through wrappers that
// catch whatever the host throws: the wrapper sets did_jump and a chain
// budget of 0, so that the block leaves at once, and Core::run throws it
// again once the block has returned (RunScope).
//
// Copy-and-patch stencils have no unwind information (cp_translator.h);
// on macOS RuntimeDyld registers no usable unwind information for Mach-O
// AArch64 objects (helper_abi.h's kHelpersCatch).

#include <cstdint>

#include "cpu_state.h"
#include "mem.h"

// On macOS the helpers catch, and in a WebAssembly build, where an
// exception that left a block would leave the blocks' stack pointer where
// the block's frame had moved it (wasm_engine.cpp); BCORE_HELPERS_CATCH=1
// makes them catch on any host. Every helper that may reach host code is
// among the wrappers (host_bridge.cpp): one that may throw and is not would
// be a hole there.
#ifndef BCORE_HELPERS_CATCH
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)
#define BCORE_HELPERS_CATCH 1
#else
#define BCORE_HELPERS_CATCH 0
#endif
#endif

namespace bcore_bridge {

uint8_t mem_read8(Memory* mem, uint32_t addr) noexcept;
uint16_t mem_read16(Memory* mem, uint32_t addr) noexcept;
uint32_t mem_read32(Memory* mem, uint32_t addr) noexcept;
void mem_write8(Memory* mem, uint32_t addr, uint8_t value) noexcept;
void mem_write16(Memory* mem, uint32_t addr, uint16_t value) noexcept;
void mem_write32(Memory* mem, uint32_t addr, uint32_t value) noexcept;
void cec_data_alignment(CpuState* cpu, Memory* mem, uint32_t pc, uint32_t addr, uint32_t flags) noexcept;
void cec_exception(CpuState* cpu, uint32_t excp, Memory* mem) noexcept;
void bfin_syscall(CpuState* cpu, Memory* mem) noexcept;

// The wrapper of the helper `name`, or null for a helper that reaches no
// host code.
void* wrapper(const char* name);

// Set while a thread runs blocks: the CpuState a wrapper stops when it
// catches an exception. rethrow() throws what was caught, if anything.
class RunScope {
public:
    explicit RunScope(CpuState* cpu);
    ~RunScope();
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;
    void rethrow();

private:
    CpuState* previous_;
};

}  // namespace bcore_bridge
