#pragma once

// The copy-and-patch tier 0: a block made of the stencils bcore-stencilgen
// built (cp_format.h), copied in place and their holes filled in, in
// microseconds where the LLVM tier 0 takes a millisecond or more.
//
// A block the translator takes is one whose packets all have stencils:
// every 16-bit and 32-bit instruction, and a parallel packet as its pieces
// (compose_parallel), but an illegal parallel packet (a slot the decoder
// refuses, SEARCH in slot 0). For any other it places nothing and Core
// compiles the block with LLVM. Its code is, in order: the prologue (the
// tier counter, and CYCLES with host cycles), each packet's stencils (one,
// or a parallel packet's pieces), for a block the packet cap cuts the
// fall-through stencil, and, for a block that chains, its own exit
// (Config::self_loop). Stencils are split (cp_format.h's Stencil): the
// block is their hot parts, one after the other, and their cold parts lie
// in the arena's cold region. A packet's path to the next one is a jump to
// the next stencil, dropped where it ends the hot part, and so is a last
// jump to the block's own exit; every path that leaves the block
// jumps to that exit, or to the exit stencil of its arena: the chain exit
// of a block whose next PC is known only at run time
// (BBTranslator::emit_stencil_exit). A block's own exit runs the block
// again without the chain table when its next PC is its own (a loop's back
// edge). Helpers are called through jumps in the arena (a stencil's call is
// PC-relative).
//
// What a block does is what the LLVM tier 0's block does, packet for
// packet: each stencil is the packet a careful block with no hint lifts
// (BBTranslator::translate_stencil), so it compares the loop bottoms at
// every packet and needs no entry guard.
//
// Stencil code has no unwind information. The helpers that may reach the
// host's code are called through wrappers that catch whatever the host
// throws (host_bridge.h).
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
        // The block ends in an exit of its own (kSelfExits) that runs the
        // block again without the chain table when it goes on to its own PC:
        // only where it chains and not dry, and where the chain filter
        // takes the block's PC (Core::set_chain_filter).
        bool self_loop = false;
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
        uint8_t* cold = nullptr;   // its cold code (in the arena's cold region), cold_size bytes
        size_t cold_size = 0;
        const char* why = nullptr; // why there is none
    };

    explicit CpTranslator(JitEngine& jit) : jit_(jit) {}

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
        const uint8_t* cold = nullptr;  // its cold code (Stencil::cold) and relocations
        const bcore_cp::Reloc* cold_relocs = nullptr;
        size_t cold_at = 0;  // offset in the block's cold code
        uint32_t packet = 0; // the packet's index in the block, from 1 (a parallel packet's pieces share it)
    };
    // An arena's helper jumps and exit stencils, placed when the arena is
    // first used.
    struct Arena {
        std::shared_ptr<CodeArena> arena;
        uint8_t* helpers[bcore_cp::kHelperCount] = {};
        uint8_t* exits[5] = {};
    };
    bool scan(uint32_t pc, Memory& code, const Config& config, Block& out);
    // A parallel packet at pc whose slot 0 (insn) has a body stencil, as
    // pieces: slot 1, slot 2, the body, the slots' commits, the packet's
    // tail (BBTranslator::translate_stencil_body). False with why where a
    // slot has no stencil, or the packet's memory callbacks may change
    // registers (the body runs after the slots' accesses, which its reads
    // must not see).
    bool compose_parallel(uint32_t pc, uint32_t insn, int selected, const bcore_cp::Stencil& body, Memory& code,
                          const std::function<uint16_t(uint32_t)>& read, uint32_t packet, const char*& why);
    Arena* arena_for(size_t size);
    bool place(Arena& arena, uint8_t* code, uint8_t* cold, const Piece& piece, const Piece* next, uint32_t packet,
               uint64_t field0, uint32_t counter, const Config& config);
    // A value hole's value in a piece (packet: its index in the block;
    // field0: the length of the packet the fall-through stencil follows).
    bool value_of(const Piece& piece, const bcore_cp::Reloc& r, uint32_t packet, uint64_t field0,
                  uint32_t counter, uint64_t& value) const;

    // The build's stencil s as a piece.
    static Piece piece(const bcore_cp::Stencil& s, uint32_t pc, uint32_t insn, int selected);

    uint32_t threshold_ = 0;  // the last block's tier counter's (Block::threshold)
    uint64_t last_len_ = 0;   // the length of the packet the fall-through stencil follows
    int exit_piece_ = -1;     // the block's own exit (Config::self_loop): its piece, or -1
    JitEngine& jit_;
    std::function<void(const char*, const void*, size_t)> listener_;
    std::vector<std::unique_ptr<Arena>> arenas_;
    std::vector<Piece> pieces_;
    bcore_tier::CodeLog words_;
};
