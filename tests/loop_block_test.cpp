// Loop blocks (Core::set_loop_blocks): a block that runs itself again in a
// native loop keeps the guest state in SSA values across the loop and copies
// it to CpuState before each helper call and exit. It must be exact: each
// random loop program runs to the same state, run after run, with loop
// blocks as without -- every register, the packet and chain counts each run
// returns with, guest memory, and the packet state each device access sees
// (the host asks the time there) -- unchained, and chained with budgets that
// end runs inside a loop, through an exception or an interrupt a device
// raises, through inline RAM (rawmem) and through the page table.
#include "core.h"
#include "bcore_profile.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr uint32_t kCode = 0x100;
static constexpr uint32_t kRam = 0x1000;     // P4, I0, I2, I3 point here
static constexpr uint32_t kRing = 0x1800;    // I1's circular buffer
static constexpr uint32_t kDevice = 0x2000;  // every access calls the callbacks
static constexpr uint32_t kFault = 0x2800;   // a load here raises an exception from the callback
static constexpr uint32_t kIrq = 0x2900;     // a store here raises IVG14 from the callback
static constexpr uint32_t kEvx = 0x700, kIvg14 = 0x780;
static constexpr uint32_t kSize = 0x3000;
static constexpr uint32_t kPages = kSize >> Memory::kPageBits;

// Code and data below kDevice inline (rawmem, or the page table); above it,
// each access is recorded with the packet state the callback sees. A
// clobbering one says its callbacks may change registers, and its device
// read does: L0 to 12 or back to 0 (B0 at kRam), and R3.
class RecordingMemory : public Memory {
public:
    RecordingMemory(CpuState* cpu, bool pages, bool clobber = false)
        : cpu_(cpu), data_(kSize, 0), pages_(pages), clobber_(clobber) {
        table_.assign(kPageTableEntries, nullptr);
        for (uint32_t page = 0; page < (kDevice >> kPageBits); ++page) table_[page] = &data_[page << kPageBits];
    }
    struct Seen {
        char op;
        uint32_t addr, pc, insn_len, packets, chain_packets;
        bool operator==(const Seen& o) const {
            return op == o.op && addr == o.addr && pc == o.pc && insn_len == o.insn_len &&
                   packets == o.packets && chain_packets == o.chain_packets;
        }
    };
    std::vector<Seen> seen;

    uint32_t base() const override { return 0; }
    uint32_t size() const override { return kSize; }
    uint32_t rawmem_limit() const override { return pages_ ? 0 : kDevice; }
    uint8_t* const* page_table() const override { return pages_ ? table_.data() : nullptr; }
    bool callbacks_preserve_registers() const override { return !clobber_; }
    // Past the end (a pointer the loops walk on): reads 0, writes nothing.
    uint8_t read8(uint32_t a) const override { note('r', a); return a < kSize ? data_[a] : 0; }
    uint16_t read16(uint32_t a) const override {
        note('r', a);
        return a + 2 <= kSize ? uint16_t(data_[a] | data_[a + 1] << 8) : 0;
    }
    uint32_t read32(uint32_t a) const override {
        note('r', a);
        if (a == kFault) cec_exception(cpu_, 0x26, nullptr);
        if (a == kDevice && clobber_) {
            cpu_->lregs[0] = clobbers_++ % 3 == 1 ? 12 : 0;
            cpu_->bregs[0] = kRam;
            cpu_->dpregs[3] += 5;
        }
        uint32_t v = 0;
        if (a + 4 <= kSize) std::memcpy(&v, &data_[a], 4);
        return v;
    }
    void write8(uint32_t a, uint8_t v) override { note('w', a); if (a < kSize) data_[a] = v; }
    void write16(uint32_t a, uint16_t v) override { note('w', a); if (a + 2 <= kSize) std::memcpy(&data_[a], &v, 2); }
    void write32(uint32_t a, uint32_t v) override {
        note('w', a);
        if (a == kIrq) cec_raise(cpu_, 14);
        if (a + 4 <= kSize) std::memcpy(&data_[a], &v, 4);
    }
    const uint8_t* raw() const override { return data_.data(); }
    uintptr_t fast_base() const override { return uintptr_t(data_.data()); }
    void poke16(uint32_t a, uint16_t v) { std::memcpy(&data_[a], &v, 2); }
    void poke32(uint32_t a, uint32_t v) { std::memcpy(&data_[a], &v, 4); }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    void note(char op, uint32_t a) const {
        if (a >= kDevice && op == 'r') ++cpu_->unstable_reads;
        if (a >= kDevice)
            const_cast<RecordingMemory*>(this)->seen.push_back(
                {op, a, cpu_->pc, cpu_->insn_len, cpu_->packets, cpu_->chain_packets});
    }
    CpuState* cpu_;
    std::vector<uint8_t> data_;
    std::vector<uint8_t*> table_;
    bool pages_;
    bool clobber_;
    mutable unsigned clobbers_ = 0;
};

// What loop bodies are made of (encodings from bfin-elf-as).
struct Insn { uint16_t words[4]; int len; };
static const Insn kInsns[] = {
    {{0x9100}, 2},                          // R0 = [P0]          (a device)
    {{0x9300}, 2},                          // [P0] = R0
    {{0x6408}, 2},                          // R0 += 1
    {{0x9c01}, 2},                          // R1 = [I0++]
    {{0x9022}, 2},                          // R2 = [P4++]
    {{0x9221}, 2},                          // [P4++] = R1
    {{0xc863, 0x0a08, 0x9c31, 0x9c00}, 8},  // A0 += R1.L * R0.H (W32) || R1.L = W[I2++] || R0 = [I0++]
    {{0x50cb}, 2},                          // R3 = R3 + R1
    {{0x0c00}, 2},                          // CC = R0 == 0
    {{0x089a}, 2},                          // CC = R2 < R3
    {{0x9e65}, 2},                          // I1 += M1          (circular)
    {{0x9c0c}, 2},                          // R4 = [I1++]       (circular)
    {{0xc103, 0x0812}, 4},                  // A0 += R2.L * R2.L (IS)
    {{0x0000}, 2},                          // NOP
    {{0xe100, 0x1234}, 4},                  // R0.L = 0x1234
    {{0x9e39}, 2},                          // W[I3++] = R1.L
    {{0xc404, 0x2a2c}, 4},                  // R5 = R5 + R4 (S)
    {{0xc409, 0x4000}, 4},                  // A0.X = R0.L
    {{0x3809}, 2},                          // A0.W = R1
    {{0x3131}, 2},                          // R6 = A0.W
    {{0x362a}, 2},                          // L1 = R2
    {{0xc40b, 0x803f}, 4},                  // A0 += A1
    {{0xc001, 0x1801}, 4},                  // A1 += R0.L * R1.L
    {{0x1800}, 2},                          // IF CC JUMP out    (offset patched)
    {{0x9113}, 2},                          // R3 = [P2]: P2 = kFault, an exception
    {{0x9308}, 2},                          // [P1] = R0: P1 = kIrq, an interrupt
    {{0x9118}, 2},                          // R0 = [P3]: P3 odd, a misaligned load
};
static constexpr int kKinds = sizeof(kInsns) / sizeof(kInsns[0]);
static constexpr int kBranchOut = 23;
static constexpr int kRaising = 3;  // the last kinds

struct Program {
    bool hardware;           // LSETUP over the body, or a jump back to its start
    std::vector<int> body;
    uint32_t lc;             // P5, LC0 for the hardware loop
    std::vector<uint32_t> ram;  // at kRam, if not the default
    uint32_t a0x = 0, a0w = 0;  // A0 at entry
    // I1's circular buffer and step at entry (LiftVisitor::Circular).
    uint32_t i1 = kRing, b1 = kRing, l1 = 0x14, m1 = 4;
};

// The program at kCode:
//   hardware: LSETUP (top, bottom) LC0 = P5; top: body; R7 += 1; JUMP.S kCode
//   jump:     top: body; CC = R0 == 0 ... IF !CC JUMP top; R7 += 1; JUMP.S kCode
static void load(RecordingMemory& mem, const Program& p) {
    uint32_t at = kCode;
    if (p.hardware) at += 4;
    const uint32_t top = at;
    std::vector<uint32_t> branches;
    uint32_t bottom = at;
    for (int i : p.body) {
        if (i == kBranchOut) branches.push_back(at);
        bottom = at;
        for (int w = 0; w < kInsns[i].len / 2; ++w) mem.poke16(at + 2 * w, kInsns[i].words[w]);
        at += kInsns[i].len;
    }
    if (!p.hardware) {
        bottom = at;
        // IF !CC JUMP top
        mem.poke16(at, uint16_t(0x1000 | (((top - at) >> 1) & 0x3ff)));
        at += 2;
    }
    const uint32_t out = at;
    mem.poke16(at, 0x640f);  // R7 += 1
    at += 2;
    mem.poke16(at, uint16_t(0x2000 | (((kCode - at) >> 1) & 0xfff)));  // JUMP.S kCode
    for (uint32_t b : branches) mem.poke16(b, uint16_t(0x1800 | (((out - b) >> 1) & 0x3ff)));
    if (p.hardware) {
        // LSETUP (top, bottom) LC0 = P5
        mem.poke16(kCode, uint16_t(0xe0a0 | ((top - kCode) >> 1)));
        mem.poke16(kCode + 2, uint16_t(0x5000 | ((bottom - kCode) >> 1)));
    }
}

struct Run {
    uint32_t pc, packets, chain_packets, chain_blocks;
    bool operator==(const Run& o) const {
        return pc == o.pc && packets == o.packets && chain_packets == o.chain_packets && chain_blocks == o.chain_blocks;
    }
};

struct Outcome {
    std::vector<Run> runs;
    std::vector<RecordingMemory::Seen> seen;
    std::vector<uint8_t> state;  // CpuState up to the execution state
    uint32_t pc, retx, reti, seqstat;
    std::vector<uint8_t> data;
    BcoreStats stats;  // not compared
    bool operator==(const Outcome& o) const {
        return runs == o.runs && seen == o.seen && state == o.state && pc == o.pc && retx == o.retx &&
               reti == o.reti && seqstat == o.seqstat && data == o.data;
    }
};

static Outcome run(const Program& p, int opt, bool chained, uint32_t budget, bool pages, bool loops,
                   bool clobber = false, int steady = 0, uint32_t tier = 0, int background = -1, bool count = true) {
    CpuState cpu{};
    RecordingMemory mem(&cpu, pages, clobber);
    load(mem, p);
    for (uint32_t i = 0; i < 0x200; i += 4) mem.poke32(kRam + i, 0x00030000u + i * 0x1111u);
    for (size_t i = 0; i < p.ram.size(); ++i) mem.poke32(kRam + 4 * uint32_t(i), p.ram[i]);
    for (uint32_t i = 0; i < 0x20; i += 4) mem.poke32(kRing + i, i * 3 + 1);
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    core.set_count_chained(count);
    core.set_loop_blocks(loops);
    core.set_steady_skip(steady);
    core.set_tiering(tier);
    if (background >= 0) core.set_tier_background(true, uint64_t(background));
    require(core.init(opt), "init");

    // Supervisor at IVG15, interrupts enabled, as in the firmware.
    cec_init(); evt_init();
    evt_write(EVT_BASE + 4 * 3, kEvx);
    evt_write(EVT_BASE + 4 * 14, kIvg14);
    evt_write(EVT_BASE + 4 * 15, kCode);
    cec_sti(&cpu, 0xffff);
    cec_raise(&cpu, 15);
    cec_return_rti(&cpu);
    cpu.did_jump = false;
    cec_push_reti(&cpu);

    cpu.dpregs[8] = kDevice;          // P0
    cpu.dpregs[9] = kIrq;             // P1
    cpu.dpregs[10] = kFault;          // P2
    cpu.dpregs[11] = kDevice + 0x81;  // P3
    cpu.dpregs[12] = kRam + 0x100;    // P4
    cpu.dpregs[13] = p.lc;            // P5
    cpu.iregs[0] = kRam;              // I0, linear
    cpu.iregs[1] = p.i1; cpu.bregs[1] = p.b1; cpu.lregs[1] = p.l1; cpu.mregs[1] = p.m1;  // I1 circular
    cpu.iregs[2] = kRam + 0x80;       // I2, linear
    cpu.iregs[3] = kRam + 0x180;      // I3, linear
    cpu.dpregs[1] = 0x7fff;
    cpu.ax[0] = p.a0x; cpu.aw[0] = p.a0w;
    Outcome out;
    for (int runs = 0; runs < 24 && cpu.pc != kEvx && cpu.pc != kIvg14 && !cpu.halted; ++runs) {
        cpu.chain_budget = budget;
        require(core.run(runs ? cpu.pc : kCode), "run");
        out.runs.push_back({cpu.pc, cpu.packets, cpu.chain_packets, cpu.chain_blocks});
        cpu.did_jump = false;
        cec_check_pending(&cpu);
        cpu.did_jump = false;
    }
    out.seen = mem.seen;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&cpu);
    out.state.assign(bytes, bytes + offsetof(CpuState, insn_len));
    out.pc = cpu.pc; out.retx = cpu.retx; out.reti = cpu.reti; out.seqstat = cpu.seqstat;
    out.data.assign(mem.data().begin(), mem.data().begin() + kDevice);
    out.stats = core.stats();
    return out;
}

// A loop block that took L0 to be 0 when it was compiled (its entry guard
// checks), run with L0 set: as a block compiled for that.
static void test_l_zero() {
    Program p;
    p.hardware = true;
    p.body = {3, 7};  // R1 = [I0++]; R3 = R3 + R1
    p.lc = 40;
    for (int opt : {1, 2})
        for (bool pages : {false, true}) {
            auto once = [&](bool loops, bool compile_linear) {
                CpuState cpu{};
                RecordingMemory mem(&cpu, pages);
                load(mem, p);
                for (uint32_t i = 0; i < 0x200; i += 4) mem.poke32(kRam + i, i + 1);
                Core core(&cpu, &mem);
                core.set_chaining(true, false);
                core.set_loop_blocks(loops);
                require(core.init(opt), "init");
                cec_init(); evt_init();
                evt_write(EVT_BASE + 4 * 3, kEvx);
                evt_write(EVT_BASE + 4 * 15, kCode);
                cec_sti(&cpu, 0xffff);
                cec_raise(&cpu, 15);
                cec_return_rti(&cpu);
                cpu.did_jump = false;
                cec_push_reti(&cpu);
                std::vector<Run> runs;
                for (int pass = 0; pass < 2; ++pass) {
                    // The first pass compiles, linear or not; the second runs
                    // circular, over 12 bytes.
                    const bool linear = pass == 0 ? compile_linear : false;
                    cpu.iregs[0] = kRam; cpu.bregs[0] = kRam; cpu.lregs[0] = linear ? 0 : 12;
                    cpu.dpregs[13] = p.lc;
                    cpu.dpregs[3] = 0;
                    cpu.pc = kCode;
                    for (int r = 0; r < 8; ++r) {
                        cpu.chain_budget = 1000;
                        require(core.run(cpu.pc), "run");
                        runs.push_back({cpu.pc, cpu.packets, cpu.chain_packets, cpu.chain_blocks});
                        cpu.did_jump = false;
                    }
                    runs.push_back({cpu.dpregs[3], cpu.iregs[0], 0, 0});
                }
                return runs;
            };
            for (bool linear : {true, false}) {
                const auto want = once(false, linear);
                const auto got = once(true, linear);
                for (size_t i = 0; i < want.size() && i < got.size(); ++i)
                    if (!(want[i] == got[i]))
                        fprintf(stderr, "  -O%d%s%s run %zu: %x/%x %u/%u %u/%u\n", opt, pages ? " pages" : "",
                                linear ? " compiled linear" : "", i, want[i].pc, got[i].pc, want[i].packets,
                                got[i].packets, want[i].chain_packets, got[i].chain_packets);
                require(got == want, "a loop block taken to be linear ran circular differently");
            }
        }
}

// Loops whose device read changes registers behind the block (a Memory
// whose callbacks do not preserve them): among them L0, which a loop block
// may have taken to be 0 at compilation, and R3, which the loop sums into.
static unsigned test_clobbering_callbacks() {
    std::mt19937 rng(13);
    unsigned cases = 0;
    for (int program_no = 0; program_no < 24; ++program_no) {
        Program p;
        p.hardware = program_no % 2 == 0;
        p.body.resize(rng() % 6);
        for (int& i : p.body) {
            do i = int(rng() % (kKinds - kRaising)); while (i == kBranchOut);
        }
        // The device read, and a load through I0 with R3 summing it, in
        // either order.
        const bool read_first = program_no % 4 < 2;
        p.body.insert(p.body.begin() + rng() % (p.body.size() + 1), read_first ? 0 : 3);
        p.body.insert(p.body.begin() + rng() % (p.body.size() + 1), read_first ? 3 : 0);
        p.body.push_back(7);  // R3 = R3 + R1
        static const uint32_t kCounts[] = {1, 2, 5, 40};
        p.lc = kCounts[rng() % 4];
        for (int opt : {0, 1, 2})
            for (bool pages : {false, true})
                for (bool chained : {false, true})
                    for (uint32_t budget : {0u, 7u, 1000u}) {
                        if (chained != (budget != 0)) continue;
                        const Outcome want = run(p, opt, chained, budget, pages, false, true);
                        const Outcome got = run(p, opt, chained, budget, pages, true, true);
                        if (!(want == got)) {
                            fprintf(stderr, "FAIL: clobbering program %d (%s, lc %u) -O%d%s budget %u: pc %x/%x\n",
                                    program_no, p.hardware ? "hardware" : "jump", p.lc, opt, pages ? " pages" : "",
                                    budget, want.pc, got.pc);
                            for (size_t i = 0; i < want.state.size(); i += 4)
                                if (std::memcmp(&want.state[i], &got.state[i], 4))
                                    fprintf(stderr, "  CpuState+%zu: %08x/%08x\n", i,
                                            *reinterpret_cast<const uint32_t*>(&want.state[i]),
                                            *reinterpret_cast<const uint32_t*>(&got.state[i]));
                            std::exit(1);
                        }
                        ++cases;
                    }
    }
    return cases;
}

int main() {
    test_l_zero();
    const unsigned clobbering = test_clobbering_callbacks();
    printf("clobbering callback cases passed (%u)\n", clobbering);
    std::mt19937 rng(7);
    unsigned cases = 0, raised = 0, long_runs = 0;
    uint64_t loop_blocks = 0, proven = 0, speculated = 0, vectorized = 0, circular = 0, promoted = 0;
    for (int program_no = 0; program_no < 48; ++program_no) {
        Program p;
        p.hardware = program_no % 2 == 0;
        p.body.resize(1 + rng() % 10);
        for (int& i : p.body) i = int(rng() % (program_no % 4 == 1 ? kKinds : kKinds - kRaising));
        static const uint32_t kCounts[] = {0, 1, 2, 5, 40};
        p.lc = kCounts[rng() % 5];
        // I1's buffer: words, a step of words, often no larger than the
        // buffer (the circular case), sometimes larger.
        static const uint32_t kLengths[] = {4, 8, 0x14, 0x40, 0x100};
        static const int32_t kSteps[] = {-8, -4, 0, 4, 8, 0x40, -0x100, 0x104};
        p.l1 = kLengths[rng() % 5];
        p.b1 = kRing + 4 * (rng() % 0x40);
        p.i1 = p.b1 + 4 * (rng() % (p.l1 / 4));
        p.m1 = uint32_t(kSteps[rng() % 8]);
        for (int opt : {0, 1, 2})
            for (bool pages : {false, true})
                for (bool chained : {false, true})
                    for (uint32_t budget : {0u, 1u, 7u, 64u, 1000u}) {
                        if (!chained && budget) continue;
                        const Outcome want = run(p, opt, chained, budget, pages, false);
                        // Every other program with SteadyLoopSkip too, which
                        // must change nothing: a device read is a recorded
                        // access, so the recorder counts every one unstable.
                        Outcome got = run(p, opt, chained, budget, pages, true, false, program_no / 2 % 2 ? 2 : 0);
                        circular += got.stats.dag_circular;
                        // Tiered (Core::set_tiering) as well, every block
                        // promoted after its first start, its second or its
                        // seventh, here or on the worker: the same again.
                        static const uint32_t kThresholds[] = {1, 2, 7};
                        uint32_t tiered = 0;
                        if (want == got) {
                            tiered = kThresholds[(program_no + budget) % 3];
                            static const int kBackground[] = {-1, 0, 3};
                            got = run(p, opt, chained, budget, pages, true, false, program_no / 2 % 2 ? 2 : 0, tiered,
                                      kBackground[(program_no / 3 + budget) % 3]);
                            promoted += got.stats.blocks_promoted;
                        }
                        if (!(want == got)) {
                            fprintf(stderr, "FAIL: program %d (%s, lc %u) -O%d%s%s budget %u tier threshold %u: %zu/%zu runs, "
                                            "%zu/%zu accesses, pc %x/%x\n",
                                    program_no, p.hardware ? "hardware" : "jump", p.lc, opt,
                                    pages ? " pages" : "", chained ? " chained" : "", budget, tiered, want.runs.size(),
                                    got.runs.size(), want.seen.size(), got.seen.size(), want.pc, got.pc);
                            for (size_t i = 0; i < want.runs.size() && i < got.runs.size(); ++i)
                                if (!(want.runs[i] == got.runs[i]))
                                    fprintf(stderr, "  run %zu: pc %x/%x packets %u/%u chain %u/%u blocks %u/%u\n",
                                            i, want.runs[i].pc, got.runs[i].pc, want.runs[i].packets,
                                            got.runs[i].packets, want.runs[i].chain_packets,
                                            got.runs[i].chain_packets, want.runs[i].chain_blocks,
                                            got.runs[i].chain_blocks);
                            for (size_t i = 0; i < want.state.size(); i += 4)
                                if (std::memcmp(&want.state[i], &got.state[i], 4))
                                    fprintf(stderr, "  CpuState+%zu: %08x/%08x\n", i,
                                            *reinterpret_cast<const uint32_t*>(&want.state[i]),
                                            *reinterpret_cast<const uint32_t*>(&got.state[i]));
                            if (want.data != got.data) fprintf(stderr, "  guest memory differs\n");
                            return 1;
                        }
                        raised += want.pc == kEvx || want.pc == kIvg14;
                        for (auto& r : want.runs) long_runs += r.chain_packets > 16;
                        ++cases;
                    }
    }
    // The firmware's FIR filler shape: one MAC (W32) with two loads, in a
    // hardware loop, over data that saturates the accumulator or does not,
    // from accumulators near the window's ends, so that a speculative sum
    // both holds and fails its bound. Every other program with chained
    // blocks uncounted (op1emu's release build): the loop's latch is then
    // its stop test, which the speculative copy must drop to be vectorized.
    for (int program_no = 0; program_no < 40; ++program_no) {
        Program p;
        p.hardware = true;
        p.body = {6};
        if (program_no % 5 == 4) p.body = {6, 7};  // and an add
        static const uint32_t kCounts[] = {0, 1, 2, 7, 8, 9, 40, 63, 100};
        p.lc = kCounts[rng() % 9];
        p.ram.resize(0x80);
        const int pattern = program_no % 4;
        for (auto& word : p.ram) {
            const uint32_t r = rng();
            switch (pattern) {
            case 0: word = (r & 0x00ff00ff); break;                        // small
            case 1: word = r; break;                                       // anything
            case 2: word = 0x80008000u; break;                             // 0x8000 * 0x8000
            default: word = (r & 1) ? 0x7fff7fffu : (r & 0x0fff0fffu); break;  // some large
            }
        }
        static const uint32_t kA0[][2] = {{0, 0}, {0, 0x7ffffff0}, {0xff, 0x80000010}, {0, 0x12345678}, {0x7f, 0}};
        const auto* a0 = kA0[rng() % 5];
        p.a0x = a0[0]; p.a0w = a0[1];
        for (int opt : {1, 2})
            for (bool pages : {false, true})
                for (uint32_t budget : {1u, 64u, 1000u, 100000u}) {
                    const bool count = program_no % 2;
                    const Outcome want = run(p, opt, true, budget, pages, false, false, 0, 0, -1, count);
                    Outcome got = run(p, opt, true, budget, pages, true, false, 0, 0, -1, count);
                    // Tiered too, the block promoted after its first start
                    // (the firmware's FIR filler is a tier-1 block): the
                    // same, and its copy speculated and vectorized as well.
                    if (want == got) {
                        loop_blocks += got.stats.loop_blocks;
                        proven += got.stats.loops_proven;
                        speculated += got.stats.loops_speculated;
                        vectorized += got.stats.loops_vectorized;
                        got = run(p, opt, true, budget, pages, true, false, 0, 1, -1, count);
                    }
                    if (!(want == got)) {
                        fprintf(stderr, "FAIL: mac program %d (lc %u, pattern %d, a0 %x:%08x) -O%d%s%s budget %u\n",
                                program_no, p.lc, pattern, p.a0x, p.a0w, opt, pages ? " pages" : "",
                                count ? "" : " uncounted", budget);
                        for (size_t i = 0; i < want.state.size(); i += 4)
                            if (std::memcmp(&want.state[i], &got.state[i], 4))
                                fprintf(stderr, "  CpuState+%zu: %08x/%08x\n", i,
                                        *reinterpret_cast<const uint32_t*>(&want.state[i]),
                                        *reinterpret_cast<const uint32_t*>(&got.state[i]));
                        for (size_t i = 0; i < want.runs.size() && i < got.runs.size(); ++i)
                            if (!(want.runs[i] == got.runs[i]))
                                fprintf(stderr, "  run %zu: pc %x/%x packets %u/%u chain %u/%u\n", i,
                                        want.runs[i].pc, got.runs[i].pc, want.runs[i].packets, got.runs[i].packets,
                                        want.runs[i].chain_packets, got.runs[i].chain_packets);
                        return 1;
                    }
                    promoted += got.stats.blocks_promoted;
                    loop_blocks += got.stats.loop_blocks;
                    proven += got.stats.loops_proven;
                    speculated += got.stats.loops_speculated;
                    vectorized += got.stats.loops_vectorized;
                    ++cases;
                }
    }
    require(raised > 0, "no program raised an exception or an interrupt");
#if BCORE_ENABLE_PROFILE
    require(loop_blocks > 0 && proven > 0, "no MAC loop became a loop block with its memory proven");
    require(speculated > 0, "no MAC loop's sum was speculated");
    require(vectorized == speculated, "a speculated MAC loop was not vectorized");
    require(circular > 0, "no loop block took I1 to stay in its circular buffer");
    require(promoted > 0, "no tier-0 block was promoted");
#endif
    require(long_runs > 0, "no chained run looped");
    printf("loop block test passed (%u cases, %u raised, %u long runs; MAC loops: %llu loop blocks, %llu proven, "
           "%llu speculated, %llu vectorized)\n",
           cases, raised, long_runs, (unsigned long long)loop_blocks, (unsigned long long)proven,
           (unsigned long long)speculated, (unsigned long long)vectorized);
    return 0;
}
