#pragma once

// A promotion's job (remote_tier.h): what Core::submit_promotion encodes and
// RemoteCompiler::compile decodes. Little-endian, as WebAssembly is, and
// for one build: the CPU state goes as its bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cpu_state.h"

namespace bcore {

struct RemoteJob {
    uint32_t pc = 0;
    bool careful = false;
    // What the Memory says about itself (Memory::page_table() non-null,
    // callbacks_preserve_registers) and the chain filter about pc.
    bool page_table = false;
    bool preserve_registers = false;
    bool chain_ok = true;
    bool light = false;  // Core::set_light_tier1
    // The Core's settings that its translations read: those of
    // Core::init_llvm (and init's opt and codegen levels).
    struct Settings {
        int32_t opt_level = 2, codegen_level = -1;
        uint32_t max_packets = 0;
        bool chaining = false, dry = false, host_cycles = false, count_chained = true, lazy_state = true,
             loop_blocks = true, verify = true, idle_waits = false;
        int32_t steady_skip = 0;
        std::string post_pipeline;
    } settings;
    std::vector<std::pair<uint32_t, uint16_t>> code;  // the halfwords tier 0 read
    CpuState hint{};                                  // its pointers null
};

std::vector<uint8_t> encode_job(const RemoteJob& job);
// False (why) for bytes that are no job of this build's.
bool decode_job(const uint8_t* bytes, size_t size, RemoteJob& job, std::string& why);
// The settings' bytes alone: which Core of RemoteCompiler's compiles it.
std::vector<uint8_t> encode_settings(const RemoteJob::Settings& settings);

}  // namespace bcore
