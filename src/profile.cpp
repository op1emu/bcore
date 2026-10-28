// Implementation of the bcore JIT observability API (include/bcore_profile.h).
//
// When BCORE_ENABLE_PROFILE=0 this TU still provides stubs so the public API
// on Core always links; the instrumentation in Core::run() is compiled out
// separately (guarded in core.cpp) so a disabled build is unchanged.

#include "profile_state.h"

#include <algorithm>
#include <ctime>
#include <mutex>

#if BCORE_ENABLE_PROFILE && BCORE_PERF_JIT_EVENTS && defined(__linux__)
#define BCORE_CP_JITDUMP 1
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Needed unconditionally: ProfileState's destructor must see the complete
// llvm::JITEventListener type even when BCORE_ENABLE_PROFILE=0 (the unique_ptr
// members are simply never populated then).
#include <llvm/ExecutionEngine/JITEventListener.h>

#if BCORE_ENABLE_PROFILE

#include <charconv>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>

#include "jit_engine.h"

#endif

namespace bcore {

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
ProfileState::~ProfileState() {
#if BCORE_CP_JITDUMP
    if (cp_jitdump_marker) munmap(cp_jitdump_marker, size_t(sysconf(_SC_PAGESIZE)));
    if (cp_jitdump_fd >= 0) ::close(cp_jitdump_fd);
#endif
}

void ProfileState::clearBlocks() {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    const auto now = monotonic_ns();
    for (const auto& [addr, b] : blocks) retire(addr, b, now);
    blocks.clear();
    blocks_by_object.clear();
}

void ProfileState::recordBlock(uint64_t object_key, uint64_t addr, uint64_t size,
                               uint32_t guest_pc, uint32_t variant) {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    const auto now = history_capacity ? monotonic_ns() : 0;
    auto old = blocks.find(addr);
    if (old != blocks.end()) retire(addr, old->second, now);
    blocks[addr] = Block{size, guest_pc, variant, now};
    blocks_by_object[object_key].push_back(addr);
}

#if BCORE_CP_JITDUMP
namespace {
// The jitdump format (perf's Documentation/jitdump-specification.txt).
struct JitdumpHeader {
    uint32_t magic = 0x4A695444;  // "JiTD"
    uint32_t version = 1;
    uint32_t total_size = sizeof(JitdumpHeader);
    uint32_t elf_mach = 62;  // EM_X86_64
    uint32_t pad1 = 0;
    uint32_t pid = 0;
    uint64_t timestamp = 0;
    uint64_t flags = 0;
};
struct JitdumpCodeLoad {
    uint32_t id = 0;  // JIT_CODE_LOAD
    uint32_t total_size = 0;
    uint64_t timestamp = 0;
    uint32_t pid = 0, tid = 0;
    uint64_t vma = 0, code_addr = 0, code_size = 0, code_index = 0;
};

bool write_all(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        const ssize_t n = ::write(fd, p, size);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

// jit-<pid>.dump in a directory of its own under $JITDUMPDIR/.debug/jit (or
// $HOME's), as LLVM's writer places its own: perf finds a jitdump by that
// name when the process maps it executable (the marker).
int open_jitdump(void** marker) {
    const char* base = getenv("JITDUMPDIR");
    if (!base) base = getenv("HOME");
    if (!base) return -1;
    std::string dir = std::string(base) + "/.debug";
    mkdir(dir.c_str(), 0755);
    dir += "/jit";
    mkdir(dir.c_str(), 0755);
    std::string temp = dir + "/bcore-cp-XXXXXX";
    if (!mkdtemp(temp.data())) return -1;
    const std::string path = temp + "/jit-" + std::to_string(getpid()) + ".dump";
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0) return -1;
    JitdumpHeader header;
    header.pid = uint32_t(getpid());
    header.timestamp = monotonic_ns();
    if (!write_all(fd, &header, sizeof header)) {
        ::close(fd);
        return -1;
    }
    *marker = mmap(nullptr, size_t(sysconf(_SC_PAGESIZE)), PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (*marker == MAP_FAILED) {
        *marker = nullptr;
        ::close(fd);
        return -1;
    }
    return fd;
}
}  // namespace
#endif

void ProfileState::recordCodeLoad(const char* name, const void* code, uint64_t size) {
#if BCORE_CP_JITDUMP
    if (!want_perf_jitdump || cp_jitdump_fd == -2) return;
    if (cp_jitdump_fd < 0) {
        cp_jitdump_fd = open_jitdump(&cp_jitdump_marker);
        if (cp_jitdump_fd < 0) {
            cp_jitdump_fd = -2;
            return;
        }
    }
    const size_t name_size = strlen(name) + 1;
    JitdumpCodeLoad record;
    record.total_size = uint32_t(sizeof record + name_size + size);
    record.timestamp = monotonic_ns();
    record.pid = uint32_t(getpid());
    record.tid = uint32_t(syscall(SYS_gettid));
    record.vma = record.code_addr = reinterpret_cast<uintptr_t>(code);
    record.code_size = size;
    record.code_index = cp_jitdump_index++;
    if (!write_all(cp_jitdump_fd, &record, sizeof record) || !write_all(cp_jitdump_fd, name, name_size) ||
        !write_all(cp_jitdump_fd, code, size))
        cp_jitdump_fd = -2;
#else
    (void)name;
    (void)code;
    (void)size;
#endif
}

std::pair<uint64_t, std::vector<uint8_t>> ProfileState::takeObjectShape(uint32_t guest_pc) {
    std::lock_guard<std::mutex> lk(blocks_mutex);
    auto it = object_shapes.find(guest_pc);
    if (it == object_shapes.end()) return {0, {}};
    auto shape = std::move(it->second);
    object_shapes.erase(it);
    // A split block's code is both parts: the function, then its cold part.
    auto cold = cold_shapes.find(guest_pc);
    if (cold != cold_shapes.end()) {
        shape.first += cold->second.first;
        shape.second.insert(shape.second.end(), cold->second.second.begin(), cold->second.second.end());
        cold_shapes.erase(cold);
    }
    return shape;
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

// Parses "bb_0x%08x" (the name JitEngine gives every block function),
// "bb_0x%08x.careful" (a block compiled again after its loop-bottom guard
// failed), either with ".t1" (a tier-0 block compiled again at tier 1,
// Core::set_tiering), and any of these with ".cold", the symbol the machine
// function splitter gives the block's cold part (cold set), each also with
// Mach-O's global prefix "_". Returns false for any other symbol.
bool parse_bb_name(llvm::StringRef name, uint32_t& guest_pc, bool& cold) {
    if (name.startswith("_bb_0x")) name = name.drop_front();
    if (!name.startswith("bb_0x")) return false;
    llvm::StringRef hex = name.drop_front(5);
    cold = hex.consume_back(".cold");
    hex.consume_back(".t1");
    hex.consume_back(".careful");
    if (hex.empty() || hex.size() > 8) return false;
    uint32_t pc = 0;
    const char* b = hex.data();
    const char* e = b + hex.size();
    auto res = std::from_chars(b, e, pc, 16);
    if (res.ec != std::errc() || res.ptr != e) return false;
    guest_pc = pc;
    return true;
}

uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

} // namespace

// The block function's bytes as the compiler emitted them (relocated fields
// still zero) and its relocations, then every section those relocations
// point into (constant pools, jump tables, tables of function pointers) with
// its own relocations, transitively: identical for identical code whatever
// addresses the JIT and the helpers land at in this process. A relocation
// names only a symbol and an offset, and a relocated field is zero before
// relocation, so without the referenced bytes two blocks differing in a
// constant would look alike, and without their relocations two blocks
// whose tables point at different functions would.
std::vector<uint8_t> object_code_material(const llvm::object::ObjectFile& obj, const llvm::object::SectionRef& sec,
                                          uint64_t offset, uint64_t size) {
    std::vector<uint8_t> out;
    auto append = [&out](const void* data, size_t n) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + n);
    };
    auto append_value = [&append](uint64_t value) { append(&value, sizeof(value)); };
    auto contents = sec.getContents();
    if (!contents || offset + size > contents->size()) {
        if (!contents) llvm::consumeError(contents.takeError());
        return {};
    }
    append(contents->data() + offset, size);

    // Sections in the order first referenced; the function's own section is
    // covered by its byte range above.
    std::vector<llvm::object::SectionRef> sections{sec};
    // Appends the relocations applied to [begin, end) of `target`, queueing
    // the sections they point into.
    auto relocations = [&](const llvm::object::SectionRef& target, uint64_t begin, uint64_t end) {
        for (const llvm::object::SectionRef& rsec : obj.sections()) {
            auto relocated = rsec.getRelocatedSection();
            if (!relocated) { llvm::consumeError(relocated.takeError()); continue; }
            if (*relocated != target) continue;
            for (const llvm::object::RelocationRef& reloc : rsec.relocations()) {
                const uint64_t at = reloc.getOffset();
                if (at < begin || at >= end) continue;
                append_value(at - begin);
                append_value(reloc.getType());
                auto symbol = reloc.getSymbol();
                if (symbol != obj.symbol_end()) {
                    auto name = symbol->getName();
                    if (name) append(name->data(), name->size());
                    else llvm::consumeError(name.takeError());
                    append_value(0);  // separator
                    auto in = symbol->getSection();
                    if (!in) {
                        llvm::consumeError(in.takeError());
                    } else if (*in != obj.section_end()) {
                        auto known = std::find(sections.begin(), sections.end(), **in);
                        // Which section, by order of first reference: section
                        // symbols have empty names.
                        append_value(static_cast<uint64_t>(known - sections.begin()));
                        if (known == sections.end()) sections.push_back(**in);
                    }
                }
                if (llvm::isa<llvm::object::ELFObjectFileBase>(&obj)) {
                    auto addend = llvm::object::ELFRelocationRef(reloc).getAddend();
                    if (addend) append_value(static_cast<uint64_t>(*addend));
                    else llvm::consumeError(addend.takeError());
                }
            }
        }
    };
    relocations(sec, offset, offset + size);
    for (size_t i = 1; i < sections.size(); ++i) {  // grows while it runs
        const llvm::object::SectionRef data = sections[i];
        append_value(data.getSize());
        auto bytes = data.getContents();
        if (bytes) append(bytes->data(), bytes->size());  // empty for zero-fill (bss)
        else llvm::consumeError(bytes.takeError());
        relocations(data, 0, data.getSize());
    }
    return out;
}

uint64_t hash_material(const std::vector<uint8_t>& material) {
    if (material.empty()) return 0;
    const uint64_t hash = fnv1a(0xcbf29ce484222325ull, material.data(), material.size());
    return hash ? hash : 1;  // 0 means unknown
}

namespace {

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
            bool cold = false;
            if (!parse_bb_name(name_or.get(), guest_pc, cold)) continue;

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
            if (state_.want_object_shapes) {
                // Inside materialization: only copy what the hash needs here;
                // Core hashes it after the translation is timed.
                const uint64_t begin = monotonic_ns();
                auto material = object_code_material(obj, sec, *addr_or - sec.getAddress(), size);
                std::lock_guard<std::mutex> lock(state_.blocks_mutex);
                (cold ? state_.cold_shapes : state_.object_shapes)[guest_pc] = {size, std::move(material)};
                state_.capture_ns += monotonic_ns() - begin;
            }
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
void detach_perf_listener(ProfileState&, JitEngine&) {}
#endif

#else // !BCORE_ENABLE_PROFILE

bool attach_listeners(ProfileState&, JitEngine&) { return false; }
bool attach_perf_listener(ProfileState&, JitEngine&) { return false; }
void detach_perf_listener(ProfileState&, JitEngine&) {}

#endif

} // namespace bcore_profile

}  // namespace bcore
