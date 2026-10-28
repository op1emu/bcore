// A host's own names must not reach into bcore's: a host that links bcore's
// static libraries may name its globals as it likes. This one, which
// includes none of bcore's headers, has global types named as bcore's are
// (Access, Tail, BBInfo, Memory, CpuState, Compiler), laid out otherwise
// and used the way bcore uses its own, so this binary has its own copies of
// the members a clash would share; and C functions named as bcore's
// helpers once were. Were bcore's names global too, the linker would keep
// one copy of each shared member, whichever layout it has, and say nothing
// (a loop block's pass threw std::length_error, JitEngine::lookup returned
// another function than it was given), or two definitions of mem_read8.
// host_types_bridge.cpp, which includes bcore's headers, runs bcore.
//
// Built at -O0 (CMakeLists.txt): optimized, this file would inline every
// member it uses and keep none for the linker to choose between.
//
//   bcore-host-types-test FILE    a loop block's module (tests/data/), optimized at O1 and O2

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

// The host's types: global, and sized unlike bcore's.
struct Access {
    uint32_t addr, value, pc, packets;
    uint8_t width, write;
};
struct Tail {
    uint64_t value[3] = {};
    bool any() const { return value[0] != 0; }
};
struct alignas(64) BBInfo {
    unsigned char bytes[64] = {};
};
class Memory {
public:
    virtual ~Memory() = default;
    virtual uint32_t read(uint32_t addr) const { return addr; }
};
struct CpuState {
    uint64_t regs[3] = {};
};
class Compiler {
public:
    bool init(int level) { return level >= 0; }
};

// The host's C functions, named as bcore's helpers were.
extern "C" uint8_t mem_read8(void*, uint32_t addr) { return uint8_t(addr); }
extern "C" void cec_raise(void*, uint32_t) {}

int run_bcore(const char* file);

namespace {

volatile uint32_t sink;
void use_host_types() {
    std::vector<Access> trace;
    for (uint32_t i = 0; i < 64; ++i) trace.push_back({i, i, i, i, 4, 0});
    Tail tail;
    tail.value[0] = trace.size();
    std::unordered_map<uint32_t, BBInfo> blocks;
    blocks.reserve(16);
    const uint32_t key = 42;  // operator[](const key_type&), as JitEngine's
    blocks[key].bytes[0] = 7;
    blocks.emplace(43, BBInfo{});
    Memory mem;
    CpuState cpu;
    Compiler compiler;
    cec_raise(&cpu, 0);
    sink = uint32_t(trace.size()) + tail.any() + uint32_t(blocks.count(42) + blocks.find(43)->second.bytes[0]) +
           mem.read(1) + uint32_t(cpu.regs[0]) + compiler.init(1) + mem_read8(nullptr, 2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    use_host_types();
    return run_bcore(argv[1]);
}
