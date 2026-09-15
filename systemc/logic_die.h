#pragma once
#include "simple_mem_if.h"
#include "online.h"
#include "Vlogic_die_top.h"
#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
struct LogicDie:sc_module {
    static constexpr uint32_t DRAM_BASE=0x80000000, MMIO_BASE=0xf000, MAILBOX=0xf400;
    static constexpr uint64_t DRAM_BYTES=64ull*1024*1024;
    sc_in<bool> clk,rst_n;
    sc_fifo_in<SimpleMemRequest> request;
    sc_fifo_out<SimpleMemResponse> response;
    sc_signal<bool> gate_busy,gate_done,gate_error;
    sc_signal<unsigned> gate_state,gate_mask;
    Vlogic_die_top rtl;
    ss_mem* mem;
    std::array<uint8_t,1024> mailbox{};
    uint64_t dma_bytes=0,host_bytes=0,dma_requests=0,host_requests=0;
    std::ofstream transactions;
    SC_HAS_PROCESS(LogicDie);
    LogicDie(sc_module_name n,const std::string& out):sc_module(n),
        mem(ss_mem_create("HBM4",8,1,16,DRAM_BYTES,out.c_str())),
        transactions(out+"/memory.csv") {
        if(!mem)throw std::runtime_error(ss_mem_error());
        transactions<<"time_fs,event,origin,id,address,status,write,issued_cycle,completion_cycle,data_hex\n";
        rtl.clk=0;rtl.rst_n=0;rtl.csr_we=0;rtl.csr_wstrb=15;rtl.eval();
        SC_THREAD(control);sensitive<<clk.pos();
        SC_THREAD(memory_clock);
    }
    ~LogicDie(){ss_mem_destroy(mem);}
    bool idle()const{return jobs.empty()&&parts.empty()&&!dma_queued&&!dma_done&&!rtl.busy;}
    uint32_t mailbox_word(unsigned offset)const {
        uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(mailbox.at(offset+i))<<(8*i);return v;
    }
    void finish(){if(ss_mem_finish(mem))throw std::runtime_error(ss_mem_error());}
private:
    struct Job {
        SimpleMemRequest req;SimpleMemResponse rsp;unsigned issued=0,returned=0,word=0;
        bool complete=false,mmio=false,scratch=false;
    };
    struct Part {std::shared_ptr<Job> job;unsigned beat;uint32_t addr;bool dma;};
    std::deque<std::shared_ptr<Job>> jobs;
    std::map<uint64_t,Part> parts;
    struct Dma {uint32_t addr;};
    std::optional<Dma> dma_queued;
    std::optional<ss_mem_response> dma_done;
    bool dma_inflight=false,prefer_dma=true;
    uint64_t next_id=1;
    void receive() {
        if(jobs.size()>=4)return;
        SimpleMemRequest r;if(!request.nb_read(r))return;
        auto j=std::make_shared<Job>();j->req=std::move(r);
        j->rsp.write=j->req.write;j->rsp.rp=j->req.rp;j->rsp.id=j->req.address.id;j->rsp.user=j->req.address.user;
        if(!j->req.write)j->rsp.read_beats.resize(j->req.beats());
        const uint64_t a=j->req.address.addr;
        const uint64_t bytes=j->req.beats()*32ull;
        j->mmio=a>=MMIO_BASE&&a+bytes<=MAILBOX;
        j->scratch=a>=MAILBOX&&a+bytes<=MAILBOX+mailbox.size();
        if(j->req.address.size!=5||a%32||(!j->mmio&&!j->scratch&&(a<DRAM_BASE||a+bytes>uint64_t(DRAM_BASE)+DRAM_BYTES))) {
            j->rsp.resp=3;for(auto&b:j->rsp.read_beats)b.resp=3;j->complete=true;
        }
        jobs.push_back(j);
    }
    void mmio() {
        for(auto&j:jobs) {
            if(j->complete||(!j->mmio&&!j->scratch))continue;
            if(j->scratch) {
                unsigned offset=j->req.address.addr-MAILBOX;
                for(unsigned b=0;b<j->req.beats();++b)for(unsigned x=0;x<32;++x) {
                    if(j->req.write) {if(j->req.write_beats[b].strobe[x])mailbox[offset+b*32+x]=j->req.write_beats[b].data[x];}
                    else j->rsp.read_beats[b].data[x]=mailbox[offset+b*32+x];
                }
                j->complete=true;return;
            }
            if(!j->req.write) {
                rtl.csr_addr=j->req.address.addr-MMIO_BASE;rtl.eval();
                if(rtl.csr_stall)return;
                for(unsigned b=0;b<j->req.beats();++b)for(unsigned w=0;w<8;++w) {
                    rtl.csr_addr=j->req.address.addr-MMIO_BASE+b*32+w*4;rtl.eval();
                    auto value=rtl.csr_rdata;
                    for(unsigned x=0;x<4;++x)j->rsp.read_beats[b].data[w*4+x]=value>>(x*8);
                }
                j->complete=true;return;
            }
            while(j->word<j->req.beats()*8) {
                unsigned w=j->word++, b=w/8,x=(w%8)*4;
                const auto& beat=j->req.write_beats[b];
                unsigned mask=0;uint32_t value=0;
                for(unsigned k=0;k<4;++k){mask|=unsigned(bool(beat.strobe[x+k]))<<k;value|=uint32_t(beat.data[x+k])<<(k*8);}
                if(!mask)continue;
                rtl.csr_we=1;rtl.csr_wstrb=mask;rtl.csr_addr=j->req.address.addr-MMIO_BASE+w*4;
                rtl.csr_wdata=value;rtl.eval();if(rtl.csr_error)j->rsp.resp=2;
                if(j->word==j->req.beats()*8)j->complete=true;
                return;
            }
            j->complete=true;return;
        }
    }
    void control() {
        while(true) {
            wait();
            rtl.clk=0;rtl.rst_n=rst_n.read();rtl.csr_we=0;
            rtl.dma_ready=!dma_queued&&!dma_done&&!dma_inflight;
            rtl.dma_rsp_valid=dma_done.has_value();
            rtl.dma_rsp_error=dma_done&&dma_done->status>1;
            for(unsigned i=0;i<8;++i) {
                uint32_t v=0;if(dma_done)for(unsigned b=0;b<4;++b)v|=uint32_t(dma_done->data[i*4+b])<<(8*b);
                rtl.dma_rsp_data[i]=v;
            }
            rtl.eval();
            if(rst_n.read()){receive();mmio();}
            const bool req=rtl.dma_valid&&rtl.dma_ready;
            const bool rsp=rtl.dma_rsp_valid&&rtl.dma_rsp_ready;
            auto address=rtl.dma_addr;
            rtl.clk=1;rtl.eval();
            if(rsp)dma_done.reset();
            if(req)dma_queued=Dma{address};
            gate_busy=rtl.busy;gate_done=rtl.done;gate_error=rtl.error;gate_state=rtl.debug_state;gate_mask=rtl.result_mask;
            if(!jobs.empty()&&jobs.front()->complete&&response.nb_write(jobs.front()->rsp))jobs.pop_front();
        }
    }
    void submit() {
        std::shared_ptr<Job> h;
        for(auto& j:jobs)if(!j->complete&&!j->mmio&&!j->scratch&&j->issued<j->req.beats()){h=j;break;}
        bool dma=dma_queued&&(!h||prefer_dma);
        if(!dma&&!h)return;
        uint32_t address=dma?dma_queued->addr:uint32_t(h->req.address.addr+h->issued*32);
        if(address<DRAM_BASE||uint64_t(address)+32>uint64_t(DRAM_BASE)+DRAM_BYTES) {
            if(!dma)throw std::runtime_error("host bounds check escaped");
            ss_mem_response e{};e.status=2;dma_done=e;dma_queued.reset();return;
        }
        bool wr=!dma&&h->req.write;
        const uint8_t* data=wr?h->req.write_beats[h->issued].data.data():nullptr;
        const uint8_t* mask=wr?h->req.write_beats[h->issued].strobe.data():nullptr;
        int ok=ss_mem_submit(mem,next_id,address-DRAM_BASE,32,wr,data,mask);
        if(ok<0)throw std::runtime_error(ss_mem_error());
        if(!ok)return;
        parts[next_id]={h,dma?0:h->issued,address,dma};
        transactions<<sc_time_stamp().value()<<",submit,"<<(dma?"logic_die":"host")<<','<<next_id<<','<<address<<",0,"<<wr<<",0,0,\n";
        ++next_id;prefer_dma=!dma;
        if(dma){dma_queued.reset();dma_inflight=true;dma_bytes+=32;++dma_requests;}
        else{++h->issued;host_bytes+=32;++host_requests;}
    }
    void collect() {
        ss_mem_response r;
        while(true) {
            int n=ss_mem_pop(mem,&r);if(n<0)throw std::runtime_error(ss_mem_error());if(!n)break;
            auto it=parts.find(r.id);if(it==parts.end())throw std::runtime_error("unknown native response");
            auto p=it->second;parts.erase(it);
            const bool write=!p.dma&&p.job->req.write;
            transactions<<sc_time_stamp().value()<<",complete,"<<(p.dma?"logic_die":"host")<<','<<r.id<<','<<p.addr<<','<<r.status<<','<<write
                <<','<<r.issued_cycle<<','<<r.completion_cycle<<',';
            if(!write) {
                static const char*hex="0123456789abcdef";
                for(unsigned i=0;i<32;++i)transactions<<hex[r.data[i]>>4]<<hex[r.data[i]&15];
            }
            transactions<<'\n';
            if(p.dma){if(dma_done)throw std::runtime_error("DMA response overwritten");dma_done=r;dma_inflight=false;}
            else{
                unsigned status=r.status<=1?0:2;
                if(p.job->req.write)p.job->rsp.resp|=status;
                else{auto&b=p.job->rsp.read_beats[p.beat];b.resp=status;std::copy_n(r.data,32,b.data.begin());}
                if(++p.job->returned==p.job->req.beats())p.job->complete=true;
            }
        }
    }
    void memory_clock() {
        while(true) {
            if(rst_n.read())submit();
            wait(sc_time(ss_mem_period_fs(mem),SC_FS));
            if(ss_mem_step(mem))throw std::runtime_error(ss_mem_error());
            collect();
        }
    }
};
