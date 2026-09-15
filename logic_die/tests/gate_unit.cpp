#include "Vlogic_die_top.h"
#include <verilated.h>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>
#define REQUIRE(c,m) do {if(!(c)) throw std::runtime_error(m);} while(0)
struct Model {
    Vlogic_die_top d;
    std::vector<uint8_t> mem=std::vector<uint8_t>(64*1024*1024,0);
    std::mt19937 rng{813};
    uint64_t ticks=0, accepted=0, stalls=0;
    bool pending=false, held=false;
    uint32_t address=0, held_addr=0;
    unsigned delay=0;
    bool fault=false;
    void step() {
        d.clk=0; d.eval();
        d.dma_ready= !pending && (rng()%4!=0);
        d.dma_rsp_valid=pending && delay==0;
        d.dma_rsp_error=fault && d.dma_rsp_valid;
        for(unsigned i=0;i<8;++i) {
            uint32_t w=0;
            for(unsigned b=0;b<4;++b) w|=uint32_t(mem[address+i*4+b])<<(8*b);
            d.dma_rsp_data[i]=w;
        }
        d.eval();
        if(held) REQUIRE(d.dma_valid && d.dma_addr==held_addr,"DMA changed while stalled");
        held=d.dma_valid && !d.dma_ready; held_addr=d.dma_addr;
        stalls+=held;
        const bool req=d.dma_valid && d.dma_ready;
        const bool rsp=d.dma_rsp_valid && d.dma_rsp_ready;
        const uint32_t a=d.dma_addr;
        d.clk=1; d.eval(); ++ticks;
        if(rsp) pending=false;
        if(pending && delay) --delay;
        if(req) {
            REQUIRE(!pending,"duplicate outstanding DMA");
            REQUIRE((a&31)==0,"unaligned DMA");
            pending=true; address=a; delay=1+rng()%9; ++accepted;
        }
        d.clk=0; d.eval();
    }
    Model() {
        d.rst_n=0; d.csr_we=0; d.csr_wstrb=15;
        for(int i=0;i<3;++i) step();
        d.rst_n=1; step();
    }
    void wr(unsigned a,uint32_t v,bool reject=false) {
        d.csr_we=1;d.csr_addr=a;d.csr_wdata=v;d.eval();
        REQUIRE(bool(d.csr_error)==reject,"CSR rejection mismatch");
        step();d.csr_we=0;d.eval();
    }
    uint32_t rd(unsigned a) {d.csr_addr=a;d.eval();return d.csr_rdata;}
    void run() {
        for(unsigned t=0;t<5000000;++t) {
            if(d.done && !d.busy) return;
            step();
        }
        throw std::runtime_error("RTL job timeout");
    }
    void store(uint32_t addr,const std::vector<uint16_t>& v) {
        for(auto x:v) {mem[addr++]=x&255;mem[addr++]=x>>8;}
    }
};
static uint32_t u32(std::istream& f) {uint32_t x;f.read((char*)&x,4); REQUIRE(bool(f),"short fixture");return x;}
struct Query {uint32_t current,mask,count; std::vector<uint32_t> indices;std::vector<uint16_t> q;};
int main(int argc,char**argv) {
    try {
        REQUIRE(argc==2,"usage: gate_unit fixture.bin");
        std::ifstream f(argv[1],std::ios::binary); REQUIRE(bool(f),"fixture missing");
        REQUIRE(u32(f)==0x314d444c,"fixture magic");
        auto chunks=u32(f), log2=u32(f), k=u32(f), nq=u32(f);
        std::vector<Query> queries(nq);
        for(auto& q:queries) {
            q.current=u32(f);q.mask=u32(f);q.count=u32(f);
            for(unsigned i=0;i<q.count;++i) q.indices.push_back(u32(f));
            q.q.resize(128);f.read((char*)q.q.data(),256);
        }
        std::vector<uint16_t> keys(chunks*(1u<<log2)*128);
        f.read((char*)keys.data(),keys.size()*2);REQUIRE(bool(f),"short keys");
        Model m;m.store(0x10000,keys);
        m.wr(4,0x10000);m.wr(8,0x1000);m.wr(12,chunks);
        m.wr(16,log2);m.wr(20,k);m.wr(24,chunks-1);m.wr(28,23);
        m.wr(12,65,true);m.wr(16,16,true);m.wr(20,64,true);m.wr(24,32,true);
        m.d.csr_wstrb=1;m.wr(4,0x20000,true);m.d.csr_wstrb=15;
        REQUIRE(m.rd(4)==0x10000,"rejected write changed descriptor");
        m.wr(0,2,true); // no resident weights
        m.wr(0,1);m.wr(4,0x20000,true);
        m.rd(0x300);REQUIRE(m.d.csr_stall,"completion read did not stall");m.run();
        m.rd(0x300);REQUIRE(!m.d.csr_stall,"completion read stuck after job");
        REQUIRE(!m.d.error,"prepare failed");
        auto prep_cycles=m.rd(36);
        REQUIRE(m.rd(52)==keys.size()*2/32,"pool DMA traffic mismatch");
        m.wr(28,24);m.wr(0,2,true);m.wr(28,23);
        std::cout<<"{\"prepare_cycles\":"<<prep_cycles<<",\"query_cycles\":[";
        for(unsigned j=0;j<nq;++j) {
            const auto& q=queries[j];m.store(0x1000,q.q);m.wr(24,q.current);
            m.wr(0,2);m.run();REQUIRE(!m.d.error,"query failed");
            REQUIRE(m.d.result_mask==q.mask,"Top-K mask differs from independent oracle");
            REQUIRE(m.d.result_count==q.count,"Top-K count mismatch");
            for(unsigned i=0;i<q.count;++i) REQUIRE(m.rd(128+4*i)==q.indices[i],"Top-K rank/tie order mismatch");
            REQUIRE(m.rd(52)==8,"resident query re-read K");
            if(j) std::cout<<',';
            std::cout<<m.rd(36);
        }
        // A transport error must fail, and a subsequent successful preparation must recover.
        if(chunks>1) {
            m.store(0x10000,std::vector<uint16_t>(keys.size(),0x3f80));m.wr(0,1);m.run();
            m.store(0x1000,std::vector<uint16_t>(128,0x7f7f));m.wr(24,1);m.wr(0,2);m.run();
            REQUIRE(m.d.error && m.d.result_count==0,"nonfinite dot accumulation was accepted");
            m.store(0x10000,keys);
        }
        m.mem[0x1000]=0x80;m.mem[0x1001]=0x7f;
        m.wr(0,2);m.run();REQUIRE(m.d.error,"nonfinite Q was accepted");
        m.fault=true;m.wr(0,1);m.run();REQUIRE(m.d.error && !(m.rd(0)&8),"DMA fault did not invalidate residency");
        m.fault=false;m.wr(0,1);m.run();REQUIRE(!m.d.error,"recovery preparation failed");
        std::cout<<"],\"dma_stalls\":"<<m.stalls<<",\"passed\":true}\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
