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
//   - off, a block with a parallel packet compiles with LLVM.
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
// R0 = R1 + R2 (dsp32alu ADD32) || NOP || NOP, and R3 = R4 + R5 || [P0] = R0.
constexpr uint16_t kParallelA[4] = {0xcc04, 0x000a, 0x0000, 0x0000};
constexpr uint16_t kParallelB[4] = {0xcc04, 0x165c, 0x0000, 0x9300};
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
    for (size_t at = 0; at < packets.size(); at += 64)
        rt.build(std::vector<bcore_cp::RuntimeStencils::Packet>(packets.begin() + at,
                                                                packets.begin() + std::min(packets.size(), at + 64)));
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
    printf("PASS\n");
    return 0;
}
