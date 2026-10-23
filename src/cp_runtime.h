#pragma once

// Stencils built at run time (Core::set_copy_patch_runtime): the copy-and-
// patch tier 0's for packets the build left without one (cp_translator.h).
// A packet with none (a parallel one, a 32-bit instruction outside
// cp_patterns.def, a 16-bit encoding whose build-time code was refused) is
// lifted as tools/stencilgen.cpp lifts every packet it builds
// (BBTranslator::translate_stencil, at kPcA, cp_build.h), compiled for the
// stencils' target, and kept for the process by its encoding: holes for its
// PC and packet index only, so one stencil serves the packet wherever it
// is. A packet that cannot be a stencil is kept too, as one without; its
// blocks compile with LLVM.
//
// Checks, as the build's: the lift reads only the packet's halfwords and
// keeps its length (or ends the block, as an illegal instruction does), its
// IR is the same lifted at another PC (kPcB), and every relocation of its
// code is a hole (extract()). Single-threaded, like CpTranslator.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cp_build.h"
#include "cp_format.h"

class Compiler;

namespace llvm {
class LLVMContext;
}

namespace bcore_cp {

class RuntimeStencils {
public:
    struct Settings {
        // The stencils' IR pipeline and backend (Compiler::init in stencil
        // mode): 0 compiles the fastest; the build's are 2.
        int opt = 0;
        // Each new stencil compiled again on its own: the same code and
        // relocations as from its batch, or it is refused (for tests).
        bool verify = false;
        // Only packets whose key_hash lies in [range_lo, range_hi] get
        // stencils (bisecting a wrong one); the others none.
        uint64_t range_lo = 0, range_hi = ~uint64_t(0);
        // At most this many packets kept; past it the others get none.
        size_t capacity = 1 << 16;
    };
    // A packet: its halfwords and their number (1, 2 or 4).
    struct Packet {
        uint16_t words[4] = {};
        uint8_t count = 0;
        uint64_t key() const;
    };
    // A stencil as cp_format.h's: its header (code and relocs 0), its code
    // and relocations.
    struct Record {
        Stencil stencil{};
        std::vector<uint8_t> code;
        std::vector<Reloc> relocs;
    };
    struct Stats {
        uint64_t hits = 0;       // lookups that found a stencil (or found none, refused before)
        uint64_t built = 0;      // stencils built
        uint64_t refused = 0;    // packets that cannot be stencils, by reason below
        uint64_t batches = 0;    // modules compiled
        uint64_t lift_ns = 0, compile_ns = 0, extract_ns = 0;
        uint64_t code_bytes = 0;
        std::map<std::string, uint64_t> reasons;
    };

    RuntimeStencils();
    ~RuntimeStencils();
    RuntimeStencils(const RuntimeStencils&) = delete;
    RuntimeStencils& operator=(const RuntimeStencils&) = delete;

    // Builds the stencils' compiler; false if it cannot (no stencils then).
    // Turning settings that change code drops what was built.
    bool configure(const Settings& settings);
    const Settings& settings() const { return settings_; }

    // The packet's stencil, null if it has none; built, with every other
    // packet of `packets` that has not been tried, in one module (a block's
    // missing packets compile together).
    const Record* find(const Packet& packet) const;
    void build(const std::vector<Packet>& packets);
    // Tried before (whether or not it got a stencil).
    bool tried(const Packet& packet) const;

    const Stats& stats() const { return stats_; }
    // A hash of the packet's encoding, for Settings::range_*.
    static uint64_t key_hash(const Packet& packet);

private:
    struct Entry {
        const Record* record = nullptr;  // null: refused
    };
    struct Hash {
        size_t operator()(const std::pair<uint64_t, uint8_t>& k) const { return size_t(k.first * 0x9e3779b97f4a7c15ull) ^ k.second; }
    };
    using Key = std::pair<uint64_t, uint8_t>;
    static Key key_of(const Packet& p) { return {p.key(), p.count}; }
    void refuse(const Packet& packet, const std::string& why);
    // Lifts packets into one module and compiles it: each one's lift (or
    // why it was refused) and the module's code by function name; false if
    // the module did not compile.
    struct Lifted;
    bool compile(const std::vector<Packet>& packets, std::vector<Lifted>& lifted, std::map<std::string, Code>& code);

    Settings settings_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<Compiler> compiler_;
    std::unique_ptr<StencilMemory> memory_;
    std::unique_ptr<BBTranslator> translator_;
    std::unordered_map<Key, Entry, Hash> entries_;
    std::deque<Record> records_;
    mutable Stats stats_;
};

}  // namespace bcore_cp
