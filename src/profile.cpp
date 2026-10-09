// Implementation of the bcore JIT observability API (include/bcore_profile.h).
//
// When BCORE_ENABLE_PROFILE=0 this TU still provides stubs so the public API
// on Core always links; the instrumentation in Core::run() is compiled out
// separately (guarded in core.cpp) so a disabled build is unchanged.

#include "profile_state.h"

#include <algorithm>
#include <ctime>
#include <mutex>

// Needed unconditionally: ProfileState's destructor must see the complete
// llvm::JITEventListener type even when BCORE_ENABLE_PROFILE=0 (the unique_ptr
// members are simply never populated then).
#include <llvm/ExecutionEngine/JITEventListener.h>

#if BCORE_ENABLE_PROFILE

#include <charconv>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>

#include "jit_engine.h"

#endif

namespace bcore_profile {

uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// ProfileState
// ---------------------------------------------------------------------------

ProfileState::ProfileState() = default;
// Out of line: destroying map_listener needs the complete listener type.
ProfileState::~ProfileState() = default;

void ProfileState::clearBlocks() {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    const auto now = monotonic_ns();
    for (const auto& [addr, b] : blocks) retire(addr, b, now);
    blocks.clear();
    blocks_by_object.clear();
}

void ProfileState::recordBlock(uint64_t object_key, uint64_t addr, uint64_t size,
                               uint32_t guest_pc) {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    const auto now = history_capacity ? monotonic_ns() : 0;
    auto old = blocks.find(addr);
    if (old != blocks.end()) retire(addr, old->second, now);
    blocks[addr] = Block{size, guest_pc, /*variant=*/0, now};
    blocks_by_object[object_key].push_back(addr);
}

void ProfileState::freeObject(uint64_t object_key) {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    auto it = blocks_by_object.find(object_key);
    if (it == blocks_by_object.end()) return;
    const auto now = history_capacity ? monotonic_ns() : 0;
    for (uint64_t addr : it->second) {
        auto b = blocks.find(addr);
        if (b != blocks.end()) {
            retire(addr, b->second, now);
            blocks.erase(b);
        }
    }
    blocks_by_object.erase(it);
}

void ProfileState::retire(uint64_t addr, const Block& b, uint64_t now) {
    if (!history_capacity) return;
    if (retired.size() >= history_capacity) { ++history_dropped; return; }
    retired.push_back({reinterpret_cast<const void*>(addr), b.size,
                       b.guest_pc, b.variant, b.loaded_ns, now});
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
            if (!name_or) { llvm::consumeError(name_or.takeError()); continue; }
            uint32_t guest_pc = 0;
            if (!parse_bb_name(name_or.get(), guest_pc)) continue;

            auto sect_or = sym.getSection();
            auto addr_or = sym.getAddress();
            if (!sect_or || !addr_or) {
                if (!sect_or) llvm::consumeError(sect_or.takeError());
                if (!addr_or) llvm::consumeError(addr_or.takeError());
                continue;
            }
            if (*sect_or == obj.section_end()) continue;
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
    state.perf_attached = false;
    state.map_listener = std::make_unique<BlockMapListener>(state);
    if (!jit.registerJITEventListener(*state.map_listener)) {
        // Linking layer is not RTDyldObjectLinkingLayer-based: the code map
        // stays empty and the iteration APIs fall back to the lookup-derived
        // address table without sizes. Hosts see this through
        // profileHasBlockSizes(); bcore prints nothing.
        state.map_listener.reset();
        return false;
    }

#if BCORE_PERF_JIT_EVENTS
    if (state.want_perf_jitdump)
        attach_perf_listener(state, jit);
#endif
    return true;
}

#if BCORE_PERF_JIT_EVENTS
bool attach_perf_listener(ProfileState& state, JitEngine& jit) {
    if (!state.want_perf_jitdump) return false;
    if (state.perf_attached) return true;
    // LLVM's own jitdump writer: gives `perf inject --jit` support for free.
    // nullptr when LLVM was built without LLVM_USE_PERF.
    if (!state.perf_listener)
        state.perf_listener = llvm::JITEventListener::createPerfJITEventListener();
    if (!state.perf_listener) return false;
    // LLVM does not deduplicate registrations. The per-engine flag above
    // prevents duplicate notifications; attach_listeners resets it on rebuild.
    state.perf_attached = jit.registerJITEventListener(*state.perf_listener);
    return state.perf_attached;
}

void detach_perf_listener(ProfileState& state, JitEngine& jit) {
    if (!state.perf_attached) return;
    jit.unregisterJITEventListener(*state.perf_listener);
    state.perf_attached = false;
}
#else
bool attach_perf_listener(ProfileState&, JitEngine&) { return false; }
#endif

#else // !BCORE_ENABLE_PROFILE

bool attach_listeners(ProfileState&, JitEngine&) { return false; }
bool attach_perf_listener(ProfileState&, JitEngine&) { return false; }
void detach_perf_listener(ProfileState&, JitEngine&) {}

#endif

} // namespace bcore_profile
