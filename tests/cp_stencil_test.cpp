// Every copy-and-patch stencil (Core::set_copy_patch) against the LLVM tier
// 0 it stands for: each 16-bit encoding with a stencil, and samples of each
// key of the 32-bit patterns (cp_patterns.def), as the first packet of a
// block [instruction, RTS], run from the same random guest state in two
// cores, one with stencils and one without. The guest state, memory and
// every host callback (with the packet index it sees, the guest time of
// op1emu's devices) must be the same.
//
// The states make each path run somewhere: loop bottoms at the
// instruction (with every loop count), CC both ways, pointers into RAM
// (aligned or not), into memory with no page (the callbacks), an
// interrupt pending.
#include "core.h"
#include "bcore_profile.h"
#include "../src/cec.h"
#include "../src/cp_format.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"

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
constexpr uint32_t kData = 0x180000;  // pointers point here, 64 KiB
constexpr uint32_t kNoPage = 0x400000;  // and here, where the callbacks answer
constexpr uint16_t kRts = 0x0010;

// RAM through the page table; outside it, callbacks that note what they
// were asked and when.
class TraceMemory : public FlatMemory {
public:
    explicit TraceMemory(CpuState* cpu) : FlatMemory(0, kRam, cpu), cpu_(cpu) { enable_page_table(); }
    std::vector<std::string> log;

    uint8_t read8(uint32_t addr) const override {
        if (addr < kRam) return FlatMemory::read8(addr);
        return uint8_t(note("r8", addr, 0));
    }
    uint16_t read16(uint32_t addr) const override {
        if (addr < kRam) return FlatMemory::read16(addr);
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
        return addr * 0x9e3779b1u;
    }
    CpuState* cpu_;
};

struct Side {
    CpuState cpu{};
    std::unique_ptr<TraceMemory> mem;
    std::unique_ptr<Core> core;
    explicit Side(bool copy_patch) {
        mem = std::make_unique<TraceMemory>(&cpu);
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
        cpu.lt[i] = kCode + 8 * (rng() % 64);
        cpu.lc[i] = rng() % 3 == 0 ? 0 : rng() % 3 == 0 ? 1 : 2 + rng() % 5;
    }
}

struct Instruction {
    uint32_t insn;
    int len;
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
            for (int sample = 0; sample < 4; ++sample) {
                const uint32_t insn = pattern.value | spread | (rng() & free_bits);
                // The decoder takes the first pattern that matches.
                uint32_t first = 0;
                while ((insn & bcore_cp::kPatterns32[first].mask) != bcore_cp::kPatterns32[first].value) ++first;
                if (first == p) out.push_back({insn, 4});
            }
        }
    }
    return out;
}

uint64_t hash_data(const TraceMemory& mem) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = kData; a < kData + 0x10000; ++a) h = (h ^ mem.raw()[a]) * 1099511628211ull;
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    // An optional stride runs every n-th instruction only (a quick check).
    const unsigned stride = argc > 1 ? unsigned(atoi(argv[1])) : 1;
    const auto all = instructions();
    std::vector<Instruction> todo;
    for (size_t i = 0; i < all.size(); i += stride) todo.push_back(all[i]);
    printf("cp_stencil_test: %zu instructions\n", todo.size());

    // A core holds at most TierTable::kCounters tier-0 blocks: a fresh pair
    // for each chunk.
    constexpr size_t kChunk = 20000;
    size_t compared = 0, made = 0;
    for (size_t begin = 0; begin < todo.size(); begin += kChunk) {
        Side cp(true), llvm(false);
        const size_t end = std::min(todo.size(), begin + kChunk);
        for (size_t i = begin; i < end; ++i) {
            const Instruction& in = todo[i];
            const uint32_t pc = kCode + 8 * uint32_t(i - begin);
            for (Side* side : {&cp, &llvm}) {
                if (in.len == 2) {
                    side->mem->write16(pc, uint16_t(in.insn));
                    side->mem->write16(pc + 2, kRts);
                } else {
                    side->mem->write16(pc, uint16_t(in.insn >> 16));
                    side->mem->write16(pc + 2, uint16_t(in.insn));
                    side->mem->write16(pc + 4, kRts);
                }
            }
            std::mt19937 rng(in.insn * 2654435761u + in.len);
            CpuState start;
            random_state(start, rng, pc);
            const bool pending = rng() % 8 == 0;
            const uint64_t before = cp.core->stats().copy_patch_blocks;
            for (Side* side : {&cp, &llvm}) {
                side->cpu = start;
                side->mem->log.clear();
                cec_init();
                evt_init();
                if (pending) cec_raise(&side->cpu, 14);
                side->cpu.chain_budget = 1;  // this block only
                require(side->core->run(pc), "run");
            }
            made += cp.core->stats().copy_patch_blocks - before;
            char what[64];
            snprintf(what, sizeof what, "%s %0*x at %08x", in.len == 2 ? "16-bit" : "32-bit", in.len * 2, in.insn,
                     pc);
            require(image(cp.cpu) == image(llvm.cpu), std::string("guest state after ") + what);
            require(cp.mem->log == llvm.mem->log, std::string("host callbacks of ") + what);
            require(hash_data(*cp.mem) == hash_data(*llvm.mem), std::string("memory after ") + what);
            ++compared;
        }
    }
    printf("cp_stencil_test: %zu compared, %zu made of stencils\n", compared, made);
    require(made == compared, "every instruction's block made of stencils");
    printf("PASS\n");
    return 0;
}
