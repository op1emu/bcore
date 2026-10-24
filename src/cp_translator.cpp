#include "cp_translator.h"

#include <cstddef>
#include <cstring>
#include <string>

#include "cec.h"
#include "code_arena.h"
#include "syscall_emu.h"
#include "tier_table.h"

using namespace bcore_cp;

namespace {

// ---- The exception bridge (cp_translator.h) ----

thread_local CpuState* t_cpu = nullptr;
thread_local std::exception_ptr t_caught;

// In a catch block: keep the exception and stop the run.
void caught() noexcept {
    if (!t_caught) t_caught = std::current_exception();
    if (t_cpu) {
        t_cpu->did_jump = true;
        t_cpu->chain_budget = 0;
    }
}

uint8_t cp_mem_read8(Memory* mem, uint32_t addr) noexcept {
    try { return mem_read8(mem, addr); } catch (...) { caught(); return 0; }
}
uint16_t cp_mem_read16(Memory* mem, uint32_t addr) noexcept {
    try { return mem_read16(mem, addr); } catch (...) { caught(); return 0; }
}
uint32_t cp_mem_read32(Memory* mem, uint32_t addr) noexcept {
    try { return mem_read32(mem, addr); } catch (...) { caught(); return 0; }
}
void cp_mem_write8(Memory* mem, uint32_t addr, uint8_t value) noexcept {
    try { mem_write8(mem, addr, value); } catch (...) { caught(); }
}
void cp_mem_write16(Memory* mem, uint32_t addr, uint16_t value) noexcept {
    try { mem_write16(mem, addr, value); } catch (...) { caught(); }
}
void cp_mem_write32(Memory* mem, uint32_t addr, uint32_t value) noexcept {
    try { mem_write32(mem, addr, value); } catch (...) { caught(); }
}
void cp_cec_data_alignment(CpuState* cpu, Memory* mem, uint32_t pc, uint32_t addr, uint32_t flags) noexcept {
    try { cec_data_alignment(cpu, mem, pc, addr, flags); } catch (...) { caught(); }
}
void cp_cec_exception(CpuState* cpu, uint32_t excp, Memory* mem) noexcept {
    try { cec_exception(cpu, excp, mem); } catch (...) { caught(); }
}
void cp_bfin_syscall(CpuState* cpu, Memory* mem) noexcept {
    try { bfin_syscall(cpu, mem); } catch (...) { caught(); }
}

// What a stencil's helper call reaches, in the order of kHelpers.
void* const kHelperFns[] = {
    reinterpret_cast<void*>(&cp_mem_read8),
    reinterpret_cast<void*>(&cp_mem_read16),
    reinterpret_cast<void*>(&cp_mem_read32),
    reinterpret_cast<void*>(&cp_mem_write8),
    reinterpret_cast<void*>(&cp_mem_write16),
    reinterpret_cast<void*>(&cp_mem_write32),
    reinterpret_cast<void*>(&cp_cec_data_alignment),
    reinterpret_cast<void*>(&cp_cec_exception),
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
    reinterpret_cast<void*>(&cp_bfin_syscall),
    reinterpret_cast<void*>(&::memset),
    reinterpret_cast<void*>(&::memcpy),
    reinterpret_cast<void*>(&::memmove),
};
static_assert(sizeof(kHelperFns) / sizeof(kHelperFns[0]) == kHelperCount, "a function for every helper");

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

// A 64-bit value hole (the others are 32-bit values, of which a 32-bit
// field holds the low half, whichever way it extends them).
bool wide(uint8_t hole) { return hole == kHoleCountOffset; }

bool patch(uint8_t* at, const Reloc& r, uint64_t value) {
    uint8_t* place = at + r.offset;
    switch (r.kind) {
    case kAbs32:
    case kAbs32S: {
        const int64_t v = int64_t(value) + r.addend;
        if (wide(r.hole) && (r.kind == kAbs32 ? (v < 0 || v > int64_t(UINT32_MAX)) : (v < INT32_MIN || v > INT32_MAX)))
            return false;
        const uint32_t field = uint32_t(v);
        memcpy(place, &field, 4);
        return true;
    }
    case kPcRel32: {
        const int64_t v = int64_t(value) + r.addend - int64_t(reinterpret_cast<uintptr_t>(place));
        if (v < INT32_MIN || v > INT32_MAX) return false;
        const int32_t field = int32_t(v);
        memcpy(place, &field, 4);
        return true;
    }
    case kAbs64: {
        const uint64_t field = value + uint64_t(int64_t(r.addend));
        memcpy(place, &field, 8);
        return true;
    }
    }
    return false;
}

constexpr size_t kTrampoline = 16;  // jmp *0(%rip); .quad target; padding

}  // namespace

CpTranslator::RunScope::RunScope(CpuState* cpu) : previous_(t_cpu) { t_cpu = cpu; }

CpTranslator::RunScope::~RunScope() { t_cpu = previous_; }

void CpTranslator::RunScope::rethrow() {
    if (!t_caught) return;
    std::exception_ptr e = t_caught;
    t_caught = nullptr;
    std::rethrow_exception(e);
}

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
                if (!compose_parallel(cur, insn, selected, kStencils[id - 1], code, read, index, why))
                    return fail(why);
                for (size_t i = first; i < pieces_.size(); ++i)
                    cycles |= (pieces_[i].stencil->flags & kCycles) != 0;
                cur += 8;
                last_bytes = 8;
                continue;
            }
        }
        if (!id) return fail(is16(w) ? "no stencil" : is_parallel(w) ? "parallel" : "32-bit");
        Piece piece = CpTranslator::piece(kStencils[id - 1], cur, insn, selected);
        const uint16_t flags = piece.stencil->flags;
        if ((flags & kIdle) && config.idle_waits) return fail("idle");
        cycles |= (flags & kCycles) != 0;
        piece.packet = index;
        pieces_.push_back(piece);
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
    pieces_[0] = CpTranslator::piece(kStencils[kPrologue[config.host_cycles && cycles]], pc, 0, -1);
    return true;
}

bool CpTranslator::compose_parallel(uint32_t pc, uint32_t insn, int selected, const Stencil& body, Memory& code,
                                    const std::function<uint16_t(uint32_t)>& read, uint32_t packet, const char*& why) {
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
    const SlotEntry& e1 = kSlots[(algn * 2) * 2][s1];
    if (!e1.stencil) {
        why = "parallel: no stencil for slot 1";
        return false;
    }
    const Stencil& st1 = kStencils[e1.stencil - 1];
    const SlotEntry& e2 = kSlots[(algn * 2 + ((st1.flags & kDjUnknown) ? 1 : 0)) * 2 + 1][s2];
    if (!e2.stencil) {
        why = "parallel: no stencil for slot 2";
        return false;
    }
    const Stencil& st2 = kStencils[e2.stencil - 1];
    const unsigned tail = ((st2.flags & kDjUnknown) ? 2 : 0) + (((st1.flags | st2.flags) & kSlotStore) ? 1 : 0);
    auto add = [&](const Stencil& s, uint32_t word, int sel) {
        pieces_.push_back(CpTranslator::piece(s, pc, word, sel));
        pieces_.back().packet = packet;
    };
    if (!(st1.flags & kNop)) add(st1, 0, -1);
    if (!(st2.flags & kNop)) add(st2, 0, -1);
    add(body, insn, selected);
    if (e1.commit) add(kStencils[e1.commit - 1], 0, -1);
    if (e2.commit) add(kStencils[e2.commit - 1], 0, -1);
    add(kStencils[kParallelTail[tail]], 0, -1);
    return true;
}

CpTranslator::Arena* CpTranslator::arena_for(size_t size) {
    size_t support = kHelperCount * kTrampoline;
    for (uint32_t e : kExits) support += kStencils[e].size + 16;
    auto arena = jit_.codeArena(size + support);
    if (!arena) return nullptr;
    for (auto& a : arenas_)
        if (a->arena == arena) return a.get();
    auto a = std::make_unique<Arena>();
    a->arena = arena;
    for (unsigned i = 0; i < kHelperCount; ++i) {
        uint8_t* at = arena->allocate(CodeArena::Region::Hot, kTrampoline, kTrampoline);
        if (!at) return nullptr;
        const uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};  // jmp *0(%rip)
        memcpy(at, jmp, sizeof jmp);
        memcpy(at + 6, &kHelperFns[i], 8);
        memset(at + 14, 0xcc, kTrampoline - 14);
        a->helpers[i] = at;
        if (listener_) listener_(("bcore_cp_jump_" + std::string(kHelpers[i])).c_str(), at, kTrampoline);
    }
    for (int e = 0; e < 5; ++e) {
        const Stencil& s = kStencils[kExits[e]];
        uint8_t* at = arena->allocate(CodeArena::Region::Hot, s.size, 16);
        if (!at || s.reloc_count || s.cold_size) return nullptr;  // the exits' code fills in nothing, in one piece
        memcpy(at, kCode + s.code, s.size);
        a->exits[e] = at;
        if (listener_) listener_("bcore_cp_exit", at, s.size);
    }
    arenas_.push_back(std::move(a));
    return arenas_.back().get();
}

bool CpTranslator::place(Arena& arena, uint8_t* code, uint8_t* cold, const Piece& piece, const Piece* next,
                         uint32_t packet, uint64_t field0, uint32_t counter, const Config& config) {
    const Stencil& s = *piece.stencil;
    uint8_t* hot_at = code + piece.at;
    uint8_t* cold_at = cold + piece.cold_at;
    memcpy(hot_at, piece.code, piece.size);
    if (s.cold_size) memcpy(cold_at, piece.cold, s.cold_size);
    const int exit = !config.chaining ? 0 : config.dry ? (config.count_chained ? 4 : 3) : (config.count_chained ? 2 : 1);
    // Each part's relocations; the hot part's last jump may be dropped.
    auto fill = [&](uint8_t* at, const Reloc* relocs, uint32_t count, size_t size) {
        for (uint32_t i = 0; i < count; ++i) {
            const Reloc& r = relocs[i];
            if (r.offset >= size) continue;  // in the dropped jump to what follows
            uint64_t value = 0;
            if (r.hole < kHoleCont) {
                if (!value_of(piece, r, packet, field0, counter, value)) return false;
            } else if (r.hole == kHoleCont) {
                if (!next) return false;
                value = reinterpret_cast<uintptr_t>(code + next->at);
            } else if (r.hole == kHoleExit) {
                value = reinterpret_cast<uintptr_t>(exit_piece_ >= 0 ? code + pieces_[exit_piece_].at : arena.exits[exit]);
            } else if (r.hole == kHoleSelf) {
                value = reinterpret_cast<uintptr_t>(code);
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
            if (!patch(at, r, value)) return false;
        }
        return true;
    };
    return fill(hot_at, piece.relocs, s.reloc_count, piece.size) &&
           fill(cold_at, piece.cold_relocs, s.cold_reloc_count, s.cold_size);
}

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
                if (r.hole < kHoleCont && value_of(p, r, packet, last_len, counter, value)) mix(&value, sizeof value);
            }
        };
        relocs(p.relocs, s.reloc_count, p.size);
        mix(p.cold, s.cold_size);
        relocs(p.cold_relocs, s.cold_reloc_count, s.cold_size);
    }
    return h ? h : 1;
}

CpTranslator::Block CpTranslator::translate(uint32_t pc, Memory& code, uint32_t counter, const Config& config) {
    Block out;
    if (!scan(pc, code, config, out)) return out;
    threshold_ = out.threshold = tier_threshold(config.threshold, config.size_ref, out.packets);
    exit_piece_ = -1;
    if (config.self_loop && config.chaining && !config.dry) {
        exit_piece_ = int(pieces_.size());
        pieces_.push_back(CpTranslator::piece(kStencils[kSelfExits[config.count_chained]], pc, 0, -1));
    }
    size_t total = 0, cold_total = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        Piece& p = pieces_[i];
        p.at = total;
        p.cold_at = cold_total;
        cold_total += p.stencil->cold_size;
        // The jump to what follows, where it does: the next packet, or the
        // block's own exit.
        const bool to_exit = exit_piece_ >= 0 && int(i) + 1 == exit_piece_;
        const bool drop = to_exit ? (p.stencil->flags & kTailExit) != 0
                                  : (p.stencil->flags & kTailCont) && i + 1 < pieces_.size();
        p.size = p.stencil->size - (drop ? 5 : 0);
        total += p.size;
    }
    Arena* arena = arena_for(total + cold_total + 16);
    uint8_t* at = arena ? arena->arena->allocate(CodeArena::Region::Hot, total, 16) : nullptr;
    uint8_t* cold = at && cold_total ? arena->arena->allocate(CodeArena::Region::Cold, cold_total, 16) : nullptr;
    if (!at || (cold_total && !cold)) {
        out.why = "no room";
        return out;
    }
    const uint64_t last_len = last_len_;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        const Piece* next = i + 1 < pieces_.size() ? &pieces_[i + 1] : nullptr;
        if (!place(*arena, at, cold, pieces_[i], next, pieces_[i].packet, last_len, counter, config)) {
            // Filled with int3: the code is never run (nothing points at it).
            memset(at, 0xcc, total);
            if (cold) memset(cold, 0xcc, cold_total);
            out.why = "relocation";
            return out;
        }
    }
    out.fn = reinterpret_cast<BbFunc>(at);
    out.size = total;
    out.cold = cold;
    out.cold_size = cold_total;
    return out;
}
