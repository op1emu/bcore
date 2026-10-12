// Regression test for the JIT observability API (include/bcore_profile.h).
// Builds with BCORE_ENABLE_PROFILE on or off and checks the matching contract.
#include "core.h"
#include "mmr.h"
#if BCORE_ENABLE_PROFILE
#include "profile_state.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

class Ram : public Memory {
public:
    std::vector<uint8_t> bytes = std::vector<uint8_t>(4096);
    uint32_t base() const override { return 0; }
    uint32_t size() const override { return bytes.size(); }
    uint8_t read8(uint32_t a) const override { return bytes.at(a); }
    uint16_t read16(uint32_t a) const override { return read8(a) | uint16_t(read8(a + 1)) << 8; }
    uint32_t read32(uint32_t a) const override { return read16(a) | uint32_t(read16(a + 2)) << 16; }
    void write8(uint32_t a, uint8_t v) override { bytes.at(a) = v; }
    void write16(uint32_t a, uint16_t v) override { write8(a, v); write8(a + 1, v >> 8); }
    void write32(uint32_t a, uint32_t v) override { write16(a, v); write16(a + 2, v >> 16); }
    const uint8_t* raw() const override { return bytes.data(); }
    uintptr_t fast_base() const override { return reinterpret_cast<uintptr_t>(bytes.data()); }
};

struct Sink : BcoreEventSink {
    int begins = 0, ends = 0, failures = 0;
    std::vector<std::string> stages;
    uint64_t last_end = 0;
    void onTranslateBegin(uint32_t) override { ++begins; }
    void onTranslateEnd(uint32_t, uint64_t) override { ++ends; }
    void onTranslateFailure(uint32_t, uint64_t) override { ++failures; }
    void onCompileStage(uint32_t, const char* stage, uint64_t b, uint64_t e, bool ok) override {
        require(e >= b && ok, "stage is a successful interval");
        require(b >= last_end, "stages are in order and do not overlap");
        last_end = e;
        stages.push_back(stage);
    }
};

// A sink that takes its time must not be charged to the translation.
struct SlowSink : BcoreEventSink {
    void onCompileStage(uint32_t, const char*, uint64_t, uint64_t, bool) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
};

// A sink slow to answer wantsCompileMetrics, or to take the metrics, is
// charged to no translation either.
struct SlowMetricsSink : BcoreEventSink {
    uint64_t elapsed = 0, collect = 0;
    bool wantsCompileMetrics() const override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return true;
    }
    void onCompileMetrics(uint32_t, const BcoreCompileMetrics& m) override {
        collect = m.collect_ns;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    void onTranslateEnd(uint32_t, uint64_t ns) override { elapsed = ns; }
};

static void check_sink_time_excluded() {
#if BCORE_ENABLE_PROFILE
    Ram mem;
    mem.write16(0, 0x2000);
    CpuState cpu{};
    Core core(&cpu, &mem);
    require(core.init(1), "init");
    SlowSink sink;
    core.setEventSink(&sink);
    require(core.run(0), "dispatch with a slow sink");
    // Three stage callbacks sleep 600 ms in all; one tiny block compiles in
    // a few ms.
    require(core.stats().translate_ns_total < 300000000ull, "stage callbacks are outside translate_ns");
    core.setEventSink(nullptr);
    Core metrics_core(&cpu, &mem);
    require(metrics_core.init(1), "init");
    SlowMetricsSink slow_metrics;
    metrics_core.setEventSink(&slow_metrics);
    require(metrics_core.run(0), "dispatch with a slow metrics sink");
    require(metrics_core.stats().translate_ns_total < 150000000ull && slow_metrics.elapsed < 150000000ull,
            "asking for and taking metrics is outside the translation's time");
    require(slow_metrics.collect > 0, "collection time reported");
    metrics_core.setEventSink(nullptr);
#endif
}

extern "C" uint32_t cec_return_rti(CpuState* cpu);

// Interrupt entries are counted per level where the CEC dispatches them.
static void check_interrupt_counts() {
    Ram mem;
    mem.write16(0, 0x2000);
    CpuState cpu{};
    Core core(&cpu, &mem);
    require(core.init(0), "init");
    cec_init();  // out of reset: IPEND = RST | IRPTEN
    const BcoreStats before = core.stats();
    for (uint64_t count : before.interrupts_entered) require(count == 0, "no entries after reset");
    cec_return_rti(&cpu);              // leave reset for user mode
    cec_exception(&cpu, 1, nullptr);   // EXCPT 1: dispatched to EVX
#if BCORE_ENABLE_PROFILE
    const BcoreStats after = core.stats();
    require(after.interrupts_entered[3] == 1, "the exception is counted at level 3 (EVX)");
    uint64_t total = 0;
    for (uint64_t count : after.interrupts_entered) total += count;
    require(total == 1, "and nowhere else");
#else
    for (uint64_t count : core.stats().interrupts_entered) require(count == 0, "disabled build counts nothing");
#endif
}

struct MetricsSink : BcoreEventSink {
    bool want = true;
    std::vector<BcoreCompileMetrics> metrics;
    bool wantsCompileMetrics() const override { return want; }
    void onCompileMetrics(uint32_t, const BcoreCompileMetrics& m) override { metrics.push_back(m); }
};

// Compile metrics describe the code as compiled, the same for the same code.
static BcoreCompileMetrics compile_once(int opt_level, bool want = true) {
    Ram mem;
    for (uint32_t a = 0; a < 8; a += 2) mem.write16(a, 0x0000);  // NOP x4
    mem.write16(8, 0x2000 | ((-4) & 0xfff));                      // JUMP.S 0
    CpuState cpu{};
    Core core(&cpu, &mem);
    require(core.init(opt_level), "init");
    MetricsSink sink;
    sink.want = want;
    core.setEventSink(&sink);
    require(core.run(0), "dispatch");
    core.setEventSink(nullptr);
    if (!want) {
        require(sink.metrics.empty(), "no metrics unless asked for");
        return {};
    }
#if BCORE_ENABLE_PROFILE
    require(sink.metrics.size() == 1, "one set of metrics per translation");
    const auto m = sink.metrics[0];
    require(m.ir_instructions > 0 && m.ir_blocks > 0, "IR counted");
    std::vector<BcoreBlockInfo> live;
    core.forEachCompiledBlock([&](const BcoreBlockInfo& b) { live.push_back(b); });
    require(live.size() == 1 && m.object_bytes == live[0].host_size, "object bytes are the block's code size");
    require(m.object_hash != 0, "code hashed");
    return m;
#else
    require(sink.metrics.empty(), "disabled build reports no metrics");
    return {};
#endif
}

#if BCORE_ENABLE_PROFILE
// Compiles a block function that reads a private constant table, as the
// compiler places constant pools and jump tables in their own sections, and
// returns the hash of the function.
static uint64_t hash_of_table_block(int first) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    const std::string ir =
        "@t = private unnamed_addr constant [4 x i32] [i32 " + std::to_string(first) +
        ", i32 7, i32 8, i32 9]\n"
        "define i32 @bb_0x00000000(i32 %i) {\n"
        "  %p = getelementptr [4 x i32], ptr @t, i32 0, i32 %i\n"
        "  %v = load i32, ptr %p\n"
        "  ret i32 %v\n}\n";
    llvm::LLVMContext context;
    llvm::SMDiagnostic diag;
    auto module = llvm::parseIR(llvm::MemoryBufferRef(ir, "table"), diag, context);
    require(module != nullptr, "test IR parses");
    const std::string triple = llvm::sys::getProcessTriple();
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    require(target != nullptr, "native target");
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, "generic", "", {}, llvm::Reloc::Static));
    module->setDataLayout(machine->createDataLayout());
    llvm::SmallVector<char, 0> object;
    llvm::raw_svector_ostream stream(object);
    llvm::legacy::PassManager passes;
    require(!machine->addPassesToEmitFile(passes, stream, nullptr, llvm::CGFT_ObjectFile), "emit object");
    passes.run(*module);
    auto file = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(llvm::StringRef(object.data(), object.size()), "table.o"));
    require(static_cast<bool>(file), "object parses");
    for (const auto& [symbol, size] : llvm::object::computeSymbolSizes(**file)) {
        auto name = symbol.getName();
        auto section = symbol.getSection();
        auto address = symbol.getAddress();
        if (!name || !section || !address || *name != "bb_0x00000000") continue;
        return bcore_profile::hash_material(
            bcore_profile::object_code_material(**file, **section, *address - (*section)->getAddress(), size));
    }
    require(false, "block function found");
    return 0;
}
#endif

static void check_compile_metrics() {
#if BCORE_ENABLE_PROFILE
    // The function's bytes and relocations are identical; only the table it
    // reads differs. A different constant is different code.
    require(hash_of_table_block(1) == hash_of_table_block(1), "the same table hashes the same");
    require(hash_of_table_block(1) != hash_of_table_block(2), "a different constant table is different code");
#endif
    compile_once(1, /*want=*/false);
#if BCORE_ENABLE_PROFILE
    const auto o0 = compile_once(0), o0_again = compile_once(0), o1 = compile_once(1);
    require(o0.object_hash == o0_again.object_hash && o0.ir_instructions == o0_again.ir_instructions,
            "the same code hashes the same in a new engine");
    require(o0.ir_instructions != o1.ir_instructions && o0.object_hash != o1.object_hash,
            "the IR pipeline changes the compiled code");

#else
    compile_once(0);
#endif
}

static std::vector<BcoreBlockInfo> blocks(const Core& core) {
    std::vector<BcoreBlockInfo> out;
    core.forEachCompiledBlock([&](const BcoreBlockInfo& b) { out.push_back(b); });
    return out;
}

static void check(int opt_level) {
    Ram mem;
    mem.write16(0, 0x2000);  // JUMP.S 0: a one-packet self loop at PC 0
    CpuState cpu{};
    Core core(&cpu, &mem);
    require(core.init(opt_level), "init");
    Sink sink;
    core.setEventSink(&sink);
    core.setProfileHistory(1);
    for (int i = 0; i < 10; ++i) require(core.run(0), "dispatch");

#if BCORE_ENABLE_PROFILE
    const std::vector<std::string> expected = opt_level
        ? std::vector<std::string>{"lift", "ir-optimize", "materialize"}
        : std::vector<std::string>{"lift", "materialize"};
    require(sink.begins == 1 && sink.ends == 1 && sink.failures == 0, "one translation, balanced");
    require(sink.stages == expected, "compile stages");
    const BcoreStats stats = core.stats();
    require(stats.blocks_translated == 1 && stats.cache_misses == 1, "one miss");
    require(stats.cache_hits == 9 && stats.blocks_executed == 10, "hits and executions counted");
    require(stats.translate_ns_total > 0, "translation time recorded");

    require(core.profileHasBlockSizes(), "RTDyld listener gives exact sizes");
    auto live = blocks(core);
    require(live.size() == 1 && live[0].guest_pc == 0 && live[0].host_size > 0, "code map has the block");
    require(live[0].loaded_ns > 0 && live[0].unloaded_ns == 0, "live block with a load time");

    // invalidate() retires the old code with an end time and maps new code.
    core.invalidate();
    require(blocks(core).empty(), "invalidate empties the live map");
    require(core.run(0), "dispatch after invalidate");
    require(blocks(core).size() == 1, "new engine's block is mapped");
    std::vector<BcoreBlockInfo> history;
    core.forEachProfileBlock([&](const BcoreBlockInfo& b) { history.push_back(b); });
    require(history.size() == 2, "one retired and one live range");
    require(history[0].unloaded_ns >= history[0].loaded_ns && history[0].unloaded_ns > 0, "retired range closed");
    require(core.profileHistoryDropped() == 0, "history within capacity");
    core.invalidate();
    require(core.profileHistoryDropped() == 1, "overflow is counted, not reattributed");

    // A translation that throws (Ram reads are bounds-checked) still pairs
    // its begin with a failure, and the exception reaches the caller.
    bool threw = false;
    try { core.run(0x2000); } catch (const std::out_of_range&) { threw = true; }
    require(threw, "decoding outside Ram throws");
    require(sink.failures == 1 && sink.begins == sink.ends + sink.failures, "begin balanced on unwind");

#else
    require(sink.begins == 0 && sink.stages.empty(), "disabled build fires no events");
    require(core.stats().blocks_executed == 0, "disabled build counts nothing");
    require(!core.profileHasBlockSizes(), "disabled build has no sizes");
    auto live = blocks(core);
    require(live.size() == 1 && live[0].host_size == 0, "lookup fallback without sizes");
    std::vector<BcoreBlockInfo> history;
    core.forEachProfileBlock([&](const BcoreBlockInfo& b) { history.push_back(b); });
    require(history.size() == 1 && history[0].host_size == 0, "profile blocks use the same fallback");
#endif
    core.setEventSink(nullptr);
}

#if BCORE_ENABLE_PROFILE && BCORE_PERF_JIT_EVENTS
// Names of the JIT_CODE_LOAD records (id 0) in this process's jitdump file.
static std::vector<std::string> jitdump_loads(const std::filesystem::path& dir) {
    const std::string file = "jit-" + std::to_string(getpid()) + ".dump";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().filename() != file) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, data.data() + at, 4); return v; };
        std::vector<std::string> loads;
        for (size_t at = u32(8); at + 16 <= data.size();) {  // header size, then records
            const uint32_t id = u32(at), size = u32(at + 4);
            if (size < 16 || at + size > data.size()) break;
            if (id == 0) loads.emplace_back(data.c_str() + at + 56);  // fixed fields, then the name
            at += size;
        }
        return loads;
    }
    return {};
}

// LLVM's perf listener is a process-wide singleton writing one file, so this
// runs before anything else enables it.
static void check_jitdump() {
    char dir[] = "/tmp/bcore-jitdump-XXXXXX";
    require(mkdtemp(dir) != nullptr, "temporary jitdump directory");
    setenv("JITDUMPDIR", dir, 1);
    Ram mem;
    mem.write16(0, 0x2000);  // JUMP.S 0 at 0
    mem.write16(4, 0x2000);  // and at 4
    CpuState cpu{};
    Core core(&cpu, &mem);
    require(core.init(0), "init");
    core.set_perf_jitdump(true);
    core.set_perf_jitdump(true);  // repeated enable must not duplicate records
    require(core.run(0), "dispatch with jitdump");
    core.invalidate();            // the new engine must get the listener too
    require(core.run(0), "dispatch after invalidate");
    core.set_perf_jitdump(false); // detaches from the current engine at once
    require(core.run(4), "dispatch after disable");
    const auto loads = jitdump_loads(dir);
    require(loads == std::vector<std::string>{"bb_0x00000000", "bb_0x00000000"},
            "one record per compiled block, none after disable");
    std::filesystem::remove_all(dir);
}
#endif

int main() {
#if BCORE_ENABLE_PROFILE && BCORE_PERF_JIT_EVENTS
    check_jitdump();
#endif
    check(0);
    check(1);
    check_sink_time_excluded();
    check_interrupt_counts();
    check_compile_metrics();
    std::puts("bcore profile test passed");
}
