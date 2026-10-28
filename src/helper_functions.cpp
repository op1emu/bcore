#include "helper_functions.h"
#include "cec.h"
#include "cpu_state.h"
#include "helper_abi.h"
#include "host_bridge.h"
#include "mem_helpers.h"
#include "syscall_emu.h"

#include <cstring>

namespace bcore {

namespace bcore_abi {

std::vector<std::pair<const char*, void*>> helper_functions() {
    const std::pair<const char*, void*> functions[] = {
        {"mem_read8", reinterpret_cast<void*>(&mem_read8)},
        {"mem_read16", reinterpret_cast<void*>(&mem_read16)},
        {"mem_read32", reinterpret_cast<void*>(&mem_read32)},
        {"mem_write8", reinterpret_cast<void*>(&mem_write8)},
        {"mem_write16", reinterpret_cast<void*>(&mem_write16)},
        {"mem_write32", reinterpret_cast<void*>(&mem_write32)},
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
        {"bfin_putchar", reinterpret_cast<void*>(&bfin_putchar)},
        {"bfin_syscall", reinterpret_cast<void*>(&bfin_syscall)},
    };
    static_assert(sizeof(functions) / sizeof(functions[0]) == sizeof(kHelpers) / sizeof(kHelpers[0]),
                  "a function for every helper");
    std::vector<std::pair<const char*, void*>> out;
    for (size_t i = 0; i < sizeof(kHelpers) / sizeof(kHelpers[0]); ++i) {
        const auto& [name, fn] = functions[i];
        if (std::strcmp(name, kHelpers[i].name) != 0) llvm::report_fatal_error("helper_functions: not kHelpers' order");
        void* caught = kHelpersCatch ? bcore_bridge::wrapper(name) : nullptr;
        out.emplace_back(name, caught ? caught : fn);
    }
    return out;
}

}  // namespace bcore_abi

}  // namespace bcore
