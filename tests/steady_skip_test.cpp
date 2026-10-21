// SteadyLoopSkip (Core::set_steady_skip): a loop block's loop that only waits
// -- polling a flag in RAM, a device register that will not change before the
// run ends, a hardware loop that counts down -- goes to its last iteration.
// It must be exact: every program runs to the same registers, packets, chain
// counts and memory with the skip as without, for budgets that end the run
// inside the loop and past it, and a device whose reads the host cannot
// promise would repeat is read as often as without. Where the reads can
// repeat, the skip must make far fewer of them.
#include "core.h"
#include "bcore_profile.h"
#include "../src/cec.h"
#include "../src/evt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr uint32_t kCode = 0x100;
static constexpr uint32_t kFlag = 0x1000;    // RAM, through the page table
static constexpr uint32_t kDevice = 0x2000;  // every read calls the callback
static constexpr uint32_t kSize = 0x3000;

// RAM below kDevice; at kDevice a register reading `value`, stable or not
// (counted in CpuState::unstable_reads as op1emu's host does).
class Device : public Memory {
public:
    Device(CpuState* cpu) : cpu_(cpu), data_(kSize, 0) {
        table_.assign(kPageTableEntries, nullptr);
        for (uint32_t page = 0; page < (kDevice >> kPageBits); ++page) table_[page] = &data_[page << kPageBits];
    }
    uint32_t value = 0;
    bool stable = true;
    mutable uint64_t reads = 0;

    uint32_t base() const override { return 0; }
    uint32_t size() const override { return kSize; }
    uint8_t* const* page_table() const override { return table_.data(); }
    uint32_t rawmem_limit() const override { return 0; }
    bool callbacks_preserve_registers() const override { return true; }
    uint8_t read8(uint32_t a) const override {
        if (a < kDevice) return data_[a];
        return uint8_t(read32(a & ~3u) >> (a & 3) * 8);
    }
    uint16_t read16(uint32_t a) const override {
        if (a < kDevice) return uint16_t(data_[a] | data_[a + 1] << 8);
        return uint16_t(read32(a & ~3u) >> (a & 2) * 8);
    }
    uint32_t read32(uint32_t a) const override {
        if (a < kDevice) { uint32_t v; std::memcpy(&v, &data_[a], 4); return v; }
        ++reads;
        if (!stable) ++cpu_->unstable_reads;
        return value;
    }
    void write8(uint32_t a, uint8_t v) override { if (a < kDevice) data_[a] = v; }
    void write16(uint32_t a, uint16_t v) override { if (a < kDevice) std::memcpy(&data_[a], &v, 2); }
    void write32(uint32_t a, uint32_t v) override { if (a < kDevice) std::memcpy(&data_[a], &v, 4); }
    const uint8_t* raw() const override { return data_.data(); }
    uintptr_t fast_base() const override { return uintptr_t(data_.data()); }
    void poke16(uint32_t a, uint16_t v) { std::memcpy(&data_[a], &v, 2); }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    CpuState* cpu_;
    std::vector<uint8_t> data_;
    std::vector<uint8_t*> table_;
};

struct Outcome {
    std::vector<uint32_t> regs;
    std::vector<uint8_t> memory;
    uint64_t reads, steady;
};

// One run per budget, chained, from kCode, with the code at kCode.
static Outcome run(int mode, std::initializer_list<uint16_t> code, uint32_t device, bool stable, uint32_t lc,
                   uint32_t budget, int opt) {
    CpuState cpu{};
    Device mem(&cpu);
    mem.value = device;
    mem.stable = stable;
    uint32_t at = kCode;
    for (uint16_t op : code) { mem.poke16(at, op); at += 2; }
    Core core(&cpu, &mem);
    core.set_chaining(true);
    core.set_steady_skip(mode);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    cpu.dpregs[8] = kFlag;     // P0
    cpu.dpregs[9] = kDevice;   // P1
    cpu.dpregs[10] = lc;       // P2
    // Runs until the budget is spent or the program is left (the first run
    // of a block after an LSETUP compiles the loop on its own).
    uint64_t total = 0;
    uint32_t pc = kCode;
    const uint32_t end = kCode + 2 * uint32_t(code.size());
    for (int runs = 0; runs < 8 && total < budget && pc >= kCode && pc < end; ++runs) {
        cpu.chain_budget = uint32_t(budget - total);
        require(core.run(pc), "run");
        total += cpu.chain_packets + cpu.packets;
        pc = cpu.pc;
    }
    Outcome o;
    for (uint32_t r : cpu.dpregs) o.regs.push_back(r);
    o.regs.insert(o.regs.end(), {cpu.lc[0], cpu.lt[0], cpu.lb[0], cpu.pc, cpu.packets, cpu.chain_packets, cpu.cc,
                                 uint32_t(total)});
    o.memory = mem.data();
    o.reads = mem.reads;
    o.steady = cpu.steady_iterations;
    return o;
}

struct Program {
    const char* name;
    std::initializer_list<uint16_t> code;
    uint32_t device;  // what the device reads
    bool polls;       // reads the device in the loop
    bool counted;     // a hardware loop: LC0 = P2 iterations at most
    bool waits = true;  // long enough to be skipped, given a long budget and LC0
};

int main() {
    const Program programs[] = {
        // L: R0 = B[P0] (Z); CC = R0 == 0; IF CC JUMP L (BP); R7 = 7;
        {"RAM flag", {0x9900, 0x0c00, 0x1ffe, 0x603f}, 0, false, false},
        // L: R0 = [P1]; CC = BITTST (R0, 0); IF !CC JUMP L; R7 = 8;
        {"device bit", {0x9108, 0x4900, 0x13fe, 0x6047}, 0, true, false},
        // LSETUP (T, B) LC0 = P2; T: R0 = [P1]; CC = BITTST (R0, 12); B: IF !CC JUMP O; O: R7 = 9;
        {"device with timeout", {0xe0a2, 0x2004, 0x9108, 0x4960, 0x1001, 0x604f}, 0x1000, true, true},
        // LSETUP (B, B) LC0 = P2; B: NOP; R7 = 10;
        {"delay", {0xe0a2, 0x2002, 0x0000, 0x6057}, 0, false, true},
        // L: R0 = [P1]; R1 += 1; CC = BITTST (R0, 0); IF !CC JUMP L;
        {"device and a count", {0x9108, 0x6409, 0x4900, 0x13fd}, 0, true, false, false},
        // A flag set from a count, read in the next iteration: no skip (it
        // passes the test while R0 < 3, then ends the loop at R0 = 4).
        // LSETUP (T, B) LC0 = P2; T: IF CC JUMP O; R0 += 1; B: CC = R0 == 3; NOP; O: R7 = 11;
        {"counter to a flag", {0xe0a2, 0x2004, 0x1804, 0x6408, 0x0c18, 0x0000, 0x605f}, 0, false, true, false},
    };
    unsigned skipped = 0;
    for (int opt : {1, 2}) {
        for (const Program& p : programs) {
            for (uint32_t lc : {1u, 2u, 7u, 100000u}) {
                for (uint32_t budget : {1u, 3u, 10u, 1000u, 300000u, 1u << 24}) {
                    for (bool stable : {true, false}) {
                        const Outcome off = run(0, p.code, p.device, stable, lc, budget, opt);
                        const Outcome dry = run(1, p.code, p.device, stable, lc, budget, opt);
                        const Outcome on = run(2, p.code, p.device, stable, lc, budget, opt);
                        if (off.regs != on.regs || off.memory != on.memory || off.regs != dry.regs) {
                            fprintf(stderr, "%s lc=%u budget=%u stable=%d opt=%d\n", p.name, lc, budget, stable, opt);
                            for (size_t i = 0; i < off.regs.size(); ++i)
                                if (off.regs[i] != on.regs[i])
                                    fprintf(stderr, "  [%zu] %u vs %u\n", i, off.regs[i], on.regs[i]);
                            require(false, "the skip changes nothing the guest can see");
                        }
                        require(dry.reads == off.reads, "dry reads as often");
                        if (p.polls && !stable) require(on.reads == off.reads, "unstable reads are all made");
                        if (p.waits && budget >= 300000 && (stable || !p.polls) && (!p.counted || lc == 100000)) {
                            // A wait long enough to skip: it does, and dry counted it.
                            require(dry.steady > 1000, "dry counts the iterations it would skip");
                            if (p.polls) require(on.reads * 100 < off.reads, "a stable wait reads far less");
                            ++skipped;
                        }
                    }
                }
            }
        }
    }
    require(skipped > 0, "some waits were skipped");
    printf("steady skip test passed (%u long waits)\n", skipped);
    return 0;
}
