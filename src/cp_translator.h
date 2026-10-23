#pragma once

// The copy-and-patch tier 0: a block made of the stencils bcore-stencilgen
// built (cp_format.h), copied in place and their holes filled in, in
// microseconds where the LLVM tier 0 takes a millisecond or more.
//
// A block the translator takes is one whose packets all have stencils: the
// build's (the 16-bit instructions and the 32-bit patterns of
// cp_patterns.def), or, with set_runtime, one built for the packet at run
// time (cp_runtime.h: a parallel packet, any other 32-bit instruction). For
// any other it places nothing and Core compiles the block with LLVM. Its code is, in order: the prologue (the tier counter,
// and CYCLES with host cycles), each packet's stencil, and for a block the
// packet cap cuts the fall-through stencil. A packet's path to the next
// one is a jump to the next stencil, dropped where it is the stencil's
// last instruction; every path that leaves the block jumps to the exit
// stencil of its arena: the chain exit of a block whose next PC is known
// only at run time (BBTranslator::emit_stencil_exit). Helpers are called
// through jumps in the arena (a stencil's call is PC-relative).
//
// What a block does is what the LLVM tier 0's block does, packet for
// packet: each stencil is the packet a careful block with no hint lifts
// (BBTranslator::translate_stencil), so it compares the loop bottoms at
// every packet and needs no entry guard.
//
// Stencil code has no unwind information. The helpers that may reach the
// host's code (memory callbacks, exceptions that read guest memory) are
// called through wrappers that catch whatever the host throws: the wrapper
// sets did_jump and a chain budget of 0, so that the block leaves at once,
// and Core::run throws it again once the block has returned (RunScope).
//
// x86-64 only (BCORE_COPY_PATCH); single-threaded, like JitEngine.

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include "cp_format.h"
#include "cpu_state.h"
#include "jit_engine.h"
#include "mem.h"
#include "tier_memory.h"

class CodeArena;
namespace bcore_cp { class RuntimeStencils; }

class CpTranslator {
public:
    struct Config {
        // The exit: BBTranslator::set_chaining and set_count_chained.
        bool chaining = false;
        bool dry = false;
        bool count_chained = true;
        bool host_cycles = false;  // BBTranslator::set_host_cycles
        bool idle_waits = false;   // a block with IDLE then compiles with LLVM
        uint32_t threshold = 0;    // the tier counter's
        uint32_t size_ref = 0;     // and tier_threshold's (tier_table.h), by the block's packets
        uint32_t max_packets = 256;
    };
    struct Block {
        BbFunc fn = nullptr;       // null: no block (why says why)
        uint32_t fallthrough_pc = 0;
        uint32_t packets = 0;
        uint32_t threshold = 0;    // its tier counter's: tier_threshold of its packets
        size_t size = 0;           // its code's bytes, at fn
        const char* why = nullptr; // why there is none
        // Packets whose stencils were built at run time (set_runtime), and
        // how many of them this translation built.
        uint32_t runtime_packets = 0;
        uint32_t built = 0;
    };

    explicit CpTranslator(JitEngine& jit) : jit_(jit) {}
    // Stencils built at run time for packets the build left without one
    // (null: none, such a block compiles with LLVM). Kept by the caller.
    void set_runtime(bcore_cp::RuntimeStencils* runtime) { runtime_ = runtime; }

    // The block at pc, its instructions read through code, its starts
    // counted in TierTable::count[counter]. words() then holds every
    // halfword it read, for a translation of the same block to read again.
    Block translate(uint32_t pc, Memory& code, uint32_t counter, const Config& config);
    const bcore_tier::CodeLog& words() const { return words_; }
    // A hash of the last block's code, the same for the same block in any
    // process (BcoreCompileMetrics::object_hash): FNV-1a over each stencil's
    // code before relocation, its relocations and the values of its value
    // holes, but not the addresses its jumps and calls reach.
    uint64_t code_hash(uint32_t counter) const;
    // Told of the code an arena's first block places beside it: the exit
    // stencils (bcore_cp_exit) and the jumps to the helpers
    // (bcore_cp_jump_<helper>), for a profiler's code map.
    void set_code_listener(std::function<void(const char*, const void*, size_t)> listener) {
        listener_ = std::move(listener);
    }

    // Set while a thread runs blocks: the CpuState a wrapper stops when it
    // catches an exception. rethrow() throws what was caught, if anything.
    class RunScope {
    public:
        explicit RunScope(CpuState* cpu);
        ~RunScope();
        RunScope(const RunScope&) = delete;
        RunScope& operator=(const RunScope&) = delete;
        void rethrow();

    private:
        CpuState* previous_;
    };

private:
    struct Piece {
        const bcore_cp::Stencil* stencil;
        const uint8_t* code;            // the stencil's code
        const bcore_cp::Reloc* relocs;  // and relocations
        uint32_t pc;
        uint32_t insn;
        int selected;  // the 32-bit pattern (kSelected32), or -1
        size_t at;     // offset in the block's code
        size_t size;   // bytes placed
    };
    // An arena's helper jumps and exit stencils, placed when the arena is
    // first used.
    struct Arena {
        std::shared_ptr<CodeArena> arena;
        uint8_t* helpers[bcore_cp::kHelperCount] = {};
        uint8_t* exits[5] = {};
    };
    bool scan(uint32_t pc, Memory& code, const Config& config, Block& out);
    Arena* arena_for(size_t size);
    bool place(Arena& arena, uint8_t* code, const Piece& piece, const Piece* next, uint32_t packet,
               uint64_t field0, uint32_t counter, const Config& config);
    // A value hole's value in a piece (packet: its index in the block;
    // field0: the length of the packet the fall-through stencil follows).
    bool value_of(const Piece& piece, const bcore_cp::Reloc& r, uint32_t packet, uint64_t field0,
                  uint32_t counter, uint64_t& value) const;

    // The build's stencil s as a piece.
    static Piece piece(const bcore_cp::Stencil& s, uint32_t pc, uint32_t insn, int selected);

    uint32_t threshold_ = 0;  // the last block's tier counter's (Block::threshold)
    JitEngine& jit_;
    bcore_cp::RuntimeStencils* runtime_ = nullptr;
    std::function<void(const char*, const void*, size_t)> listener_;
    std::vector<std::unique_ptr<Arena>> arenas_;
    std::vector<Piece> pieces_;
    bcore_tier::CodeLog words_;
};
