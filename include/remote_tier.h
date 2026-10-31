#pragma once

// Promotions compiled by another instance of the program (Core::set_remote_tier):
// a Web Worker's, say, where a WebAssembly build has no threads. The Core
// that runs the guest hands its host a job, the bytes of everything its
// promotion of a block reads: the halfwords its tier-0 compilation read,
// the CPU state it saw (the hint), what its Memory says about itself and
// the chain filter about the block, and the Core's settings. Another
// instance (the same build) compiles it with a RemoteCompiler, as the Core
// would have (Core::compile), to an object that reaches nothing of the
// first instance's but through CpuState and by its imports' names; the
// first instance's host gives that back (Core::deliver).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bcore {

// A job's code: the object (as LLVM's backend wrote it), its function's
// name (the module's export) and the block's fallthrough PC; or why there
// is none.
struct RemoteCode {
    std::vector<uint8_t> object;
    std::string name;
    uint32_t fallthrough_pc = 0;
    std::string why;
};

// Compiles jobs. It keeps a Core (and LLVM's Compiler) for each set of
// settings the jobs name; one thread.
class RemoteCompiler {
public:
    RemoteCompiler();
    ~RemoteCompiler();
    // False (out.why) for bytes that are no job of this build's, code the
    // job does not have (its compilation read past it), or a failed
    // compilation.
    bool compile(const uint8_t* job, size_t size, RemoteCode& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bcore
