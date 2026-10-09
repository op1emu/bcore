// Regression test for the JIT observability API (include/bcore_profile.h).
// Builds with BCORE_ENABLE_PROFILE on or off and checks the matching contract.
#include "core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
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
    std::puts("bcore profile test passed");
}
