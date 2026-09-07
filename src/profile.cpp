// Implementation of the bcore JIT observability API (include/bcore_profile.h).
//
// When BCORE_ENABLE_PROFILE=0 this TU still provides stubs so the public API
// on Core always links; the probe branch in Core::run() is compiled out
// separately (guarded in core.cpp) so a disabled build is unchanged.

#include "profile_state.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

// Needed unconditionally: ProfileState's destructor must see the complete
// llvm::JITEventListener type even when BCORE_ENABLE_PROFILE=0 (the unique_ptr
// members are simply never populated then).
#include <llvm/ExecutionEngine/JITEventListener.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if BCORE_ENABLE_PROFILE

#include <charconv>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Support/raw_ostream.h>

#include "jit_engine.h"

#endif

namespace bcore_profile {

// ---------------------------------------------------------------------------
// TSC busy-wait (block cost probe)
// ---------------------------------------------------------------------------

namespace {

#if defined(__x86_64__) || defined(__i386__)
inline uint64_t read_tsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
#endif

} // namespace

uint64_t tsc_hz() {
    static std::once_flag once;
    static uint64_t hz = 0;
    std::call_once(once, [] {
#if defined(__x86_64__) || defined(__i386__)
        // Median of 3 short calibration windows against steady_clock.
        // sleeping here is fine — this is calibration, not measurement.
        uint64_t samples[3];
        for (int i = 0; i < 3; i++) {
            const auto t0 = std::chrono::steady_clock::now();
            const uint64_t c0 = read_tsc();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto t1 = std::chrono::steady_clock::now();
            const uint64_t c1 = read_tsc();
            const double sec = std::chrono::duration<double>(t1 - t0).count();
            samples[i] = static_cast<uint64_t>(static_cast<double>(c1 - c0) / sec);
        }
        std::sort(samples, samples + 3);
        hz = samples[1];
#else
        hz = 0;  // spin_ns() falls back to a clock_gettime loop
#endif
    });
    return hz;
}

void spin_ns(uint64_t ns) {
#if defined(__x86_64__) || defined(__i386__)
    const uint64_t hz = tsc_hz();
    if (hz != 0) {
        const uint64_t start = read_tsc();
        const uint64_t delta =
            static_cast<uint64_t>((static_cast<unsigned __int128>(ns) * hz) / 1000000000u);
        while (read_tsc() - start < delta)
            _mm_pause();
        return;
    }
#endif
    // Fallback: CLOCK_MONOTONIC spin (still never sleeps/yields).
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < static_cast<int64_t>(ns)) {
    }
}

// ---------------------------------------------------------------------------
// ProfileState
// ---------------------------------------------------------------------------

ProfileState::ProfileState() = default;

#if BCORE_ENABLE_PROFILE
// Listener holds a back-reference; defined below. Destruction is fine from
// here because profile.cpp has the complete type.
ProfileState::~ProfileState() = default;
#else
ProfileState::~ProfileState() = default;
#endif

void ProfileState::clearBlocks() {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    blocks.clear();
    blocks_by_object.clear();
}

void ProfileState::recordBlock(uint64_t object_key, uint64_t addr, uint64_t size,
                               uint32_t guest_pc) {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    blocks[addr] = Block{size, guest_pc, /*variant=*/0};
    blocks_by_object[object_key].push_back(addr);
}

void ProfileState::freeObject(uint64_t object_key) {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    auto it = blocks_by_object.find(object_key);
    if (it == blocks_by_object.end()) return;
    for (uint64_t addr : it->second)
        blocks.erase(addr);
    blocks_by_object.erase(it);
}

#if BCORE_ENABLE_PROFILE

// ---------------------------------------------------------------------------
// Code map listener
// ---------------------------------------------------------------------------

namespace {

// Parses "bb_0x%08x" (the name JitEngine gives every block function).
// Returns false for any other symbol.
bool parse_bb_name(llvm::StringRef name, uint32_t& guest_pc) {
    if (!name.startswith("bb_0x")) return false;
    llvm::StringRef hex = name.drop_front(5);
    if (hex.empty() || hex.size() > 8) return false;
    uint32_t pc = 0;
    const char* b = hex.data();
    const char* e = b + hex.size();
    auto res = std::from_chars(b, e, pc, 16);
    if (res.ec != std::errc() || res.ptr != e) return false;
    guest_pc = pc;
    return true;
}

// Reconstructs guest attribution for every compiled block from the JIT's own
// object files. Exact sizes come from the object-file symbol table via
// llvm::object::computeSymbolSizes — no "nearest preceding block" heuristic,
// so a sample inside a block is always attributed to the right block.
class BlockMapListener : public llvm::JITEventListener {
public:
    explicit BlockMapListener(ProfileState& state) : state_(state) {}

    void notifyObjectLoaded(ObjectKey key, const llvm::object::ObjectFile& obj,
                            const llvm::RuntimeDyld::LoadedObjectInfo& li) override {
        for (const auto& entry : llvm::object::computeSymbolSizes(obj)) {
            const llvm::object::SymbolRef& sym = entry.first;
            const uint64_t size = entry.second;

            auto name_or = sym.getName();
            if (!name_or) continue;
            uint32_t guest_pc = 0;
            if (!parse_bb_name(name_or.get(), guest_pc)) continue;

            auto sect_or = sym.getSection();
            auto addr_or = sym.getAddress();
            if (!sect_or || !addr_or) continue;
            const llvm::object::SectionRef sec = **sect_or;
            const uint64_t sec_load = li.getSectionLoadAddress(sec);
            if (sec_load == 0) continue;  // section not loaded (e.g. debug)

            const uint64_t host_addr = sec_load + (*addr_or - sec.getAddress());
            state_.recordBlock(static_cast<uint64_t>(key), host_addr, size, guest_pc);
        }
    }

    void notifyFreeingObject(ObjectKey key) override {
        state_.freeObject(static_cast<uint64_t>(key));
    }

private:
    ProfileState& state_;
};

} // namespace

bool attach_listeners(ProfileState& state, JitEngine& jit) {
    state.map_listener = std::make_unique<BlockMapListener>(state);
    if (!jit.registerJITEventListener(*state.map_listener)) {
        // Linking layer is not RTDyldObjectLinkingLayer-based: the code map
        // stays empty and Core::forEachCompiledBlock() falls back to the
        // lookup-derived address table without sizes.
        llvm::errs() << "bcore: object linking layer does not support JIT event "
                        "listeners; exact block sizes unavailable\n";
        state.map_listener.reset();
        return false;
    }

#if BCORE_PERF_JIT_EVENTS
    if (state.want_perf_jitdump) {
        // LLVM's own jitdump writer: gives `perf inject --jit` support for
        // free. nullptr when LLVM was built without LLVM_USE_PERF.
        // Created once but (re)registered on EVERY engine: Core::invalidate()
        // replaces the JitEngine, and a listener left on the old engine would
        // silently stop recording all blocks compiled after invalidation.
        if (!state.perf_listener)
            state.perf_listener.reset(
                llvm::JITEventListener::createPerfJITEventListener());
        if (state.perf_listener)
            jit.registerJITEventListener(*state.perf_listener);
    }
#endif
    return true;
}

#else // !BCORE_ENABLE_PROFILE

bool attach_listeners(ProfileState&, JitEngine&) { return false; }

#endif

} // namespace bcore_profile
