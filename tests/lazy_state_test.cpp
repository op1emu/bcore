// Blocks compiled with the packet state written only before its readers
// (Core::set_lazy_state, src/packet_state.cpp) show every reader the same
// pc, insn_len and packets as blocks that write them at every packet: the
// memory callbacks (the host asks the time there), and the state the block
// returns with.
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
static constexpr uint32_t kSize = 0x3000;

// Code and data below kDevice inline; above it, each access is recorded with
// the packet state the callback sees.
class RecordingMemory : public Memory {
public:
    explicit RecordingMemory(const CpuState* cpu) : cpu_(cpu), data_(kSize, 0) {}
    struct Seen {
        char op;
        uint32_t addr, pc, insn_len, packets;
        bool operator==(const Seen& o) const {
            return op == o.op && addr == o.addr && pc == o.pc && insn_len == o.insn_len && packets == o.packets;
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
        uint32_t v;
        std::memcpy(&v, &data_[a], 4);
        return v;
    }
    void write8(uint32_t a, uint8_t v) override { note('w', a); data_[a] = v; }
    void write16(uint32_t a, uint16_t v) override { note('w', a); std::memcpy(&data_[a], &v, 2); }
    void write32(uint32_t a, uint32_t v) override { note('w', a); std::memcpy(&data_[a], &v, 4); }
    const uint8_t* raw() const override { return data_.data(); }
    uintptr_t fast_base() const override { return uintptr_t(data_.data()); }
    void poke16(uint32_t a, uint16_t v) { std::memcpy(&data_[a], &v, 2); }

private:
    void note(char op, uint32_t a) const {
        if (a >= kDevice) const_cast<RecordingMemory*>(this)->seen.push_back({op, a, cpu_->pc, cpu_->insn_len, cpu_->packets});
    }
    const CpuState* cpu_;
    std::vector<uint8_t> data_;
};

// The instructions a program is made of: 16-bit ones and two 32-bit ones.
struct Insn { uint16_t words[2]; int len; };
static const Insn kInsns[] = {
    {{0x9100, 0}, 2},       // R0 = [P0]
    {{0x9300, 0}, 2},       // [P0] = R0
    {{0x9101, 0}, 2},       // R1 = [P0]
    {{0x6408, 0}, 2},       // R0 += 1
    {{0x0000, 0}, 2},       // NOP
    {{0xe100, 0x1234}, 4},  // R0.L = 0x1234
    {{0xc408, 0x0000}, 4},  // A0 = 0
};

struct Outcome {
    std::vector<RecordingMemory::Seen> seen;
    uint32_t pc, insn_len, packets, r0, r1;
    bool did_jump;
};

static Outcome run(const std::vector<int>& program, int opt, bool chained, bool lazy) {
    CpuState cpu{};
    RecordingMemory mem(&cpu);
    uint32_t at = kCode;
    for (int i : program) {
        for (int w = 0; w < kInsns[i].len / 2; ++w) mem.poke16(at + 2 * w, kInsns[i].words[w]);
        at += kInsns[i].len;
    }
    mem.poke16(at, 0x2000);  // JUMP.S 0: ends the block, to itself
    Core core(&cpu, &mem);
    core.set_chaining(chained, false);
    core.set_lazy_state(lazy);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    cpu.dpregs[8] = kDevice;  // P0
    cpu.chain_budget = 0;  // chained: the self jump leaves through the chain exit
    require(core.run(kCode), "run");
    return {mem.seen, cpu.pc, cpu.insn_len, cpu.packets, cpu.dpregs[0], cpu.dpregs[1], cpu.did_jump};
}

int main() {
    std::mt19937 rng(1);
    unsigned cases = 0;
    for (int program_no = 0; program_no < 40; ++program_no) {
        std::vector<int> program(1 + rng() % 20);
        for (int& i : program) i = int(rng() % (sizeof(kInsns) / sizeof(kInsns[0])));
        for (int opt : {0, 1, 2})
            for (bool chained : {false, true}) {
                const Outcome a = run(program, opt, chained, false);
                const Outcome b = run(program, opt, chained, true);
                const bool same = a.seen == b.seen && a.pc == b.pc && a.insn_len == b.insn_len &&
                                  a.packets == b.packets && a.r0 == b.r0 && a.r1 == b.r1 &&
                                  a.did_jump == b.did_jump;
                if (!same) {
                    fprintf(stderr, "FAIL: program %d -O%d%s: %zu vs %zu accesses, pc %x/%x insn_len %u/%u packets %u/%u\n",
                            program_no, opt, chained ? " chained" : "", a.seen.size(), b.seen.size(),
                            a.pc, b.pc, a.insn_len, b.insn_len, a.packets, b.packets);
                    for (size_t i = 0; i < a.seen.size() && i < b.seen.size(); ++i)
                        if (!(a.seen[i] == b.seen[i]))
                            fprintf(stderr, "  access %zu: pc %x/%x insn_len %u/%u packets %u/%u\n", i,
                                    a.seen[i].pc, b.seen[i].pc, a.seen[i].insn_len, b.seen[i].insn_len,
                                    a.seen[i].packets, b.seen[i].packets);
                    return 1;
                }
                ++cases;
            }
    }
    printf("lazy state test passed (%u cases)\n", cases);
    return 0;
}
