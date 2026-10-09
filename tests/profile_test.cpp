// Regression test for the JIT observability API (include/bcore_profile.h).
// Builds with BCORE_ENABLE_PROFILE on or off and checks the matching contract.
#include "core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

    // jitdump can be enabled repeatedly and survives invalidate (no-op unless
    // built with BCORE_PERF_JIT_EVENTS).
    core.set_perf_jitdump(true);
    core.set_perf_jitdump(true);
    core.invalidate();
    require(core.run(0), "dispatch with jitdump enabled");
#else
    require(sink.begins == 0 && sink.stages.empty(), "disabled build fires no events");
    require(core.stats().blocks_executed == 0, "disabled build counts nothing");
    require(!core.profileHasBlockSizes(), "disabled build has no sizes");
    auto live = blocks(core);
    require(live.size() == 1 && live[0].host_size == 0, "lookup fallback without sizes");
#endif
    core.setEventSink(nullptr);
}

int main() {
    check(0);
    check(1);
    std::puts("bcore profile test passed");
}
