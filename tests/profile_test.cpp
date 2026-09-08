#include "core.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
class Ram : public Memory {
public:
    std::vector<uint8_t> bytes = std::vector<uint8_t>(4096);
    uint32_t base() const override { return 0; }
    uint32_t size() const override { return bytes.size(); }
    uint8_t read8(uint32_t a) const override { return bytes.at(a); }
    uint16_t read16(uint32_t a) const override { return read8(a) | uint16_t(read8(a+1))<<8; }
    uint32_t read32(uint32_t a) const override { return read16(a) | uint32_t(read16(a+2))<<16; }
    void write8(uint32_t a,uint8_t v) override { bytes.at(a)=v; }
    void write16(uint32_t a,uint16_t v) override { write8(a,v); write8(a+1,v>>8); }
    void write32(uint32_t a,uint32_t v) override { write16(a,v); write16(a+2,v>>16); }
    const uint8_t* raw() const override { return bytes.data(); }
    uintptr_t fast_base() const override { return reinterpret_cast<uintptr_t>(bytes.data()); }
};
struct Sink : BcoreEventSink {
    int begins=0,ends=0,stages=0,hits=0;
    void onTranslateBegin(uint32_t) override { ++begins; }
    void onTranslateEnd(uint32_t,uint64_t) override { ++ends; }
    void onCacheHit(uint32_t) override { ++hits; }
    void onCompileStage(uint32_t,const char*,uint64_t b,uint64_t e,bool ok) override {
        require(e>=b && ok,"balanced successful stage"); ++stages;
    }
};
int main(int argc,char**) {
    Ram mem; mem.write16(0,0x2000); mem.write16(2,0x2000); // JUMP.S 0
    CpuState cpu{};
    Core core(&cpu,&mem); require(core.init(0),"init");
    Sink sink; core.setEventSink(&sink); core.setProfileHistory(2);
    require(core.run(0),"initial dispatch");
    const auto reference=cpu;
    core.setBlockCostProbe(0,0);
    for(int i=0;i<100;++i) require(core.run(0),"sham dispatch");
#if BCORE_ENABLE_PROFILE
    auto p=core.blockCostProbeStats();
    require(p.executions==100 && p.elapsed_ns>0 && p.requested_ns==0,"PC0 sham is armed and measured");
    require(sink.begins==1 && sink.ends==1 && sink.stages==2 && sink.hits==0,"sparse miss-only events");
    core.setBlockCostProbe(0,500);
    for(int i=0;i<100;++i) require(core.run(0),"dose dispatch");
    p=core.blockCostProbeStats();
    require(p.executions==100 && p.elapsed_ns>=50000,"actual dose accounting");
    require(std::memcmp(&reference,&cpu,sizeof(cpu))==0,"probe preserves complete guest state");
    core.clearBlockCostProbe(); core.run(0);
    require(core.blockCostProbeStats().executions==100,"clear disarms");
    std::vector<BcoreBlockInfo> before;
    core.forEachCompiledBlock([&](const auto& b){before.push_back(b);});
    require(before.size()==1 && before[0].host_size>0 && before[0].loaded_ns>0,"exact code lifetime");
    core.invalidate(); core.run(2);
    std::vector<BcoreBlockInfo> history;
    core.forEachProfileBlock([&](const auto& b){history.push_back(b);});
    require(history.size()==2 && history[0].guest_pc==0 && history[0].unloaded_ns>0 &&
            history[1].guest_pc==2 && history[1].loaded_ns>=history[0].unloaded_ns,"old code retained across invalidation");
    core.invalidate(); core.run(0); core.invalidate(); core.run(2);
    require(core.profileHistoryDropped()==1,"bounded history loss visible");
#else
    require(core.stats().blocks_executed==0 && core.blockCostProbeStats().executions==0,"disabled stubs");
    size_t n=0; core.forEachCompiledBlock([&](const auto& b){++n;require(b.host_size==0,"unknown sizes without listener");});
    require(n==1,"disabled map fallback");
#endif
    core.setEventSink(nullptr);
    if(argc>1) {
        core.run(0);
        for(int round=0;round<6;++round) {
            for(int j=0;j<3;++j) {
                const int index=round%2 ? 2-j : j;
                const uint64_t dose=uint64_t(index)*200;
                core.setBlockCostProbe(0,dose);
                const auto begin=std::chrono::steady_clock::now();
                for(int i=0;i<300000;++i) core.run(0);
                const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
                const auto p=core.blockCostProbeStats();
                std::printf("round=%d dose=%llu executions=%llu envelope_ns=%llu wall_ns=%lld\n",round,
                            (unsigned long long)dose,(unsigned long long)p.executions,
                            (unsigned long long)p.elapsed_ns,(long long)elapsed);
            }
        }
    }
    std::puts("profile tests passed");
}
