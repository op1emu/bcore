#include "cp_translator.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include <llvm/Support/Memory.h>

#include "cec.h"
#include "code_arena.h"
#include "cp_patch.h"
#include "helper_abi.h"
#include "lift_visitor.h"
#include "host_bridge.h"
#include "syscall_emu.h"
#include "tier_table.h"

namespace bcore {

using namespace bcore_cp;

namespace {

#if !defined(__EMSCRIPTEN__)
// What a stencil's helper call reaches, in the order of kHelpers.
void* const kHelperFns[] = {
    reinterpret_cast<void*>(&bcore_bridge::mem_read8),
    reinterpret_cast<void*>(&bcore_bridge::mem_read16),
    reinterpret_cast<void*>(&bcore_bridge::mem_read32),
    reinterpret_cast<void*>(&bcore_bridge::mem_write8),
    reinterpret_cast<void*>(&bcore_bridge::mem_write16),
    reinterpret_cast<void*>(&bcore_bridge::mem_write32),
    reinterpret_cast<void*>(&bcore_bridge::cec_data_alignment),
    reinterpret_cast<void*>(&bcore_bridge::cec_exception),
    reinterpret_cast<void*>(&cec_raise),
    reinterpret_cast<void*>(&cec_return_rti),
    reinterpret_cast<void*>(&cec_return_rtx),
    reinterpret_cast<void*>(&cec_return_rtn),
    reinterpret_cast<void*>(&cec_return_rte),
    reinterpret_cast<void*>(&cec_cli),
    reinterpret_cast<void*>(&cec_sti),
    reinterpret_cast<void*>(&cec_push_reti),
    reinterpret_cast<void*>(&cec_pop_reti),
    reinterpret_cast<void*>(&cec_check_sup),
    reinterpret_cast<void*>(&cec_is_user_mode),
    reinterpret_cast<void*>(&cec_check_pending),
    reinterpret_cast<void*>(&bfin_hwloop_step),
    reinterpret_cast<void*>(&bfin_putchar),
    reinterpret_cast<void*>(&bcore_bridge::bfin_syscall),
    reinterpret_cast<void*>(&::memset),
    reinterpret_cast<void*>(&::memcpy),
    reinterpret_cast<void*>(&::memmove),
};
static_assert(sizeof(kHelperFns) / sizeof(kHelperFns[0]) == kHelperCount, "a function for every helper");
#endif

// ---- Decoding, as decoder.h decides ----

bool is16(uint16_t w) {
    return (w & 0xc000) != 0xc000 || (w & 0xff00) == 0xf800 || (w & 0xff00) == 0xf900 || (w & 0xff00) == 0xff00;
}

bool is_parallel(uint16_t w) { return (w & 0x0800) && (w & 0xe800) != 0xe800; }

int match32(uint32_t insn) {
    for (uint32_t i = 0; i < kPatterns32Count; ++i)
        if ((insn & kPatterns32[i].mask) == kPatterns32[i].value) return int(i);
    return -1;
}

// mask's bits of v, packed from the lowest.
uint32_t gather(uint32_t v, uint32_t mask) {
    uint32_t out = 0;
    for (unsigned bit = 0; mask; mask &= mask - 1, ++bit)
        if (v & mask & -mask) out |= 1u << bit;
    return out;
}

#if !defined(__EMSCRIPTEN__)
// A jump to a helper: an absolute jump, 16 bytes. The other host
// specifics: the jump a stencil's hot part may end in (kTailCont,
// kTailExit), and how far a stencil's call or jump reaches (an arena's
// helper jumps and exits lie within that of every block that uses them).
constexpr size_t kTrampoline = 16;
#if defined(__aarch64__)
constexpr size_t kTailJump = 4;  // B
constexpr size_t kReach = size_t(120) << 20;  // B/BL's 128 MiB, less a block's largest
#else
constexpr size_t kTailJump = 5;  // jmp rel32
constexpr size_t kReach = SIZE_MAX;  // rel32's 2 GiB: the whole arena
#endif

void write_trampoline(uint8_t* at, const void* target) {
#if defined(__aarch64__)
    store32(at, 0x58000050);      // ldr x16, #8
    store32(at + 4, 0xd61f0200);  // br x16
    memcpy(at + 8, &target, 8);
#else
    const uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};  // jmp *0(%rip)
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + 6, &target, 8);
    memset(at + 14, 0xcc, kTrampoline - 14);
#endif
}

// Code nothing points at: what it would run traps (int3, BRK #0).
void fill_trap(uint8_t* at, size_t size) {
#if defined(__aarch64__)
    for (size_t i = 0; i + 4 <= size; i += 4) store32(at + i, 0xd4200000);
#else
    memset(at, 0xcc, size);
#endif
}
#else
constexpr size_t kReach = SIZE_MAX;  // a module's calls reach anything
#endif

}  // namespace

CpTranslator::CpTranslator(JitEngine& jit) : jit_(jit), reach_(kReach) {}

void CpTranslator::set_reach(size_t bytes) { reach_ = bytes ? std::min(bytes, kReach) : kReach; }

CpTranslator::Piece CpTranslator::piece(const Stencil& s, uint32_t pc, uint32_t insn, int selected) {
    return {&s, kCode + s.code, kRelocs + s.relocs, pc, insn, selected, 0, 0, kCode + s.cold, kRelocs + s.cold_relocs, 0};
}

bool CpTranslator::scan(uint32_t pc, Memory& code, const Config& config, Block& out) {
    words_.clear();
    std::function<uint16_t(uint32_t)> read = [&](uint32_t addr) -> uint16_t {
        const uint16_t w = code.read16(addr);
        words_.emplace_back(addr, w);
        return w;
    };
    pieces_.clear();
    pieces_.push_back({nullptr, nullptr, nullptr, pc, 0, -1, 0, 0});  // the prologue, chosen below
    guarded_ = false;
    pinned_ = 0;
    past_guard_ = false;
    // Whether the hint's loops still hold at the packet: none has moved one
    // since the block's start (kLoopsMoved).
    bool steady = true;
    uint32_t last_pc = pc;
    bool cycles = false;
    uint32_t cur = pc, packets = 0, last_bytes = 0;
    auto fail = [&](const char* why) {
        out.why = why;
        return false;
    };
    for (uint32_t k = 0; k < config.max_packets; ++k) {
        const uint32_t index = ++packets;
        const uint16_t w = read(cur);
        uint32_t id = 0, insn = w;
        int selected = -1;
        if (is16(w)) {
            id = kIndex16[w];
        } else {
            // The decoder reads the second halfword of anything not 16-bit,
            // a parallel packet's further ones only as its decoding goes on.
            insn = uint32_t(w) << 16 | read(cur + 2);
            const int p = match32(insn);
            if (p >= 0 && kPatterns32[p].selected >= 0) {
                selected = kPatterns32[p].selected;
                const Selected32& sel = kSelected32[selected];
                id = kKeys32[sel.keys + gather(insn, sel.key_mask)];
            } else if (p < 0) {
                id = kUndef32 + 1;  // slot 0 too: the packet the decoder abandons
            }
            // A parallel packet whose slot 0 takes the instruction: its
            // pieces (BBTranslator::translate_stencil_body).
            if (id && (kStencils[id - 1].flags & kBody)) {
                const char* why = nullptr;
                const size_t first = pieces_.size();
                if (!compose_parallel(cur, insn, selected, kStencils[id - 1], code, read, index, why, config, steady))
                    return fail(why);
                last_pc = cur;
                for (size_t i = first; i < pieces_.size(); ++i)
                    cycles |= (pieces_[i].stencil->flags & kCycles) != 0;
                cur += 8;
                last_bytes = 8;
                continue;
            }
        }
        if (!id) return fail(is16(w) ? "no stencil" : is_parallel(w) ? "parallel" : "32-bit");
        // IDLE waits where the host does (kIdleWait).
        const bool waits = (kStencils[id - 1].flags & kIdle) && config.idle_waits;
        Piece piece = CpTranslator::piece(kStencils[waits ? kIdleWait : id - 1], cur, insn, selected);
        const uint16_t flags = piece.stencil->flags;
        cycles |= (flags & kCycles) != 0;
        piece.packet = index;
        pieces_.push_back(piece);
        last_pc = cur;
        if (flags & kLoopsMoved) steady = false;
        cur += piece.stencil->bytes;
        last_bytes = piece.stencil->bytes;
        if (flags & kTerminates) break;
    }
    out.fallthrough_pc = cur;
    out.packets = packets;
    last_len_ = last_bytes;
    if (!(pieces_.back().stencil->flags & kTerminates)) {
        pieces_.push_back(CpTranslator::piece(kStencils[kFallthrough], cur, 0, -1));
        pieces_.back().packet = packets;  // the last packet's
    }
    // A block that never reaches its threshold need not count its starts.
    const uint32_t* prologue = config.threshold == UINT32_MAX ? kPrologueUncounted : kPrologue;
    pieces_[0] = CpTranslator::piece(kStencils[prologue[config.host_cycles && cycles]], pc, 0, -1);
    if (guarded_) {
        // Before the prologue: a block whose guard fails neither counts its
        // start nor sets CYCLES.
        pieces_.insert(pieces_.begin(), CpTranslator::piece(kStencils[kGuards[pinned_]], pc, 0, -1));
        for (int i = 0; i < 2; ++i) {
            hint_lb_[i] = config.hint->lb[i];
            hint_lt_[i] = config.hint->lt[i];
        }
        span_ = last_pc - pc;
        past_guard_ = steady;
    }
    return true;
}

bool CpTranslator::compose_parallel(uint32_t pc, uint32_t insn, int selected, const Stencil& body, Memory& code,
                                    const std::function<uint16_t(uint32_t)>& read, uint32_t packet, const char*& why,
                                    const Config& config, bool& steady) {
    if (!code.callbacks_preserve_registers()) {
        why = "parallel: callbacks may change registers";
        return false;
    }
    const uint16_t s1 = read(pc + 4), s2 = read(pc + 6);
    if (!is16(s1) || !is16(s2)) {
        why = "parallel: a slot of 32 bits";
        return false;
    }
    const unsigned algn = (body.flags & kSetsDisAlgn) ? 1 : 0;
    const SlotEntry& e1 = kSlots[algn * 2][s1];
    if (!e1.stencil) {
        why = "parallel: no stencil for slot 1";
        return false;
    }
    const Stencil& st1 = kStencils[e1.stencil - 1];
    const SlotEntry& e2 = kSlots[algn * 2 + 1][s2];
    if (!e2.stencil) {
        why = "parallel: no stencil for slot 2";
        return false;
    }
    const Stencil& st2 = kStencils[e2.stencil - 1];
    const unsigned tail = ((st1.flags | st2.flags) & kSlotStore) ? 1 : 0;
    auto add = [&](const Stencil& s, uint32_t word, int sel) {
        pieces_.push_back(CpTranslator::piece(s, pc, word, sel));
        pieces_.back().packet = packet;
    };
    if (!(st1.flags & kNop)) add(st1, 0, -1);
    if (!(st2.flags & kNop)) add(st2, 0, -1);
    add(body, insn, selected);
    if (e1.commit) add(kStencils[e1.commit - 1], 0, -1);
    if (e2.commit) add(kStencils[e2.commit - 1], 0, -1);
    // Its tail, by what the hint says of its loop bottoms (Config::hint).
    const CpuState* hint = config.hint;
    if ((st1.flags | st2.flags | body.flags) & kLoopsMoved) steady = false;
    const bool bottom = hint && (hint->lb[0] == pc || hint->lb[1] == pc);
    uint32_t id = (bottom ? kParallelTailBottom : kParallelTail)[tail];
    if (hint && !config.careful && steady) {
        if (const unsigned pin = LiftVisitor::pin_mask(*hint, pc)) {
            id = kParallelTailPinned[pin - 1][tail];
            pinned_ |= pin;
            guarded_ = true;
        } else if (!bottom) {
            id = kParallelTailAbsent[tail];
            guarded_ = true;
        }
    }
    add(kStencils[id], 0, -1);
    return true;
}

#if !defined(__EMSCRIPTEN__)
// Where a stencil's call of each helper goes: the helper (wrapped,
// kHelperFns) where the helpers' convention is C's, else a trampoline in
// it to the C function (CodeLoader::makeTrampolines); memset, memcpy and
// memmove are called in C's.
bool CpTranslator::helper_targets() {
    if (helpers_ == Helpers::kReady) return true;
    if (helpers_ == Helpers::kFailed) return false;
    helpers_ = Helpers::kFailed;  // once: a failed module's names stay defined
    std::vector<const char*> names;
    std::vector<void*> targets;
    std::vector<unsigned> which;
    for (unsigned i = 0; i < kHelperCount; ++i) {
        helper_targets_[i] = kHelperFns[i];
        if (bcore_abi::kHelperCC == llvm::CallingConv::C || !bcore_abi::find_helper(kHelpers[i])) continue;
        names.push_back(kHelpers[i]);
        targets.push_back(kHelperFns[i]);
        which.push_back(i);
    }
    std::vector<void*> out(names.size());
    if (!names.empty() &&
        !jit_.loader().makeTrampolines(names.size(), names.data(), targets.data(), out.data(), "bcore_cp_trampoline."))
        return false;
    for (size_t k = 0; k < which.size(); ++k) helper_targets_[which[k]] = out[k];
    helpers_ = Helpers::kReady;
    return true;
}

size_t CpTranslator::support_size() {
    size_t size = kHelperCount * kTrampoline;
    for (uint32_t e : kExits) size += kStencils[e].size + 16;
    return size;
}

CpTranslator::Arena* CpTranslator::arena_for(size_t size) {
    // Room for the block and for a set of jumps and exits beside it.
    auto arena = jit_.loader().codeArena(size + support_size());
    if (!arena) return nullptr;
    for (auto& a : arenas_)
        if (a->arena == arena) return a.get();
    auto a = std::make_unique<Arena>();
    a->arena = arena;
    arenas_.push_back(std::move(a));
    return arenas_.back().get();
}

// The arena's helper jumps and exits within reach of code placed next in
// its hot region: its last set, or a new set there.
const CpTranslator::Support* CpTranslator::support_for(Arena& a) {
    uint8_t* next = a.arena->allocate(CodeArena::Region::Hot, 0, 16);
    if (!next) return nullptr;
    if (!a.sets.empty() && size_t(next - a.sets.back().start) < reach_) return &a.sets.back();
    Support set;
    for (unsigned i = 0; i < kHelperCount; ++i) {
        uint8_t* at = a.arena->allocate(CodeArena::Region::Hot, kTrampoline, kTrampoline);
        if (!at) return nullptr;
        if (i == 0) set.start = at;
        write_trampoline(at, helper_targets_[i]);
        set.helpers[i] = at;
        if (listener_) listener_(("bcore_cp_jump_" + std::string(kHelpers[i])).c_str(), at, kTrampoline);
    }
    for (int e = 0; e < 5; ++e) {
        const Stencil& s = kStencils[kExits[e]];
        uint8_t* at = a.arena->allocate(CodeArena::Region::Hot, s.size, 16);
        if (!at || s.reloc_count || s.cold_size) return nullptr;  // the exits' code fills in nothing, in one piece
        memcpy(at, kCode + s.code, s.size);
        set.exits[e] = at;
        if (listener_) listener_("bcore_cp_exit", at, s.size);
    }
    uint8_t* end = a.arena->allocate(CodeArena::Region::Hot, 0, 1);
    llvm::sys::Memory::InvalidateInstructionCache(set.start, size_t(end - set.start));
    a.sets.push_back(set);
    ++support_sets_;
    return &a.sets.back();
}

namespace {

// A conditional branch between a stencil's hot and cold code (AArch64). The
// splitter's cold code lies far from the hot code, beyond a B.cond's 1 MiB and
// a TBZ's 32 KiB, which LLVM 15 does not relax (LLVM 18 does, the same way):
// the branch goes to a B near it in the same region, which reaches anywhere
// in the arena (CodeArena's Shared layout). The fast path is the same; the
// rare one takes one more branch. A piece's cold trampolines follow its cold
// part (which ends in a jump); hot ones follow the block's code, or, in a
// long block, a piece that keeps its jump to the next (kIsland).
bool conditional(const Reloc& r) { return r.kind == kCondBr19 || r.kind == kTestBr14; }

// A piece's branches to one place share a trampoline: the first of them.
bool first_to(const Reloc* relocs, uint32_t i) {
    for (uint32_t k = 0; k < i; ++k)
        if (conditional(relocs[k]) && relocs[k].hole == relocs[i].hole && relocs[k].addend == relocs[i].addend)
            return false;
    return true;
}

size_t trampolines(const Reloc* relocs, uint32_t count, size_t size) {
    size_t n = 0;
    for (uint32_t i = 0; i < count; ++i) n += relocs[i].offset < size && conditional(relocs[i]) && first_to(relocs, i);
    return n;
}

constexpr size_t kBranchTrampoline = 4;  // B
// The most hot code between a TBZ and its trampoline (it reaches 32 KiB, and
// a piece is under 1 KiB).
constexpr size_t kIsland = size_t(24) << 10;

}  // namespace

bool CpTranslator::place(const Support& arena, uint8_t* code, uint8_t* cold, const Piece& piece, const Piece* next,
                         uint32_t packet, uint64_t field0, uint32_t counter, const Config& config) {
    const Stencil& s = *piece.stencil;
    uint8_t* hot_at = code + piece.at;
    uint8_t* cold_at = cold + piece.cold_at;
    memcpy(hot_at, piece.code, piece.size);
    if (s.cold_size) memcpy(cold_at, piece.cold, s.cold_size);
    const int exit = !config.chaining ? 0 : config.dry ? (config.count_chained ? 4 : 3) : (config.count_chained ? 2 : 1);
    // Each part's relocations; the hot part's last jump may be dropped.
    auto fill = [&](uint8_t* at, const Reloc* relocs, uint32_t count, size_t size, uint8_t*& trampoline) {
        std::vector<std::pair<Reloc, uint8_t*>> jumps;  // this part's trampolines
        for (uint32_t i = 0; i < count; ++i) {
            const Reloc& r = relocs[i];
            if (r.offset >= size) continue;  // in the dropped jump to what follows
            uint64_t value = 0;
            if (is_value_hole(r.hole)) {
                if (!value_of(piece, r, packet, field0, counter, value)) return false;
            } else if (r.hole == kHoleCont) {
                if (!next) return false;
                value = reinterpret_cast<uintptr_t>(code + next->at);
            } else if (r.hole == kHoleExit) {
                value = reinterpret_cast<uintptr_t>(exit_piece_ >= 0 ? code + pieces_[exit_piece_].at : arena.exits[exit]);
            } else if (r.hole == kHoleSelf) {
                // Its start, or past its guard: the prologue.
                value = reinterpret_cast<uintptr_t>(code + (past_guard_ ? pieces_[1].at : 0));
            } else if (r.hole == kHoleHot) {
                value = reinterpret_cast<uintptr_t>(hot_at);  // the place there in the addend
            } else if (r.hole == kHoleCold) {
                if (!s.cold_size) return false;
                value = reinterpret_cast<uintptr_t>(cold_at);
            } else if (r.hole >= kHoleHelper && r.hole < kHoleHelper + kHelperCount) {
                value = reinterpret_cast<uintptr_t>(arena.helpers[r.hole - kHoleHelper]);
            } else {
                return false;
            }
            if (conditional(r)) {
                // The B the piece's first branch there placed, or a new one.
                uint8_t* jump_at = nullptr;
                for (const auto& [to, placed] : jumps)
                    if (to.hole == r.hole && to.addend == r.addend) jump_at = placed;
                if (!jump_at) {
                    if (!trampoline) return false;
                    jump_at = trampoline;
                    const Reloc jump{0, r.hole, kBranch26, r.addend};
                    store32(jump_at, 0x14000000u);  // B
                    if (!patch_reloc(jump_at, jump, value)) return false;
                    jumps.push_back({r, jump_at});
                    trampoline += kBranchTrampoline;
                }
                const Reloc branch{r.offset, r.hole, r.kind, 0};
                if (!patch_reloc(at, branch, reinterpret_cast<uintptr_t>(jump_at))) return false;
                continue;
            }
            if (!patch_reloc(at, r, value)) return false;
        }
        return true;
    };
    uint8_t* hot_jumps = piece.jumps ? code + piece.jumps_at : nullptr;
    uint8_t* cold_jumps = piece.cold_jumps ? cold + piece.cold_jumps_at : nullptr;
    return fill(hot_at, piece.relocs, s.reloc_count, piece.size, hot_jumps) &&
           fill(cold_at, piece.cold_relocs, s.cold_reloc_count, s.cold_size, cold_jumps);
}

#endif

bool CpTranslator::value_of(const Piece& piece, const Reloc& r, uint32_t packet, uint64_t field0,
                            uint32_t counter, uint64_t& value) const {
    if (r.hole == kHolePc) {
        value = piece.pc;
    } else if (r.hole == kHolePacket) {
        value = packet;
    } else if (r.hole >= kHoleField0 && r.hole < kHoleField0 + 4) {
        const unsigned f = r.hole - kHoleField0;
        if (piece.selected >= 0) value = field_hole_value(kSelected32[piece.selected].fields[f], piece.insn, piece.pc);
        else if (f == 0) value = field0;
        else return false;
    } else if (r.hole == kHoleCounter) {
        value = counter;
    } else if (r.hole == kHoleCountOffset) {
        value = offsetof(TierTable, count) + 4 * uint64_t(counter);
    } else if (r.hole == kHoleThreshold) {
        value = threshold_;
    } else if (r.hole == kHoleBlockPc) {
        value = piece.pc;  // the exit's piece is the block's PC's
    } else if (r.hole == kHoleLb0 || r.hole == kHoleLb1) {
        value = hint_lb_[r.hole - kHoleLb0];
    } else if (r.hole == kHoleLt0 || r.hole == kHoleLt1) {
        value = hint_lt_[r.hole - kHoleLt0];
    } else if (r.hole == kHoleSpan) {
        value = span_;
    } else if (r.hole >= kHoleReg0 && r.hole < kHoleReg0 + kRegHoles && piece.selected >= 0) {
        value = field_hole_value(kSelected32[piece.selected].regs[r.hole - kHoleReg0], piece.insn, piece.pc);
    } else {
        return false;
    }
    return true;
}

uint64_t CpTranslator::code_hash(uint32_t counter) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    const uint64_t last_len = last_len_;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        const Piece& p = pieces_[i];
        const Stencil& s = *p.stencil;
        mix(p.code, p.size);
        const uint32_t packet = p.packet;
        auto relocs = [&](const Reloc* relocs, uint32_t count, size_t size) {
            for (uint32_t k = 0; k < count; ++k) {
                const Reloc& r = relocs[k];
                if (r.offset >= size) continue;
                mix(&r, sizeof r);
                uint64_t value = 0;
                if (is_value_hole(r.hole) && value_of(p, r, packet, last_len, counter, value))
                    mix(&value, sizeof value);
            }
        };
        relocs(p.relocs, s.reloc_count, p.size);
        mix(p.cold, s.cold_size);
        relocs(p.cold_relocs, s.cold_reloc_count, s.cold_size);
    }
    return h ? h : 1;
}

CpTranslator::Block CpTranslator::translate(uint32_t pc, Memory& code, uint32_t counter, const Config& config) {
#if defined(__EMSCRIPTEN__)
    return translate(pc, code, counter, config, nullptr);
}

CpTranslator::Block CpTranslator::translate(uint32_t pc, Memory& code, uint32_t counter, const Config& config,
                                            Batch* batch) {
#endif
    Block out;
    if (!scan(pc, code, config, out)) return out;
    threshold_ = out.threshold = tier_threshold(config.threshold, config.size_ref, out.packets);
    exit_piece_ = -1;
#if defined(__EMSCRIPTEN__)
    loop_all_ = false;
#endif
    if (config.self_loop && config.chaining && !config.dry) {
        exit_piece_ = int(pieces_.size());
#if defined(__EMSCRIPTEN__)
        loop_all_ = batch && !past_guard_;
        const uint32_t* exits = past_guard_ || loop_all_ ? kSelfExitsLoop : kSelfExits;
#else
        const uint32_t* exits = past_guard_ ? kSelfExitsLoop : kSelfExits;
#endif
        pieces_.push_back(CpTranslator::piece(kStencils[exits[config.count_chained]], pc, 0, -1));
    } else {
        past_guard_ = false;
    }
#if defined(__EMSCRIPTEN__)
    return emit_wasm(pc, counter, config, out, batch);
#else
    size_t total = 0, cold_total = 0;
    size_t open = 0;     // the first piece whose hot trampolines are not placed
    size_t pending = 0;  // and the trampolines of pieces [open, i]
    // Those of pieces [open, end) after the code so far.
    auto place_jumps = [&](size_t end) {
        for (; open < end; ++open) {
            pieces_[open].jumps_at = total;
            total += pieces_[open].jumps * kBranchTrampoline;
        }
    };
    for (size_t i = 0; i < pieces_.size(); ++i) {
        Piece& p = pieces_[i];
        p.at = total;
        p.cold_at = cold_total;
        cold_total += p.stencil->cold_size;
        p.cold_jumps = trampolines(p.cold_relocs, p.stencil->cold_reloc_count, p.stencil->cold_size);
        p.cold_jumps_at = cold_total;
        cold_total += p.cold_jumps * kBranchTrampoline;
        // The jump to what follows, where it does: the next packet, or the
        // block's own exit.
        const bool to_exit = exit_piece_ >= 0 && int(i) + 1 == exit_piece_;
        const bool drop = to_exit ? (p.stencil->flags & kTailExit) != 0
                                  : (p.stencil->flags & kTailCont) && i + 1 < pieces_.size();
        p.size = p.stencil->size - (drop ? kTailJump : 0);
        total += p.size;
        p.jumps = trampolines(p.relocs, p.stencil->reloc_count, p.size);
        // Far from the first unplaced trampolines' branches: after this
        // piece, which then keeps its jump to the next.
        pending += p.jumps;
        if (pending && i + 1 < pieces_.size() && total + pending * kBranchTrampoline - pieces_[open].at > kIsland) {
            if (drop) {
                p.size += kTailJump;
                total += kTailJump;
            }
            place_jumps(i + 1);
            pending = 0;
        }
    }
    // The rest after the code, where nothing runs into them: the block's
    // last piece ends in a jump.
    place_jumps(pieces_.size());
    const size_t hot_size = total;
    const size_t cold_size = cold_total;
    // The helpers' trampolines first: their code takes room in the arena
    // (once per translator).
    if (!helper_targets()) {
        out.why = "no helper trampolines";
        return out;
    }
    JitWriteScope write;  // what follows writes code only
    Arena* arena = arena_for(hot_size + cold_size + 16);
    // Cold code within B/BL's reach of the hot code and the helper jumps and
    // exits it goes to: an arena's code as a whole, on AArch64 (Shared).
    if (arena && cold_total && kReach != SIZE_MAX && arena->arena->codeSpan() > CodeArena::kBranchReach) {
        out.why = "cold code out of reach";
        return out;
    }
    const Support* support = arena ? support_for(*arena) : nullptr;
    uint8_t* at = support ? arena->arena->allocate(CodeArena::Region::Hot, hot_size, 16) : nullptr;
    uint8_t* cold = at && cold_size ? arena->arena->allocate(CodeArena::Region::Cold, cold_size, 16) : nullptr;
    if (!at || (cold_size && !cold)) {
        out.why = "no room";
        return out;
    }
    const uint64_t last_len = last_len_;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        const Piece* next = i + 1 < pieces_.size() ? &pieces_[i + 1] : nullptr;
        if (!place(*support, at, cold, pieces_[i], next, pieces_[i].packet, last_len, counter, config)) {
            // The code is never run (nothing points at it).
            fill_trap(at, hot_size);
            if (cold) fill_trap(cold, cold_size);
            out.why = "relocation";
            return out;
        }
    }
    llvm::sys::Memory::InvalidateInstructionCache(at, hot_size);
    if (cold) llvm::sys::Memory::InvalidateInstructionCache(cold, cold_size);
    out.fn = reinterpret_cast<BbFunc>(at);
    out.size = hot_size;
    out.cold = cold;
    out.cold_size = cold_size;
    return out;
#endif
}

}  // namespace bcore
