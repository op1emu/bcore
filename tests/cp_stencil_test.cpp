// Every copy-and-patch stencil (Core::set_copy_patch) against the LLVM tier
// 0 it stands for: each 16-bit encoding, samples of each key of the 32-bit
// patterns (two register holes the same register in some), and random
// parallel packets, which the translator puts together from the build's
// pieces (or, an illegal one, compiles with LLVM). Each is the first packet
// of a block [packet, RTS], run from the same random guest state in two
// cores, one with stencils and one without. The guest state, memory and
// every host callback (with the packet index it sees, the guest time of
// op1emu's devices) must be the same.
//
// The states make each path run somewhere: loop bottoms at the
// instruction (with every loop count), CC both ways, pointers into RAM
// (aligned or not), into memory with no page (the callbacks), an
// interrupt pending; and loads whose callback raises an exception.
//
// Arguments: a stride and an offset (every n-th instruction from the
// offset only: ctest runs a stride's shards in parallel); "STRIDE parallel
// N" for N random parallel packets alone; or "STRIDE clobber N", every
// n-th instruction and N parallel packets with a Memory whose callbacks
// change a data register: no parallel packet is made of pieces then (they
// need callbacks_preserve_registers()), the rest the same.

#include "core.h"
#include "mmr.h"
#include "bcore_profile.h"
#include "../src/cec.h"
#include "../src/cp_format.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& why) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", why.c_str());
        std::exit(1);
    }
}

constexpr uint32_t kRam = 0x200000;
constexpr uint32_t kCode = 0x10000;   // one instruction every 8 bytes from here
// Or from here (`high`): L1 instruction SRAM, where op1emu's code runs,
// PCs whose MOVZ/MOVK halves (AArch64) are both set; outside the RAM and
// the page table, its code fetched from a buffer of its own unlogged.
constexpr uint32_t kHighCode = 0xffa00000;
constexpr uint32_t kHighSize = 0x60000;
uint32_t g_code = kCode;
constexpr uint32_t kData = 0x180000;  // pointers point here, 64 KiB
constexpr uint32_t kNoPage = 0x400000;  // and here, where the callbacks answer
constexpr uint16_t kRts = 0x0010;

// RAM through the page table; outside it, callbacks that note what they
// were asked and when.
class TraceMemory : public FlatMemory {
public:
    explicit TraceMemory(CpuState* cpu) : FlatMemory(0, kRam, cpu), cpu_(cpu) { enable_page_table(); }
    std::vector<std::string> log;
    // A read outside the RAM raises an exception (a guest one, as a bus
    // error would: did_jump set in the middle of a packet).
    bool read_raises = false;
    // Each callback changes a data register (and says it may).
    bool clobbers = false;
    bool callbacks_preserve_registers() const override { return !clobbers; }

    uint8_t read8(uint32_t addr) const override {
        if (addr < kRam) return FlatMemory::read8(addr);
        return uint8_t(note("r8", addr, 0));
    }
    uint16_t read16(uint32_t addr) const override {
        if (addr < kRam) return FlatMemory::read16(addr);
        if (addr - kHighCode < kHighSize) return uint16_t(high_[addr - kHighCode] | high_[addr - kHighCode + 1] << 8);
        return uint16_t(note("r16", addr, 0));
    }
    uint32_t read32(uint32_t addr) const override {
        if (addr < kRam) return FlatMemory::read32(addr);
        return note("r32", addr, 0);
    }
    void write8(uint32_t addr, uint8_t v) override {
        if (addr < kRam) return FlatMemory::write8(addr, v);
        note("w8", addr, v);
    }
    void write16(uint32_t addr, uint16_t v) override {
        if (addr < kRam) return FlatMemory::write16(addr, v);
        if (addr - kHighCode < kHighSize) {
            high_[addr - kHighCode] = uint8_t(v);
            high_[addr - kHighCode + 1] = uint8_t(v >> 8);
            return;
        }
        note("w16", addr, v);
    }
    void write32(uint32_t addr, uint32_t v) override {
        if (addr < kRam) return FlatMemory::write32(addr, v);
        note("w32", addr, v);
    }
    void data_alignment_fault(uint32_t pc, uint32_t addr, uint32_t status) override {
        note("align", addr, pc ^ status);
    }

private:
    uint32_t note(const char* what, uint32_t addr, uint32_t value) const {
        char line[96];
        snprintf(line, sizeof line, "%s %08x %08x packets=%u", what, addr, value, cpu_->packets);
        const_cast<TraceMemory*>(this)->log.push_back(line);
        if (clobbers) cpu_->dpregs[(addr >> 2) & 7] ^= 0x5a5a0001u;
        if (what[0] == 'r' && read_raises) cec_exception(cpu_, 0x26, const_cast<TraceMemory*>(this));
        return addr * 0x9e3779b1u;
    }
    CpuState* cpu_;
    std::vector<uint8_t> high_ = std::vector<uint8_t>(kHighSize + 2);
};

struct Side {
    CpuState cpu{};
    std::unique_ptr<TraceMemory> mem;
    std::unique_ptr<Core> core;
    Side(bool copy_patch, bool clobbers) {
        mem = std::make_unique<TraceMemory>(&cpu);
        mem->clobbers = clobbers;
        core = std::make_unique<Core>(&cpu, mem.get());
        core->set_chaining(true, false);
        core->set_tiering(0x7fffffff);  // tier 0 throughout
        core->set_copy_patch(copy_patch);
        require(core->init(1), "init");
    }
};

// The fields compared: the guest state and what a run leaves for the host.
std::vector<uint8_t> image(const CpuState& cpu) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&cpu);
    std::vector<uint8_t> out(bytes, bytes + offsetof(CpuState, page_table));
    auto add = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    add(&cpu.chain_packets, sizeof cpu.chain_packets);
    add(&cpu.chain_blocks, sizeof cpu.chain_blocks);
    add(&cpu.chain_would, sizeof cpu.chain_would);
    add(&cpu.idle, sizeof cpu.idle);
    return out;
}

uint32_t pointer(std::mt19937& rng) {
    switch (rng() % 8) {
    case 0: return kNoPage + (rng() & 0xfffc);
    case 1: return kData + (rng() & 0xffff);  // any alignment
    case 2: return kData + 0x8000;
    default: return kData + 0x1000 + (rng() & 0xdffc);
    }
}

void random_state(CpuState& cpu, std::mt19937& rng, uint32_t pc) {
    CpuState fresh{};
    cpu = fresh;
    for (int i = 0; i < 8; ++i) cpu.dpregs[i] = rng() % 4 ? rng() : rng() % 64;
    for (int i = 8; i < 14; ++i) cpu.dpregs[i] = pointer(rng);
    cpu.dpregs[14] = kData + 0x8000;  // SP
    cpu.dpregs[15] = kData + 0x9000;  // FP
    for (int i = 0; i < 4; ++i) {
        cpu.iregs[i] = pointer(rng);
        cpu.mregs[i] = rng() % 2 ? 4 * (rng() % 8) : rng();
        cpu.bregs[i] = cpu.iregs[i] & ~0xffu;
        cpu.lregs[i] = rng() % 4 ? 0 : 0x40 + 4 * (rng() % 16);
    }
    cpu.ax[0] = rng() & 0xff;
    cpu.aw[0] = rng();
    cpu.ax[1] = rng() & 0xff;
    cpu.aw[1] = rng();
    cpu.rets = 0x8000;
    cpu.cc = rng() % 2;
    cpu.az = rng() % 2;
    cpu.an = rng() % 2;
    cpu.ac0 = rng() % 2;
    cpu.v = rng() % 2;
    cpu.av0 = rng() % 2;
    // The loops: one ends at the instruction now and then, with a count of
    // 0, 1 or more.
    for (int i = 0; i < 2; ++i) {
        cpu.lb[i] = rng() % 4 == 0 ? pc : 0x9000;
        cpu.lt[i] = g_code + 8 * (rng() % 64);
        cpu.lc[i] = rng() % 3 == 0 ? 0 : rng() % 3 == 0 ? 1 : 2 + rng() % 5;
    }
}

// A packet: its halfwords, 1 to 4.
struct Instruction {
    uint64_t insn;
    int len;  // bytes
    // A packet whose load reaches P0 at an address the callbacks answer, and
    // whose callback raises an exception, in an interrupt's handler (where
    // the exception leaves the registers alone, as the callbacks promise).
    bool raises = false;
    bool body = false;  // a slot 0 piece's key, NOP slots: made of the build's pieces
};

std::vector<Instruction> instructions() {
    std::vector<Instruction> out;
    for (uint32_t w = 0; w < 0x10000; ++w)
        if (bcore_cp::kIndex16[w]) out.push_back({w, 2});
    std::mt19937 rng(7);
    for (uint32_t p = 0; p < bcore_cp::kPatterns32Count; ++p) {
        const auto& pattern = bcore_cp::kPatterns32[p];
        if (pattern.selected < 0) continue;
        const auto& sel = bcore_cp::kSelected32[pattern.selected];
        const uint32_t free_bits = ~pattern.mask & ~sel.key_mask;
        const unsigned keys = 1u << __builtin_popcount(sel.key_mask);
        for (uint32_t key = 0; key < keys; ++key) {
            if (!bcore_cp::kKeys32[sel.keys + key]) continue;
            uint32_t spread = 0, v = key;
            for (uint32_t m = sel.key_mask; m; m &= m - 1, v >>= 1)
                if (v & 1) spread |= m & -m;
            unsigned reg_count = 0;
            while (reg_count < bcore_cp::kRegHoles && (sel.regs[reg_count].flags & bcore_cp::kFormulaUsed)) ++reg_count;
            for (int sample = 0; sample < 4; ++sample) {
                uint32_t insn = pattern.value | spread | (rng() & free_bits);
                // Register holes: two of them the same register, or all.
                auto set_reg = [&](unsigned k, uint32_t r) {
                    insn = (insn & ~(7u << sel.regs[k].lsb)) | (r << sel.regs[k].lsb);
                };
                if (reg_count >= 2 && sample == 2) {
                    const unsigned a = rng() % reg_count, b = (a + 1 + rng() % (reg_count - 1)) % reg_count;
                    set_reg(b, (insn >> sel.regs[a].lsb) & 7);
                } else if (reg_count >= 2 && sample == 3) {
                    const uint32_t r = rng() % 8;
                    for (unsigned k = 0; k < reg_count; ++k) set_reg(k, r);
                }
                // The decoder takes the first pattern that matches.
                uint32_t first = 0;
                while ((insn & bcore_cp::kPatterns32[first].mask) != bcore_cp::kPatterns32[first].value) ++first;
                if (first != p) continue;
                // A key with the multi-issue bit: slot 0's piece, in a packet
                // with NOP slots.
                const uint32_t entry = bcore_cp::kKeys32[sel.keys + key];
                if (bcore_cp::kStencils[entry - 1].flags & bcore_cp::kBody)
                    out.push_back({uint64_t(insn) << 32, 8, false, true});
                else
                    out.push_back({insn, 4});
            }
        }
    }
    return out;
}

// Parallel packets: a DSP instruction (any 32-bit pattern of 0xc000, the
// multi-issue bit set) and two 16-bit slots (mostly NOPs, loads and stores
// and words the slot takes, as the guest's are), legal or not.
// Whether slot 0 of a parallel packet whose first 32 bits are insn has a
// body (its key's stencil, as CpTranslator::scan finds it): the packet is
// made of pieces then, if anything. Otherwise slot 0 does not take the
// instruction, and the packet raises its exception as a lone stencil.
bool slot0_has_body(uint32_t insn) {
    using namespace bcore_cp;
    for (uint32_t p = 0; p < kPatterns32Count; ++p) {
        if ((insn & kPatterns32[p].mask) != kPatterns32[p].value) continue;
        if (kPatterns32[p].selected < 0) return false;
        const Selected32& sel = kSelected32[kPatterns32[p].selected];
        uint32_t key = 0;
        unsigned bit = 0;
        for (uint32_t m = sel.key_mask; m; m &= m - 1, ++bit)
            if (insn & m & -m) key |= 1u << bit;
        const uint32_t id = kKeys32[sel.keys + key];
        return id && (kStencils[id - 1].flags & kBody);
    }
    return false;
}

std::vector<Instruction> parallel_instructions(size_t parallel) {
    std::vector<Instruction> out;
    std::mt19937 rng(11);
    auto is16 = [](uint16_t w) {
        return (w & 0xc000) != 0xc000 || (w & 0xff00) == 0xf800 || (w & 0xff00) == 0xf900 || (w & 0xff00) == 0xff00;
    };
    std::vector<uint32_t> dsp;
    for (uint32_t p = 0; p < bcore_cp::kPatterns32Count; ++p)
        if ((bcore_cp::kPatterns32[p].value >> 28) == 0xc) dsp.push_back(p);
    // A slot's word: a NOP, a load or store, one the slot takes (the
    // build's pieces), or any.
    std::vector<uint16_t> legal[2];
    for (uint32_t w = 0; w < 0x10000; ++w)
        for (int k = 0; k < 2; ++k)
            if (bcore_cp::kSlots[k][w].stencil) legal[k].push_back(uint16_t(w));
    auto slot_word = [&](int k) -> uint16_t {
        switch (rng() % 6) {
        case 0: case 1: return 0x0000;                     // NOP
        case 2: return uint16_t(0x8000 | (rng() & 0x1fff));  // loads and stores
        case 3: case 4: return legal[k][rng() % legal[k].size()];
        default:
            for (;;) {
                const uint16_t w = uint16_t(rng());
                if (is16(w)) return w;
            }
        }
    };
    const size_t first_parallel = out.size();
    while (out.size() - first_parallel < parallel) {
        const auto& pattern = bcore_cp::kPatterns32[dsp[rng() % dsp.size()]];
        const uint32_t insn = pattern.value | (rng() & ~pattern.mask) | 0x08000000u;
        if ((insn >> 16 & 0xe800) == 0xe800) continue;  // not a multi-issue encoding
        const uint16_t s1 = slot_word(0), s2 = slot_word(1);
        out.push_back({uint64_t(insn) << 32 | uint64_t(s1) << 16 | s2, 8});
    }
    // MNOP || R0 = [P0] || a slot 2, its load raising an exception; and
    // R0 = [P0] alone.
    for (int i = 0; i < 200 && parallel; ++i) out.push_back({0x9000, 2, true});
    for (int i = 0; i < 2000 && parallel; ++i)
        out.push_back({0xc8031800ull << 32 | uint64_t(0x9000) << 16 | slot_word(1), 8, true});
    return out;
}

uint64_t hash_data(const TraceMemory& mem) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = kData; a < kData + 0x10000; ++a) h = (h ^ mem.raw()[a]) * 1099511628211ull;
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    // [STRIDE [OFFSET]] [parallel N | clobber N] [high]: every STRIDE-th
    // instruction from OFFSET; N random parallel packets alone; or every
    // STRIDE-th instruction and N parallel packets, callbacks clobbering;
    // with `high`, the code at kHighCode.
    if (argc > 1 && !strcmp(argv[argc - 1], "high")) {
        g_code = kHighCode;
        --argc;
    }
    const unsigned stride = argc > 1 ? std::max(1, atoi(argv[1])) : 1;
    const bool parallel_only = argc > 2 && !strcmp(argv[2], "parallel");
    const bool clobbers = argc > 2 && !strcmp(argv[2], "clobber");
    const size_t offset = argc > 2 && !parallel_only && !clobbers ? size_t(atol(argv[2])) : 0;
    const size_t parallel = (parallel_only || clobbers) && argc > 3 ? size_t(atol(argv[3])) : 24000;
    std::vector<Instruction> todo;
    if (!parallel_only) {
        const auto lone = instructions();
        for (size_t i = offset; i < lone.size(); i += stride) todo.push_back(lone[i]);
    }
    {
        const auto more = parallel_instructions(parallel);
        for (size_t i = clobbers ? 0 : offset; i < more.size(); i += clobbers ? 1 : stride) todo.push_back(more[i]);
    }
    printf("cp_stencil_test: %zu instructions\n", todo.size());

    // A core holds at most TierTable::kCounters tier-0 blocks: a fresh pair
    // for each chunk.
    constexpr size_t kChunk = 20000;
    size_t compared = 0, made = 0, refused = 0;
    for (size_t begin = 0; begin < todo.size(); begin += kChunk) {
        Side cp(true, clobbers), llvm(false, clobbers);
        const size_t end = std::min(todo.size(), begin + kChunk);
        for (size_t i = begin; i < end; ++i) {
            const Instruction& in = todo[i];
            const uint32_t pc = g_code + 16 * uint32_t(i - begin);
            for (Side* side : {&cp, &llvm}) {
                for (int w = 0; w < in.len / 2; ++w)
                    side->mem->write16(pc + 2 * w, uint16_t(in.insn >> (16 * (in.len / 2 - 1 - w))));
                side->mem->write16(pc + in.len, kRts);
            }
            std::mt19937 rng(uint32_t(in.insn * 2654435761u + (in.insn >> 32) * 40503u + in.len));
            CpuState start;
            random_state(start, rng, pc);
            const bool pending = rng() % 8 == 0;
            if (in.raises) start.dpregs[8] = kNoPage + 0x40;
            const uint64_t before = cp.core->stats().copy_patch_blocks;
            for (Side* side : {&cp, &llvm}) {
                side->cpu = start;
                side->mem->log.clear();
                side->mem->read_raises = in.raises;
                cec_init();
                evt_init();
                if (pending) cec_raise(&side->cpu, 14);
                if (in.raises) {  // in interrupt 7's handler, supervisor
                    evt_write(EVT_BASE + 4 * 3, 0x8100);
                    evt_write(EVT_BASE + 4 * 7, 0x8180);
                    cec_mmr_write(&side->cpu, CEC_MMR_BASE + 4, 1u << 7);
                    cec_raise(&side->cpu, 7);
                    cec_return_rti(&side->cpu);
                }
                side->cpu.chain_budget = 1;  // this block only
                require(side->core->run(pc), "run");
            }
            const uint64_t got = cp.core->stats().copy_patch_blocks - before;
            made += got;
            if (!got && (in.len < 8 || (in.body && !clobbers))) {  // the build has stencils for each
                fprintf(stderr, "no stencil block for %0*llx\n", in.len * 2, (unsigned long long)in.insn);
                ++refused;
            }
            if (got && in.len == 8 && clobbers && slot0_has_body(uint32_t(in.insn >> 32))) {
                fprintf(stderr, "parallel %016llx made of pieces, callbacks clobbering\n", (unsigned long long)in.insn);
                ++refused;
            }
            char what[64];
            snprintf(what, sizeof what, "%s %0*llx at %08x", in.len == 2 ? "16-bit" : in.len == 4 ? "32-bit" : "parallel",
                     in.len * 2, (unsigned long long)in.insn, pc);
            require(image(cp.cpu) == image(llvm.cpu), std::string("guest state after ") + what);
            require(cp.mem->log == llvm.mem->log, std::string("host callbacks of ") + what);
            require(hash_data(*cp.mem) == hash_data(*llvm.mem), std::string("memory after ") + what);
            ++compared;
        }
    }
    printf("cp_stencil_test: %zu compared, %zu made of stencils\n", compared, made);
#if BCORE_ENABLE_PROFILE  // the count is a profiling counter
    require(refused == 0, "a block made of stencils or not against the rules above");
    // Parallel packets the build has no pieces for (an illegal slot,
    // SEARCH) compile with LLVM, compared all the same.
#endif
    printf("PASS\n");
    return 0;
}
