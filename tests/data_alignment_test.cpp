#include "core.h"
#include "mmr.h"
#include "../src/evt.h"
#include "../src/cec.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
class Ram : public Memory {
public:
    std::array<uint8_t,4096> bytes{};
    int mode, faults=0; uint32_t fault_pc=0, fault_addr=0, status=0;
    explicit Ram(int m):mode(m) {}
    uint32_t base() const override {return 0;}
    uint32_t size() const override {return bytes.size();}
    const uint8_t* raw() const override {return bytes.data();}
    uintptr_t fast_base() const override {return reinterpret_cast<uintptr_t>(bytes.data());}
    bool is_fastmem() const override {return mode==2;}
    uint32_t rawmem_limit() const override {return mode==1 ? bytes.size() : 0;}
    std::vector<uint8_t*> pages;
    uint8_t* const* page_table() const override {
        if(mode!=3) return nullptr;
        auto& table=const_cast<std::vector<uint8_t*>&>(pages);
        if(table.empty()) {table.assign(kPageTableEntries,nullptr);table[0]=const_cast<uint8_t*>(bytes.data());}
        return table.data();
    }
    uint8_t read8(uint32_t a) const override {return bytes.at(a);}
    uint16_t read16(uint32_t a) const override {uint16_t v;memcpy(&v,&bytes.at(a),2);return v;}
    uint32_t read32(uint32_t a) const override {uint32_t v;memcpy(&v,&bytes.at(a),4);return v;}
    void write8(uint32_t a,uint8_t v) override {bytes.at(a)=v;}
    void write16(uint32_t a,uint16_t v) override {memcpy(&bytes.at(a),&v,2);}
    void write32(uint32_t a,uint32_t v) override {memcpy(&bytes.at(a),&v,4);}
    void data_alignment_fault(uint32_t pc,uint32_t addr,uint32_t st) override {
        ++faults;fault_pc=pc;fault_addr=addr;status=st;
    }
};
int main() {
    int count=0;
    for(int mode=0;mode<4;++mode) for(int opt:{0,2}) {
        for(uint16_t op:{0x9000,0x9200,0x9400,0x9600,0x9800,0x9a00}) for(unsigned offset:{0u,1u,2u,3u}) {
            Ram mem(mode);CpuState cpu{}; Core core(&cpu,&mem);require(core.init(opt),"init");
            cec_init();evt_init();evt_write(EVT_BASE+12,0x300);
            // Supervisor IRQ context, with exceptions still able to preempt.
            cec_mmr_write(&cpu,CEC_MMR_BASE+4,1u<<7);
            evt_write(EVT_BASE+28,0x100);cec_raise(&cpu,7);cec_return_rti(&cpu);
            mem.write16(0x100,op);mem.write16(0x102,0x2000);
            mem.write32(0x200,0x12345678);
            cpu.dpregs[0]=0xdeadbeef;cpu.dpregs[8]=0x200+offset;
            require(core.run(0x100),"run");
            bool byte=op==0x9800 || op==0x9a00;
            bool write=op==0x9200 || op==0x9600 || op==0x9a00;
            unsigned width=byte?1:(op==0x9400||op==0x9600?2:4);
            if(offset & (width-1)) {
                require(mem.faults==1,"one alignment fault");
                require((cpu.seqstat&63)==0x24 && cpu.retx==0x100 && cpu.pc==0x300,"EVT3 and fault PC");
                require(mem.fault_addr==0x200+offset && mem.fault_pc==0x100,"fault address");
                require(mem.status==((1u<<17)|(write?1u<<16:0)),"fault status");
                require(cpu.dpregs[8]==0x200+offset && cpu.dpregs[0]==0xdeadbeef,"no destination/postincrement commit");
                require(mem.read32(0x200)==0x12345678,"no failed store");
            } else {
                require(mem.faults==0,"legal access");
                require(cpu.dpregs[8]>0x200+offset,"postincrement on success");
            }
            ++count;
        }
        {
            Ram mem(mode);CpuState cpu{};Core core(&cpu,&mem);require(core.init(opt),"push init");
            cec_init();evt_init();evt_write(EVT_BASE+12,0x300);
            cec_mmr_write(&cpu,CEC_MMR_BASE+4,1u<<7);
            evt_write(EVT_BASE+28,0x100);cec_raise(&cpu,7);cec_return_rti(&cpu);
            mem.write16(0x100,0x0140);mem.write16(0x102,0x2000); // [--SP] = R0
            cpu.dpregs[14]=0x205;cpu.dpregs[0]=0x11223344;
            require(core.run(0x100),"push run");
            require(mem.faults==1 && mem.fault_addr==0x201 && cpu.dpregs[14]==0x205,"failed push preserves SP");
            ++count;
        }
        {
            Ram mem(mode);CpuState cpu{};Core core(&cpu,&mem);require(core.init(opt),"jump init");
            cec_init();evt_init();evt_write(EVT_BASE+12,0x300);cec_return_rti(&cpu);
            mem.write16(0x100,0);mem.write16(0x102,0x0050);cpu.dpregs[8]=0x201;
            require(core.run(0x100),"odd indirect jump");
            require((cpu.seqstat&63)==0x2a && cpu.retx==0x102 && cpu.pc==0x300,"instruction alignment uses 0x2a and fault PC");
            ++count;
        }
        // Parallel DAG1 fault must retain packet PC and discard earlier register writes.
        for(bool supervisor:{false,true}) {
            Ram mem(mode);CpuState cpu{};Core core(&cpu,&mem);require(core.init(opt),"parallel init");
            cec_init();evt_init();evt_write(EVT_BASE+12,0x300);
            if(supervisor) {
                cec_mmr_write(&cpu,CEC_MMR_BASE+4,1u<<7);
                evt_write(EVT_BASE+28,0x100);cec_raise(&cpu,7);
            }
            cec_return_rti(&cpu);
            mem.write16(0x100,0x0000); // NOP: fault is not at the block's entry PC.
            mem.write32(0x102,0x2413cc04);mem.write32(0x106,0x9c099c00);
            mem.write16(0x10a,0x2000);mem.write32(0x200,0x12345678);
            cpu.iregs[0]=0x200;cpu.iregs[1]=0x203;
            cpu.dpregs[0]=11;cpu.dpregs[1]=12;cpu.dpregs[2]=13;cpu.dpregs[3]=14;
            require(core.run(0x100),"parallel run");
            require(mem.faults==1 && mem.fault_pc==0x102 && cpu.retx==0x102,"packet fault PC");
            require(mem.status==((1u<<18)|(supervisor?1u<<17:0)),"DAG1 and privilege flags");
            require(cpu.dpregs[0]==11 && cpu.dpregs[1]==12 && cpu.dpregs[2]==13 && cpu.iregs[0]==0x200,"parallel register writes discarded");
            ++count;
        }
        Ram mem(mode);CpuState cpu{};Core core(&cpu,&mem);require(core.init(opt),"init suppression");
        cec_init();evt_init();
        mem.write32(0x100,0xc000cc12);mem.write32(0x104,0x00009c00);mem.write16(0x108,0x2000);
        mem.write32(0x200,0x12345678);cpu.iregs[0]=0x201;
        require(core.run(0x100),"run DISALGNEXCPT");
        require(mem.faults==0 && cpu.dpregs[0]==0x12345678 && cpu.iregs[0]==0x205,"suppressed load aligns down");
        ++count;
        // DISALGNEXCPT lasts for its packet only: a later load in the same block faults.
        for(bool cp:{false,true}) {
            if(cp && mode!=3) continue;  // copy-and-patch needs the page table
            Ram mem(mode);CpuState cpu{};Core core(&cpu,&mem);require(core.init(opt),"init packet scope");
            core.set_copy_patch(cp);
            cec_init();evt_init();evt_write(EVT_BASE+12,0x300);cec_return_rti(&cpu);
            mem.write32(0x100,0xc000cc12);mem.write32(0x104,0x00009c00);  // DISALGNEXCPT || R0 = [I0++] || NOP
            mem.write16(0x108,0x9c01);mem.write16(0x10a,0x2000);           // R1 = [I0++]
            mem.write32(0x200,0x12345678);cpu.iregs[0]=0x201;cpu.dpregs[1]=7;
            require(core.run(0x100),"run packet scope");
            require(cpu.dpregs[0]==0x12345678,"the packet's own load aligns down");
            require(mem.faults==1 && mem.fault_addr==0x205 && cpu.retx==0x108,"the next packet's load faults");
            require(cpu.dpregs[1]==7 && cpu.iregs[0]==0x205,"no commit of the faulting load");
            ++count;
        }
    }
    printf("PASS: %d data alignment cases (callback/raw/fastmem/page table, O0/O2)\n",count);
}
