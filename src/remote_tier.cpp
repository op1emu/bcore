// Promotions compiled by another instance of the program (remote_tier.h):
// the jobs' bytes, and the instance that compiles them.
#include "remote_tier.h"

#include "bb_translator.h"
#include "code_loader.h"
#include "compiler.h"
#include "core.h"
#include "guest_memory.h"
#include "remote_job.h"
#include "tier_memory.h"

#include <cstring>
#include <map>

#include <llvm/IR/Module.h>

namespace bcore {

namespace {

constexpr char kMagic[4] = {'B', 'T', 'J', '1'};
constexpr uint32_t kVersion = 1;

class Writer {
public:
    template <typename T> void pod(const T& value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof value);
    }
    void flag(bool value) { out.push_back(value ? 1 : 0); }
    void str(const std::string& s) {
        pod(uint32_t(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    std::vector<uint8_t> out;
};

class Reader {
public:
    Reader(const uint8_t* bytes, size_t size) : p_(bytes), end_(bytes + size) {}
    template <typename T> bool pod(T& value) {
        if (size_t(end_ - p_) < sizeof value) return false;
        std::memcpy(&value, p_, sizeof value);
        p_ += sizeof value;
        return true;
    }
    bool flag(bool& value) {
        uint8_t byte;
        if (!pod(byte) || byte > 1) return false;
        value = byte;
        return true;
    }
    bool str(std::string& s) {
        uint32_t n;
        if (!pod(n) || size_t(end_ - p_) < n) return false;
        s.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }
    bool done() const { return p_ == end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

void write_settings(Writer& w, const RemoteJob::Settings& s) {
    w.pod(s.opt_level);
    w.pod(s.codegen_level);
    w.flag(s.unlimited);
    w.pod(s.max_packets);
    for (bool flag : {s.chaining, s.dry, s.host_cycles, s.count_chained, s.lazy_state, s.loop_blocks, s.verify,
                      s.idle_waits})
        w.flag(flag);
    w.pod(s.steady_skip);
    w.str(s.post_pipeline);
}

bool read_settings(Reader& r, RemoteJob::Settings& s) {
    return r.pod(s.opt_level) && r.pod(s.codegen_level) && r.flag(s.unlimited) && r.pod(s.max_packets) &&
           r.flag(s.chaining) &&
           r.flag(s.dry) && r.flag(s.host_cycles) && r.flag(s.count_chained) && r.flag(s.lazy_state) &&
           r.flag(s.loop_blocks) && r.flag(s.verify) && r.flag(s.idle_waits) && r.pod(s.steady_skip) &&
           r.str(s.post_pipeline);
}

// The Memory a job's compilation sees: what the host's said about itself,
// and no data (a promotion reads none; its code comes from the job's log,
// through a ReplayMemory). Any read or write here is a translation reading
// what the job does not have.
class JobMemory : public Memory {
public:
    uint32_t base() const override { return 0; }
    uint32_t size() const override { return 0; }
    uint8_t read8(uint32_t) const override { return touch(); }
    uint16_t read16(uint32_t) const override { return touch(); }
    uint32_t read32(uint32_t) const override { return touch(); }
    void write8(uint32_t, uint8_t) override { touch(); }
    void write16(uint32_t, uint16_t) override { touch(); }
    void write32(uint32_t, uint32_t) override { touch(); }
    const uint8_t* raw() const override { return nullptr; }
    uintptr_t fast_base() const override { return 0; }
    uint32_t rawmem_limit() const override { return 0; }
    // Only whether there is one: the code reaches the host's through
    // CpuState::page_table.
    uint8_t* const* page_table() const override { return page_table_ ? table_ : nullptr; }
    bool callbacks_preserve_registers() const override { return preserve_registers_; }

    bool page_table_ = false;
    bool preserve_registers_ = false;
    mutable bool touched_ = false;

private:
    uint8_t touch() const {
        touched_ = true;
        return 0;
    }
    uint8_t* table_[1] = {};
};

}  // namespace

std::vector<uint8_t> encode_settings(const RemoteJob::Settings& settings) {
    Writer w;
    write_settings(w, settings);
    return std::move(w.out);
}

std::vector<uint8_t> encode_job(const RemoteJob& job) {
    Writer w;
    w.out.insert(w.out.end(), kMagic, kMagic + 4);
    w.pod(kVersion);
    w.pod(uint32_t(sizeof(CpuState)));
    w.pod(job.pc);
    for (bool flag : {job.careful, job.page_table, job.preserve_registers, job.chain_ok, job.light}) w.flag(flag);
    write_settings(w, job.settings);
    w.pod(uint32_t(job.code.size()));
    for (const auto& [addr, value] : job.code) {
        w.pod(addr);
        w.pod(value);
    }
    w.pod(job.hint);
    return std::move(w.out);
}

bool decode_job(const uint8_t* bytes, size_t size, RemoteJob& job, std::string& why) {
    if (size < 4 || std::memcmp(bytes, kMagic, 4) != 0) {
        why = "no job";
        return false;
    }
    Reader r(bytes + 4, size - 4);
    uint32_t version = 0, cpu_size = 0, n = 0;
    if (!r.pod(version) || version != kVersion || !r.pod(cpu_size) || cpu_size != sizeof(CpuState)) {
        why = "a job of another build";
        return false;
    }
    bool ok = r.pod(job.pc) && r.flag(job.careful) && r.flag(job.page_table) && r.flag(job.preserve_registers) &&
              r.flag(job.chain_ok) && r.flag(job.light) && read_settings(r, job.settings) && r.pod(n);
    for (uint32_t i = 0; ok && i < n; ++i) {
        uint32_t addr;
        uint16_t value;
        ok = r.pod(addr) && r.pod(value);
        if (ok) job.code.emplace_back(addr, value);
    }
    if (!ok || !r.pod(job.hint) || !r.done()) {
        why = "a truncated job";
        return false;
    }
    return true;
}

bool Core::compile_job(uint32_t pc, bool careful, const CpuState& hint, const bcore_tier::CodeLog& code, bool light,
                       RemoteCode& out) {
    if (!translator_ && !init_llvm()) {
        out.why = "no LLVM";
        return false;
    }
    // As compile() compiles a promotion.
    bcore_tier::ReplayMemory replay(*mem_, code);
    BBTranslator::Tier tier;
    tier.level = 1;
    tier.code = &replay;
    auto result = translator_->translate(pc, &hint, careful, &tier);
    if (replay.missed() || !result.module) {
        out.why = replay.missed() ? "code the job has not" : "no translation";
        return false;
    }
    if (opt_level_ != 0) compiler_->optimize(*result.module, light);
    else lower_guest_memory(*result.module);
    for (const llvm::Function& function : *result.module)
        if (!function.isDeclaration()) out.name = function.getName().str();
    auto object = compiler_->emit(*result.module);
    result.module.reset();  // before its context
    result.context.reset();
    if (!object) {
        out.why = "the backend failed";
        return false;
    }
    out.object.assign(object->getBufferStart(), object->getBufferEnd());
    out.fallthrough_pc = result.fallthrough_pc;
#ifdef __EMSCRIPTEN__
    // Checked here, as CodeLoader::load checks an object: the first
    // instance takes a module made of it.
    out.why = CodeLoader::check(out.object.data(), out.object.size(), out.name);
    if (!out.why.empty()) return false;
#endif
    return true;
}

struct RemoteCompiler::Impl {
    JobMemory mem;
    struct Instance {
        CpuState cpu{};
        std::unique_ptr<Core> core;
    };
    std::map<std::vector<uint8_t>, std::unique_ptr<Instance>> cores;  // by settings
    // The job compiling: its PC and what the chain filter said, and whether
    // the translation asked about another PC (which the job cannot answer).
    uint32_t pc = 0;
    bool chain_ok = true;
    bool asked_other = false;
};

RemoteCompiler::RemoteCompiler() : impl_(std::make_unique<Impl>()) {}
RemoteCompiler::~RemoteCompiler() = default;

bool RemoteCompiler::compile(const uint8_t* bytes, size_t size, RemoteCode& out) {
    out = RemoteCode{};
    RemoteJob job;
    if (!decode_job(bytes, size, job, out.why)) return false;
    Impl& impl = *impl_;
    std::unique_ptr<Impl::Instance>& instance = impl.cores[encode_settings(job.settings)];
    if (!instance) {
        auto made = std::make_unique<Impl::Instance>();
        const RemoteJob::Settings& s = job.settings;
        // The two as the first instance's Core took them: init reads
        // whether there is a step limit, init_llvm (by the first job) the
        // translator's.
        made->cpu.steps_remaining = s.unlimited ? 0 : 1;
        made->core = std::make_unique<Core>(&made->cpu, &impl.mem);
        Core& core = *made->core;
        core.set_llvm_on_demand(true);  // LLVM made by the first job, with these
        core.set_chaining(s.chaining, s.dry);
        core.set_host_cycles(s.host_cycles);
        core.set_count_chained(s.count_chained);
        core.set_lazy_state(s.lazy_state);
        core.set_loop_blocks(s.loop_blocks);
        core.set_verify(s.verify);
        core.set_idle_waits(s.idle_waits);
        core.set_steady_skip(s.steady_skip);
        if (!core.set_post_pipeline(s.post_pipeline) || !core.init(s.opt_level, s.codegen_level)) {
            out.why = "no Core for the job's settings";
            impl.cores.erase(encode_settings(job.settings));
            return false;
        }
        made->cpu.steps_remaining = s.max_packets;
        core.set_chain_filter([&impl](uint32_t pc) {
            if (pc == impl.pc) return impl.chain_ok;
            impl.asked_other = true;
            return false;
        });
        instance = std::move(made);
    }
    impl.pc = job.pc;
    impl.chain_ok = job.chain_ok;
    impl.asked_other = false;
    impl.mem.page_table_ = job.page_table;
    impl.mem.preserve_registers_ = job.preserve_registers;
    impl.mem.touched_ = false;
    const bool ok = instance->core->compile_job(job.pc, job.careful, job.hint, job.code, job.light, out);
    if (ok && (impl.mem.touched_ || impl.asked_other)) {
        out = RemoteCode{};
        out.why = impl.mem.touched_ ? "a read the job cannot answer" : "the chain filter asked about another PC";
        return false;
    }
    return ok;
}

}  // namespace bcore
