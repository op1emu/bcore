// The stencils the copy-and-patch tier 0 builds at run time
// (Core::set_copy_patch_runtime, cp_runtime.h):
//   - a packet's stencil is built once: the same packet in other blocks, at
//     other PCs, after Core::invalidate, finds it;
//   - a block's missing packets build in one module;
//   - with verify, every stencil is compiled again on its own and the same;
//   - at -O2 the run time builds a 16-bit instruction's stencil as the build
//     did (the same code and relocations as kStencils);
//   - packets refused once are not tried again, and their blocks compile
//     with LLVM;
//   - off, a block with a parallel packet compiles with LLVM;
//   - a run-time stencil has no field holes (an immediate is a constant in
//     its code);
//   - a translation reads no more of a block than the packet that ends it,
//     though that packet had no stencil yet.
#include "core.h"
#include "../src/cec.h"
#include "../src/cp_format.h"
#include "../src/cp_runtime.h"
#include "../src/evt.h"
#include "../tools/flat_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
constexpr uint32_t kCode = 0x10000;
constexpr uint16_t kRts = 0x0010;
// Two legal parallel packets: a dsp32alu ADD32 || NOP || NOP, and another
// ADD32 || R3 = [P0++] || NOP (an 8-byte packet each).
constexpr uint16_t kParallelA[4] = {0xcc04, 0x000a, 0x0000, 0x0000};
constexpr uint16_t kParallelB[4] = {0xcc04, 0x165c, 0x9003, 0x0000};
// R0 = SIGNBITS R1 (dsp32shift), not one of cp_patterns.def.
constexpr uint16_t kPlain32[2] = {0xc606, 0x0001};

struct Rig {
    CpuState cpu{};
    std::unique_ptr<FlatMemory> mem;
    std::unique_ptr<Core> core;
    explicit Rig(const CpRuntimeSettings& settings, bool copy_patch = true) {
        mem = std::make_unique<FlatMemory>(0, kRam, &cpu);
        mem->enable_page_table();
        core = std::make_unique<Core>(&cpu, mem.get());
        core->set_chaining(true, false);
        core->set_tiering(0x7fffffff);  // tier 0 throughout
        core->set_copy_patch_runtime(settings);
        core->set_copy_patch(copy_patch);
        require(core->init(1), "init");
    }
    // The block at pc: the packets, then RTS.
    void place(uint32_t pc, std::initializer_list<std::vector<uint16_t>> packets) {
        uint32_t at = pc;
        for (const auto& p : packets)
            for (uint16_t w : p) {
                mem->write16(at, w);
                at += 2;
            }
        mem->write16(at, kRts);
    }
    void run(uint32_t pc) {
        cpu.rets = 0x8000;
        mem->write16(0x8000, 0x0020);  // IDLE where RTS goes: no block after this one
        cec_init();
        evt_init();
        cpu.chain_budget = 1;
        require(core->run(pc), "run");
    }
};

std::vector<uint16_t> words(const uint16_t* w, size_t n) { return std::vector<uint16_t>(w, w + n); }

void test_once_per_encoding() {
    Rig rig({});
    rig.place(kCode, {words(kParallelA, 4), words(kPlain32, 2), {0x0000}});
    rig.place(kCode + 0x100, {{0x0000}, words(kParallelA, 4)});
    rig.place(kCode + 0x200, {words(kPlain32, 2), words(kParallelA, 4), words(kParallelB, 4)});
    rig.run(kCode);
    const CpRuntimeStats first = rig.core->cp_runtime_stats();
    require(first.built == 2 && first.batches == 1, "a block's two missing packets: two stencils, one module");
    rig.run(kCode + 0x100);
    require(rig.core->cp_runtime_stats().built == 2, "the same packet at another PC was built again");
    rig.run(kCode + 0x200);
    const CpRuntimeStats third = rig.core->cp_runtime_stats();
    require(third.built == 3 && third.batches == 2, "only the new packet built");
#if BCORE_ENABLE_PROFILE
    require(rig.core->stats().copy_patch_blocks == 3 && rig.core->stats().copy_patch_fallbacks == 0,
            "every block made of stencils");
#endif
    // A new engine keeps them.
    rig.core->invalidate();
    rig.run(kCode + 0x200);
    require(rig.core->cp_runtime_stats().built == 3, "Core::invalidate dropped the stencils built");
    require(rig.core->cp_runtime_stats().refused == 0, "a packet refused");
}

void test_verify() {
    CpRuntimeSettings settings;
    settings.verify = true;
    Rig rig(settings);
    rig.place(kCode, {words(kParallelA, 4), words(kParallelB, 4), words(kPlain32, 2)});
    rig.run(kCode);
    const CpRuntimeStats stats = rig.core->cp_runtime_stats();
    require(stats.built == 3 && stats.refused == 0, "a stencil's code depends on its batch");
}

// At -O2 the run time builds as tools/stencilgen.cpp does: every 16-bit
// encoding of a sample with a stencil from the build gets the same code
// and relocations.
void test_same_as_the_build() {
    bcore_cp::RuntimeStencils rt;
    bcore_cp::RuntimeStencils::Settings settings;
    settings.opt = 2;
    require(rt.configure(settings), "configure");
    std::vector<bcore_cp::RuntimeStencils::Packet> packets;
    for (uint32_t w = 0; w < 0x10000; w += 97) {
        if (!bcore_cp::kIndex16[w]) continue;
        bcore_cp::RuntimeStencils::Packet p;
        p.words[0] = uint16_t(w);
        p.count = 1;
        packets.push_back(p);
    }
    for (size_t at = 0; at < packets.size(); at += 64) {
        for (size_t i = at; i < std::min(packets.size(), at + 64); ++i)
            require(rt.stage(packets[i]).ok, "16-bit " + std::to_string(packets[i].words[0]) + " refused");
        rt.flush();
    }
    size_t same = 0;
    for (const auto& p : packets) {
        const bcore_cp::Stencil& s = bcore_cp::kStencils[bcore_cp::kIndex16[p.words[0]] - 1];
        const auto* r = rt.find(p);
        require(r != nullptr, "no stencil for 16-bit " + std::to_string(p.words[0]));
        const bool code = r->code.size() == s.size && !memcmp(r->code.data(), bcore_cp::kCode + s.code, s.size);
        bool relocs = r->relocs.size() == s.reloc_count;
        for (size_t i = 0; relocs && i < r->relocs.size(); ++i) {
            const auto& a = r->relocs[i];
            const auto& b = bcore_cp::kRelocs[s.relocs + i];
            relocs = a.offset == b.offset && a.hole == b.hole && a.kind == b.kind && a.addend == b.addend;
        }
        require(code && relocs && r->stencil.flags == s.flags && r->stencil.bytes == s.bytes,
                "16-bit " + std::to_string(p.words[0]) + ": not the build's stencil");
        ++same;
    }
    printf("cp_runtime_test: %zu 16-bit stencils the same as the build's\n", same);
}

// A packet outside the range gets none: refused once, its block with LLVM,
// and not tried again.
void test_refused() {
    CpRuntimeSettings settings;
    settings.range_lo = 1;
    settings.range_hi = 0;  // nothing in range
    Rig rig(settings);
    rig.place(kCode, {words(kParallelA, 4)});
    rig.place(kCode + 0x100, {{0x0000}, words(kParallelA, 4)});
    rig.run(kCode);
    rig.run(kCode + 0x100);
    const CpRuntimeStats stats = rig.core->cp_runtime_stats();
    require(stats.built == 0 && stats.refused == 1, "a packet refused once only");
    require(stats.reasons.count("outside the range") == 1, "the reason");
#if BCORE_ENABLE_PROFILE
    require(rig.core->stats().copy_patch_fallbacks == 2, "the blocks compiled with LLVM");
#endif
}

// R0.L = imm (LDIMMhalf, whose build stencils take the immediate from a
// field hole): built at run time, the immediate is in the code.
void test_no_field_holes() {
    bcore_cp::RuntimeStencils rt;
    require(rt.configure({}), "configure");
    bcore_cp::RuntimeStencils::Packet a, b;
    a.count = b.count = 2;
    a.words[0] = b.words[0] = 0xe100;
    a.words[1] = 0x1234;
    b.words[1] = 0x4321;
    require(rt.stage(a).ok && rt.stage(b).ok, "LDIMMhalf refused");
    rt.flush();
    const auto* ra = rt.find(a);
    const auto* rb = rt.find(b);
    require(ra && rb, "no LDIMMhalf stencils");
    for (const auto* r : {ra, rb})
        for (const auto& reloc : r->relocs)
            require(reloc.hole < bcore_cp::kHoleField0 || reloc.hole >= bcore_cp::kHoleField0 + 4,
                    "a run-time stencil with a field hole");
    require(ra->code != rb->code, "two immediates, the same code");
}

// Halfwords a translation read, at the block's addresses.
class ReadLog : public FlatMemory {
public:
    explicit ReadLog(CpuState* cpu) : FlatMemory(0, kRam, cpu) { enable_page_table(); }
    mutable std::vector<uint32_t> reads;
    uint16_t read16(uint32_t addr) const override {
        if (addr >= kCode && addr < kCode + 0x100) reads.push_back(addr);
        return FlatMemory::read16(addr);
    }
};

void test_reads_end_at_the_block() {
    // A 32-bit encoding without a build stencil that the lifter takes for
    // an illegal instruction: a packet of 2 bytes that ends the block.
    bcore_cp::RuntimeStencils probe;
    require(probe.configure({}), "configure");
    uint32_t illegal = 0;
    for (uint32_t hi = 0xc000; hi < 0xf800 && !illegal; hi += 0x0101) {
        bool is_parallel = (hi & 0x0800) && (hi & 0xe800) != 0xe800;
        if (is_parallel) continue;
        uint32_t p = 0;
        const uint32_t insn = hi << 16 | 0xffff;
        while (p < bcore_cp::kPatterns32Count &&
               (insn & bcore_cp::kPatterns32[p].mask) != bcore_cp::kPatterns32[p].value)
            ++p;
        if (p < bcore_cp::kPatterns32Count && bcore_cp::kPatterns32[p].selected >= 0) continue;
        bcore_cp::RuntimeStencils::Packet packet;
        packet.count = 2;
        packet.words[0] = uint16_t(hi);
        packet.words[1] = 0xffff;
        const auto staged = probe.stage(packet);
        if (staged.ok && (staged.flags & bcore_cp::kTerminates) && staged.bytes == 2) illegal = insn;
    }
    require(illegal != 0, "no illegal 32-bit encoding found");
    CpuState cpu{};
    ReadLog mem(&cpu);
    Core core(&cpu, &mem);
    core.set_chaining(true, false);
    core.set_tiering(0x7fffffff);
    core.set_copy_patch(true);
    require(core.init(1), "init");
    mem.write16(kCode, uint16_t(illegal >> 16));
    mem.write16(kCode + 2, uint16_t(illegal));
    for (uint32_t a = kCode + 4; a < kCode + 0x40; a += 2) mem.write16(a, 0x0000);  // NOPs after it
    cec_init();
    evt_init();
    cpu.chain_budget = 1;
    mem.reads.clear();
    try {
        core.run(kCode);
    } catch (...) {
    }
    for (uint32_t addr : mem.reads)
        require(addr < kCode + 4, "the translation read past the packet that ends the block");
    require(core.cp_runtime_stats().built == 1, "the illegal instruction's stencil was not built");
#if BCORE_ENABLE_PROFILE
    require(core.stats().copy_patch_blocks == 1, "its block was not made of stencils");
#endif
}

void test_off() {
    CpRuntimeSettings settings;
    settings.enable = false;
    Rig rig(settings);
    rig.place(kCode, {words(kParallelA, 4)});
    rig.run(kCode);
    require(rig.core->cp_runtime_stats().built == 0, "stencils built while off");
#if BCORE_ENABLE_PROFILE
    require(rig.core->stats().copy_patch_fallbacks == 1, "the block compiled with LLVM");
#endif
    // Turned on, the next new block has them.
    rig.core->set_copy_patch_runtime({});
    rig.place(kCode + 0x100, {words(kParallelB, 4)});
    rig.run(kCode + 0x100);
    require(rig.core->cp_runtime_stats().built == 1, "turned on, no stencil built");
}

}  // namespace

int main() {
    test_once_per_encoding();
    test_verify();
    test_same_as_the_build();
    test_refused();
    test_off();
    test_no_field_holes();
    test_reads_end_at_the_block();
    printf("PASS\n");
    return 0;
}
