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

bool CpTranslator::scan(uint32_t pc, Memory& code, const Config& config, Block& out) {
    pieces_.clear();
    words_.clear();
    auto read = [&](uint32_t addr) {
        const uint16_t w = code.read16(addr);
        words_.emplace_back(addr, w);
        return w;
    };
    pieces_.push_back({nullptr, pc, 0, -1, 0, 0});  // the prologue, chosen below
    bool cycles = false;
    uint32_t cur = pc;
    for (uint32_t k = 0; k < config.max_packets; ++k) {
        const uint16_t w = read(cur);
        uint32_t id = 0, insn = w;
        int selected = -1;
        if (is16(w)) {
            id = kIndex16[w];
        } else {
            if (is_parallel(w)) {
                out.why = "parallel";
                return false;
            }
            insn = uint32_t(w) << 16 | read(cur + 2);
            const int p = match32(insn);
            if (p < 0 || kPatterns32[p].selected < 0) {
                out.why = "32-bit";
                return false;
            }
            selected = kPatterns32[p].selected;
            const Selected32& sel = kSelected32[selected];
            id = kKeys32[sel.keys + gather(insn, sel.key_mask)];
        }
        if (!id) {
            out.why = "no stencil";
            return false;
        }
        const Stencil& s = kStencils[id - 1];
        if ((s.flags & kIdle) && config.idle_waits) {
            out.why = "idle";
            return false;
        }
        cycles |= (s.flags & kCycles) != 0;
        pieces_.push_back({&s, cur, insn, selected, 0, 0});
        cur += s.bytes;
        if (s.flags & kTerminates) break;
    }
    out.fallthrough_pc = cur;
    out.packets = uint32_t(pieces_.size() - 1);
    if (!(pieces_.back().stencil->flags & kTerminates))
        pieces_.push_back({&kStencils[kFallthrough], cur, 0, -1, 0, 0});
    pieces_[0].stencil = &kStencils[kPrologue[config.host_cycles && cycles]];
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
        if (!at || s.reloc_count) return nullptr;  // the exits' code fills in nothing
        memcpy(at, kCode + s.code, s.size);
        a->exits[e] = at;
        if (listener_) listener_("bcore_cp_exit", at, s.size);
    }
    arenas_.push_back(std::move(a));
    return arenas_.back().get();
}

bool CpTranslator::place(Arena& arena, uint8_t* code, const Piece& piece, const Piece* next, uint32_t packet,
                         uint64_t field0, uint32_t counter, const Config& config) {
    const Stencil& s = *piece.stencil;
    uint8_t* at = code + piece.at;
    memcpy(at, kCode + s.code, piece.size);
    const int exit = !config.chaining ? 0 : config.dry ? (config.count_chained ? 4 : 3) : (config.count_chained ? 2 : 1);
    for (uint32_t i = 0; i < s.reloc_count; ++i) {
        const Reloc& r = kRelocs[s.relocs + i];
        if (r.offset >= piece.size) continue;  // in the dropped jump to the next packet
        uint64_t value = 0;
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
            value = config.threshold;
        } else if (r.hole == kHoleCont) {
            if (!next) return false;
            value = reinterpret_cast<uintptr_t>(code + next->at);
        } else if (r.hole == kHoleExit) {
            value = reinterpret_cast<uintptr_t>(arena.exits[exit]);
        } else if (r.hole >= kHoleHelper && r.hole < kHoleHelper + kHelperCount) {
            value = reinterpret_cast<uintptr_t>(arena.helpers[r.hole - kHoleHelper]);
        } else {
            return false;
        }
        if (!patch(at, r, value)) return false;
    }
    return true;
}

CpTranslator::Block CpTranslator::translate(uint32_t pc, Memory& code, uint32_t counter, const Config& config) {
    Block out;
    if (!scan(pc, code, config, out)) return out;
    size_t total = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        Piece& p = pieces_[i];
        p.at = total;
        p.size = p.stencil->size - ((p.stencil->flags & kTailCont) && i + 1 < pieces_.size() ? 5 : 0);
        total += p.size;
    }
    Arena* arena = arena_for(total + 16);
    uint8_t* at = arena ? arena->arena->allocate(CodeArena::Region::Hot, total, 16) : nullptr;
    if (!at) {
        out.why = "no room";
        return out;
    }
    // The packet the fall-through stencil follows: its length.
    const uint64_t last_len = pieces_.size() > 1 ? pieces_[pieces_.size() - 2].stencil->bytes : 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        const Piece* next = i + 1 < pieces_.size() ? &pieces_[i + 1] : nullptr;
        const bool fallthrough = pieces_[i].stencil == &kStencils[kFallthrough] && i > 0;
        const uint32_t packet = fallthrough ? uint32_t(i - 1) : uint32_t(i);
        if (!place(*arena, at, pieces_[i], next, packet, last_len, counter, config)) {
            // Filled with int3: the code is never run (nothing points at it).
            memset(at, 0xcc, total);
            out.why = "relocation";
            return out;
        }
    }
    out.fn = reinterpret_cast<BbFunc>(at);
    out.size = total;
    return out;
}
