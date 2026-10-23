#include "compile_worker.h"

#include <ctime>

static uint64_t clock_ns(clockid_t clock) {
    timespec ts{};
    clock_gettime(clock, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

CompileWorker::CompileWorker(std::unique_ptr<Compiler> compiler, std::function<void()> on_start)
    : compiler_(std::move(compiler)), on_start_(std::move(on_start)) {
    thread_ = std::thread([this] { loop(); });
}

CompileWorker::~CompileWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    work_.notify_all();
    thread_.join();
}

void CompileWorker::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    work_.notify_one();
}

bool CompileWorker::take(Result& out, bool wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (wait) idle_.wait(lock, [this] { return !results_.empty() || (jobs_.empty() && !busy_); });
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

void CompileWorker::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void CompileWorker::set_generation(uint64_t generation) { generation_ = generation; }

void CompileWorker::loop() {
    if (on_start_) on_start_();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        Result result = compile(std::move(job));
        lock.lock();
        busy_ = false;
        results_.push_back(std::move(result));
        idle_.notify_all();
    }
}

CompileWorker::Result CompileWorker::compile(Job job) {
    Result result;
    result.begin_ns = clock_ns(CLOCK_MONOTONIC);
    const uint64_t cpu_begin = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    auto stale = [&] { return job.generation < generation_.load(); };
    if (stale()) {
        job.module.reset();
        job.context.reset();
        result.job = std::move(job);
        result.end_ns = result.begin_ns;
        return result;
    }
    compiler_->optimize(*job.module, job.light);
    const ModuleCounts counts = count_module(*job.module);
    result.ir_instructions = counts.ir_instructions;
    result.ir_blocks = counts.ir_blocks;
    result.loop_blocks = counts.loop_blocks;
    result.loops_proven = counts.loops_proven;
    result.loops_speculated = counts.loops_speculated;
    result.loops_vectorized = counts.loops_vectorized;
    result.loops_steady = counts.loops_steady;
    result.dag_circular = counts.dag_circular;
    if (!stale()) result.object = compiler_->emit(*job.module);
    job.module.reset();  // before its context
    job.context.reset();
    result.job = std::move(job);
    result.cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_begin;
    result.end_ns = clock_ns(CLOCK_MONOTONIC);
    return result;
}
