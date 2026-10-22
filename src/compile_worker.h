#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>

#include "compiler.h"

// Compiles promotions (Core::set_tier_background) on a thread of its own: the
// IR pipeline and the backend, with a Compiler of its own. It never touches
// the JIT: the thread that runs Core::run lifts every module, and takes the
// objects back to load and publish them. Each job carries the engine
// generation it was lifted for; the owner drops results of an older one.
class CompileWorker {
public:
    struct Job {
        uint32_t pc = 0;
        uint32_t counter = 0;  // the tier-0 compilation's (TierTable)
        uint64_t generation = 0;
        uint32_t fallthrough_pc = 0;
        std::string name;
        const char* exit_kind = "packet-cap";
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;
    };
    struct Result {
        Job job;  // module and context gone
        std::unique_ptr<llvm::MemoryBuffer> object;  // null on failure
        // The module as compiled (BcoreCompileMetrics, BcoreStats).
        uint64_t ir_instructions = 0, ir_blocks = 0;
        uint64_t loop_blocks = 0, loops_proven = 0, loops_speculated = 0, loops_vectorized = 0;
        uint64_t loops_steady = 0, dag_circular = 0;
        // CLOCK_MONOTONIC span and the worker's CPU time in it.
        uint64_t begin_ns = 0, end_ns = 0, cpu_ns = 0;
    };

    // compiler: initialized, for this thread alone from now on. on_start runs
    // first on the new thread (the host's: priority, CPU).
    CompileWorker(std::unique_ptr<Compiler> compiler, std::function<void()> on_start);
    ~CompileWorker();  // waits for the job in flight, drops the rest
    CompileWorker(const CompileWorker&) = delete;
    CompileWorker& operator=(const CompileWorker&) = delete;

    void submit(Job job);
    // The oldest result, in the order of the jobs: false if there is none
    // yet (with wait, only if no job is queued or in flight either).
    bool take(Result& out, bool wait);
    // Blocks until no job is queued or in flight.
    void wait_idle();
    // A job of an older generation still gives its result, in order, but
    // without code: one queued does nothing, one in flight stops before
    // the backend.
    void set_generation(uint64_t generation);

private:
    void loop();
    Result compile(Job job);

    std::unique_ptr<Compiler> compiler_;
    std::function<void()> on_start_;
    std::mutex mutex_;
    std::condition_variable work_, idle_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    std::atomic<uint64_t> generation_{0};
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_;
};
