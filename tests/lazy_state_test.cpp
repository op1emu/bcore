// Blocks compiled with the packet state written only before its readers
// (Core::set_lazy_state, src/packet_state.cpp) show every reader the same
// pc, insn_len and packets as blocks that write them at every packet: the
// memory callbacks (the host asks the time there), an exception or interrupt
// a callback raises (RETX and RETI come from pc), and the state the block
// returns with -- unchained, and chained with a budget that runs the block
// (a loop back to its start) again and again through its chain exit.
#include "core.h"
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
static constexpr uint32_t kDevice = 0x2000;  // past rawmem_limit: every access calls the callbacks
static constexpr uint32_t kFault = 0x2800;   // a load here raises an exception from the callback
static constexpr uint32_t kIrq = 0x2900;     // a store here raises IVG14 from the callback
static constexpr uint32_t kEvx = 0x700, kIvg14 = 0x780;
static constexpr uint32_t kSize = 0x3000;

// Code and data below kDevice inline; above it, each access is recorded with
// the packet state the callback sees.
class RecordingMemory : public Memory {
public:
    explicit RecordingMemory(CpuState* cpu) : cpu_(cpu), data_(kSize, 0) {}
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
    uint32_t rawmem_limit() const override { return kDevice; }
    uint8_t read8(uint32_t a) const override { note('r', a); return data_[a]; }
    uint16_t read16(uint32_t a) const override { note('r', a); return data_[a] | data_[a + 1] << 8; }
    uint32_t read32(uint32_t a) const override {
        note('r', a);
        // A data fault the host raises from inside the access.
        if (a == kFault) cec_exception(cpu_, 0x26, nullptr);
        uint32_t v;
        std::memcpy(&v, &data_[a], 4);
        return v;
    }
    void write8(uint32_t a, uint8_t v) override { note('w', a); data_[a] = v; }
    void write16(uint32_t a, uint16_t v) override { note('w', a); std::memcpy(&data_[a], &v, 2); }
    void write32(uint32_t a, uint32_t v) override {
        note('w', a);
        // A device that interrupts when written: taken after the store.
        if (a == kIrq) cec_raise(cpu_, 14);
        std::memcpy(&data_[a], &v, 4);
    }
    const uint8_t* raw() const override { return data_.data(); }
    uintptr_t fast_base() const override { return uintptr_t(data_.data()); }
    void poke16(uint32_t a, uint16_t v) { std::memcpy(&data_[a], &v, 2); }

private:
    void note(char op, uint32_t a) const {
        if (a >= kDevice)
            const_cast<RecordingMemory*>(this)->seen.push_back(
                {op, a, cpu_->pc, cpu_->insn_len, cpu_->packets, cpu_->chain_packets});
    }
    CpuState* cpu_;
    std::vector<uint8_t> data_;
};

// The instructions a program is made of.
struct Insn { uint16_t words[4]; int len; };
static const Insn kInsns[] = {
    {{0x9100}, 2},                          // R0 = [P0]
    {{0x9300}, 2},                          // [P0] = R0
    {{0x9101}, 2},                          // R1 = [P0]
    {{0x6408}, 2},                          // R0 += 1
    {{0x0000}, 2},                          // NOP
    {{0xe100, 0x1234}, 4},                  // R0.L = 0x1234
    {{0xc408, 0x0000}, 4},                  // A0 = 0
    {{0xcc04, 0x0001, 0x9002, 0x9c01}, 8},  // R0 = R0 + R1 (NS) || R2 = [P0++] || R1 = [I0++]
    {{0x9113}, 2},                          // R3 = [P2]: P2 = kFault, an exception
    {{0x9308}, 2},                          // [P1] = R0: P1 = kIrq, an interrupt
    {{0x9118}, 2},                          // R0 = [P3]: P3 odd, a misaligned load
};
static constexpr int kKinds = sizeof(kInsns) / sizeof(kInsns[0]);

struct Outcome {
    std::vector<RecordingMemory::Seen> seen;
    uint32_t regs[4], p0, i0, pc, insn_len, packets, chain_packets, retx, reti, seqstat;
    bool did_jump;
    bool operator==(const Outcome& o) const {
        return seen == o.seen && std::memcmp(regs, o.regs, sizeof(regs)) == 0 && p0 == o.p0 && i0 == o.i0 &&
               pc == o.pc && insn_len == o.insn_len && packets == o.packets &&
               chain_packets == o.chain_packets && retx == o.retx && reti == o.reti &&
               seqstat == o.seqstat && did_jump == o.did_jump;
    }
};

static Outcome run(const std::vector<int>& program, int opt, bool chained, uint32_t budget, bool lazy) {
    CpuState cpu{};
    RecordingMemory mem(&cpu);
    uint32_t at = kCode;
    for (int i : program) {
        for (int w = 0; w < kInsns[i].len / 2; ++w) mem.poke16(at + 2 * w, kInsns[i].words[w]);
        at += kInsns[i].len;
    }
    // JUMP.S back to the start: the block is its own successor, a native
    // loop when chained.
    mem.poke16(at, uint16_t(0x2000 | (((kCode - at) >> 1) & 0xfff)));
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    core.set_lazy_state(lazy);
    require(core.init(opt), "init");

    // Supervisor at IVG15, interrupts enabled: an exception or IVG14 can be
    // taken, as in the firmware.
    cec_init(); evt_init();
    evt_write(EVT_BASE + 4 * 3, kEvx);
    evt_write(EVT_BASE + 4 * 14, kIvg14);
    evt_write(EVT_BASE + 4 * 15, kCode);
    cec_sti(&cpu, 0xffff);
    cec_raise(&cpu, 15);
    cec_return_rti(&cpu);
    // Entering IVG15 sets IRPTEN, interrupts off until the handler saves
    // RETI ([--SP] = RETI, which a pending did_jump would refuse).
    cpu.did_jump = false;
    cec_push_reti(&cpu);

    cpu.dpregs[8] = kDevice;          // P0
    cpu.dpregs[9] = kIrq;             // P1
    cpu.dpregs[10] = kFault;          // P2
    cpu.dpregs[11] = kDevice + 0x81;  // P3
    cpu.iregs[0] = kDevice + 0x400;   // I0, linear
    cpu.chain_budget = budget;
    require(core.run(kCode), "run");
    return {mem.seen, {cpu.dpregs[0], cpu.dpregs[1], cpu.dpregs[2], cpu.dpregs[3]}, cpu.dpregs[8], cpu.iregs[0],
            cpu.pc, cpu.insn_len, cpu.packets, cpu.chain_packets, cpu.retx, cpu.reti, cpu.seqstat, cpu.did_jump};
}

int main() {
    std::mt19937 rng(1);
    unsigned cases = 0, exceptions = 0, interrupts = 0, chained_loops = 0;
    for (int program_no = 0; program_no < 60; ++program_no) {
        std::vector<int> program(1 + rng() % 20);
        // Most programs run to their end; some raise something on the way.
        for (int& i : program) i = int(rng() % (program_no % 3 ? kKinds - 3 : kKinds));
        for (int opt : {0, 1, 2})
            for (bool chained : {false, true})
                for (uint32_t budget : {0u, 64u}) {
                    if (!chained && budget) continue;
                    const Outcome a = run(program, opt, chained, budget, false);
                    const Outcome b = run(program, opt, chained, budget, true);
                    if (!(a == b)) {
                        fprintf(stderr, "FAIL: program %d -O%d%s budget %u: %zu vs %zu accesses, pc %x/%x "
                                        "insn_len %u/%u packets %u/%u chain %u/%u retx %x/%x reti %x/%x\n",
                                program_no, opt, chained ? " chained" : "", budget, a.seen.size(), b.seen.size(),
                                a.pc, b.pc, a.insn_len, b.insn_len, a.packets, b.packets, a.chain_packets,
                                b.chain_packets, a.retx, b.retx, a.reti, b.reti);
                        for (size_t i = 0; i < a.seen.size() && i < b.seen.size(); ++i)
                            if (!(a.seen[i] == b.seen[i]))
                                fprintf(stderr, "  access %zu: pc %x/%x insn_len %u/%u packets %u/%u\n", i,
                                        a.seen[i].pc, b.seen[i].pc, a.seen[i].insn_len, b.seen[i].insn_len,
                                        a.seen[i].packets, b.seen[i].packets);
                        return 1;
                    }
                    exceptions += a.pc == kEvx;
                    interrupts += a.pc == kIvg14;
                    chained_loops += a.chain_packets != 0;
                    ++cases;
                }
    }
    // The cases above are only worth something if they reach these paths.
    require(exceptions > 0, "no program took an exception");
    require(interrupts > 0, "no program took IVG14");
    require(chained_loops > 0, "no chained run looped");
    printf("lazy state test passed (%u cases, %u exceptions, %u interrupts, %u looped chained)\n", cases,
           exceptions, interrupts, chained_loops);
    return 0;
}
