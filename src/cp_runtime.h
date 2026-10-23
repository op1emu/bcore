#pragma once

// Stencils built at run time (Core::set_copy_patch_runtime): the copy-and-
// patch tier 0's for packets the build left without one (cp_translator.h).
// A packet with none (a parallel one, a 32-bit instruction outside
// cp_patterns.def, a 16-bit encoding whose build-time code was refused) is
// lifted as tools/stencilgen.cpp lifts every packet it builds
// (BBTranslator::translate_stencil, at kPcA, cp_build.h), compiled for the
// stencils' target, and kept for the process by its encoding: holes for its
// PC and packet index only (no field holes: the encoding's immediates are
// constants), so one stencil serves the packet wherever it is. A packet that
// cannot be a stencil is kept too, as one without; its blocks compile with
// LLVM.
//
// A translation stages each missing packet as it meets it: the packet is
// lifted and checked at once, which says how long it is and whether it ends
// the block (so the translator reads no further than the block goes), and
// flush() then compiles every staged packet in one module.
//
// A packet is the halfwords its decoding reads, which only those read
// before decide (decodeInstruction): an illegal first slot of a parallel
// packet ends it after two, a third slot that looks like a 32-bit
// instruction adds a fifth. Its first lift fetches them from the
// translator one by one as the decoder asks, and every shorter prefix of
// them is kept as "read one more": a translation that finds a packet so
// reads exactly the halfwords the LLVM tier 0's decoding would.
//
// Checks, as the build's: the lift reads only the packet's halfwords and
// keeps its length (or ends the block, as an illegal instruction does), its
// IR is the same lifted at another PC (kPcB), and every relocation of its
// code is a hole (extract()). Single-threaded, like CpTranslator.

#include <cstdint>
#include <deque>
#include <functional>
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
    // A packet: the halfwords its decoding reads and their number (1 to
    // StencilMemory::kMaxWords).
    struct Packet {
        uint16_t words[StencilMemory::kMaxWords] = {};
        uint8_t count = 0;
        bool operator==(const Packet& o) const;
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

    // What is kept for these halfwords: a stencil, none (refused), or that
    // decoding reads one more (`more`); nothing yet if none of the three.
    struct Found {
        const Record* record = nullptr;
        bool refused = false;
        bool more = false;
        bool known() const { return record || refused || more; }
    };
    Found find(const Packet& packet) const;
    // Halfwords not known yet, lifted and checked for the next flush(), the
    // decoding's further halfwords taken from fetch (their index in the
    // packet) and added to packet: its length and flags (kTerminates,
    // kCycles, kIdle) if it may be a stencil, else ok false (refused, and
    // kept so). Whatever fetch throws is thrown, with nothing staged.
    struct Staged {
        bool ok = false;
        uint8_t bytes = 0;
        uint16_t flags = 0;
    };
    Staged stage(Packet& packet, const std::function<uint16_t(int)>& fetch);
    // Compiles the staged packets in one module and keeps their stencils
    // (or refusals); discard() drops them untried instead.
    void flush();
    void discard();
    bool staged() const { return !pending_.empty(); }

    const Stats& stats() const { return stats_; }
    // A hash of the packet's encoding, for Settings::range_*.
    static uint64_t key_hash(const Packet& packet);

private:
    struct Entry {
        const Record* record = nullptr;  // null and not more: refused
        bool more = false;
    };
    struct Hash {
        size_t operator()(const Packet& p) const { return size_t(key_hash(p)); }
    };
    void refuse(const Packet& packet, const std::string& why);
    // Lifts and checks the packet into mod as `name`: why it cannot be a
    // stencil, or empty.
    struct Lifted;
    std::string lift(llvm::Module& mod, Packet& packet, const std::string& name, Lifted& out,
                     const std::function<uint16_t(int)>& fetch = nullptr);
    // Compiles mod: its code by function name; false if it did not compile.
    bool compile(llvm::Module& mod, std::map<std::string, Code>& code);

    Settings settings_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<Compiler> compiler_;
    std::unique_ptr<StencilMemory> memory_;
    std::unique_ptr<BBTranslator> translator_;
    std::unordered_map<Packet, Entry, Hash> entries_;
    std::deque<Record> records_;
    // Staged for the next flush: the module and its packets.
    std::unique_ptr<llvm::Module> batch_;
    std::vector<Lifted> pending_;
    mutable Stats stats_;
};

}  // namespace bcore_cp
