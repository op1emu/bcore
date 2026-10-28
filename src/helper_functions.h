#pragma once

// The C function behind each helper generated code calls (helper_abi.h's
// kHelpers, by name), and what an engine binds the name to: the function,
// or where a host's exception may not cross generated code its wrapper,
// which catches it (host_bridge.h, kHelpersCatch). No LLVM: every engine's.

#include <utility>

#include "cec.h"
#include "cpu_state.h"
#include "host_bridge.h"
#include "mem.h"
#include "syscall_emu.h"

namespace bcore_abi {

inline const std::pair<const char*, void*> kHelperFunctions[] = {
    {"mem_read8", reinterpret_cast<void*>(&mem_read8)},
    {"mem_read16", reinterpret_cast<void*>(&mem_read16)},
    {"mem_read32", reinterpret_cast<void*>(&mem_read32)},
    {"mem_write8", reinterpret_cast<void*>(&mem_write8)},
    {"mem_write16", reinterpret_cast<void*>(&mem_write16)},
    {"mem_write32", reinterpret_cast<void*>(&mem_write32)},
    {"bfin_syscall", reinterpret_cast<void*>(&bfin_syscall)},
    {"bfin_putchar", reinterpret_cast<void*>(&bfin_putchar)},
    {"cec_data_alignment", reinterpret_cast<void*>(&cec_data_alignment)},
    {"cec_exception", reinterpret_cast<void*>(&cec_exception)},
    {"cec_raise", reinterpret_cast<void*>(&cec_raise)},
    {"cec_return_rti", reinterpret_cast<void*>(&cec_return_rti)},
    {"cec_return_rtx", reinterpret_cast<void*>(&cec_return_rtx)},
    {"cec_return_rtn", reinterpret_cast<void*>(&cec_return_rtn)},
    {"cec_return_rte", reinterpret_cast<void*>(&cec_return_rte)},
    {"cec_cli", reinterpret_cast<void*>(&cec_cli)},
    {"cec_sti", reinterpret_cast<void*>(&cec_sti)},
    {"cec_push_reti", reinterpret_cast<void*>(&cec_push_reti)},
    {"cec_pop_reti", reinterpret_cast<void*>(&cec_pop_reti)},
    {"cec_check_sup", reinterpret_cast<void*>(&cec_check_sup)},
    {"cec_is_user_mode", reinterpret_cast<void*>(&cec_is_user_mode)},
    {"cec_check_pending", reinterpret_cast<void*>(&cec_check_pending)},
    {"bfin_hwloop_step", reinterpret_cast<void*>(&bfin_hwloop_step)},
};

inline void* bound_helper(const char* name, void* fn) {
    void* caught = BCORE_HELPERS_CATCH ? bcore_bridge::wrapper(name) : nullptr;
    return caught ? caught : fn;
}

}  // namespace bcore_abi
