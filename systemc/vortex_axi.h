#pragma once
#include "axi_bus.h"
#include "api.h"
#include <deque>
#include <functional>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
struct VortexAxi: sc_module {
    sc_in<bool> clk,rst_n;
    AxiBus& bus;
    struct Job {ss::VortexRequest req;std::function<void(const std::array<uint8_t,64>&)> host_done;};
    std::deque<Job> queue;
    ss::Vortex vortex;
    bool running=false, finished=false;
    uint64_t gpu_cycles=0, requests=0, read_bytes=0, write_bytes=0, issue_stalls=0;
    uint64_t r_stalls=0,b_stalls=0;
    std::ofstream log;
    SC_HAS_PROCESS(VortexAxi);
    VortexAxi(sc_module_name n,AxiBus& b,const std::string& dir):sc_module(n),bus(b),
        vortex([this](const ss::VortexRequest& r){
            if(queue.size()>=16) {++issue_stalls;return false;}
            queue.push_back({r,{}});++requests;
            (r.write?write_bytes:read_bytes)+=r.size;return true;
        }),log(dir+"/axi.csv") {
        log<<"time_fs,origin,channel,id,address,beat,data_hex,strobe_hex\n";
        SC_THREAD(run);sensitive<<clk.pos();
    }
    bool enqueue(const ss::VortexRequest& r,std::function<void(const std::array<uint8_t,64>&)> cb) {
        if(queue.size()>=16)return false;
        queue.push_back({r,std::move(cb)});return true;
    }
    void start(uint32_t pc){vortex.start(pc,0);running=true;}
    bool idle() const{return queue.empty()&&!active.has_value();}
private:
    enum Stage {AW,W,B,AR,R};
    Stage stage=AW;
    std::optional<Job> active;
    unsigned beat=0;uint16_t next_id=0,id=0;
    std::array<uint8_t,64> response{};
    std::mt19937 rng{2026};
    void record(const char*ch,const uint8_t*data=nullptr,const uint8_t*mask=nullptr) {
        log<<sc_time_stamp().value()<<','<<(active->host_done?"loader":"vortex")<<','<<ch<<','<<id<<','
           <<active->req.address<<','<<beat<<',';
        static const char* hex="0123456789abcdef";
        if(data)for(unsigned i=0;i<32;++i)log<<hex[data[i]>>4]<<hex[data[i]&15];
        log<<',';
        if(mask)for(unsigned i=0;i<32;++i)log<<int(mask[i]!=0);
        log<<'\n';
    }
    void complete() {
        if(active->host_done)active->host_done(response);
        else vortex.complete(active->req.token,response.data(),active->req.size);
        active.reset();
    }
    void run() {
        while(true) {
            wait();
            if(!rst_n.read()) {
                bus.av=0;bus.wv=0;bus.arv=0;bus.br=0;bus.rr=0;continue;
            }
            if(active) {
                if(stage==AW&&bus.av&&bus.ar){record("AW");stage=W;beat=0;}
                else if(stage==W&&bus.wv&&bus.wr){
                    record("W",bus.w.read().data,bus.w.read().strb);
                    if(++beat==active->req.size/32)stage=B;
                } else if(stage==B) {
                    b_stalls+=bus.bv&&!bus.br;
                    if(bus.bv&&bus.br) {
                        if(bus.b.read().id!=id||bus.b.read().resp)throw std::runtime_error("AXI write failed");
                        record("B");complete();
                    }
                } else if(stage==AR&&bus.arv&&bus.arr){record("AR");stage=R;beat=0;}
                else if(stage==R) {
                    r_stalls+=bus.rv&&!bus.rr;
                    if(bus.rv&&bus.rr) {
                        const auto& r=bus.r.read();
                        if(r.id!=id||r.resp||r.last!=(beat+1==active->req.size/32))
                            throw std::runtime_error("AXI read response contract failed");
                        record("R",r.data);
                        std::copy_n(r.data,32,response.begin()+beat*32);
                        if(++beat==active->req.size/32)complete();
                    }
                }
            }
            if(!active&&!queue.empty()) {
                active=std::move(queue.front());queue.pop_front();id=(next_id++)&1023u;beat=0;response.fill(0);
                if(active->req.size!=32&&active->req.size!=64)throw std::runtime_error("unsupported VORTEX transfer width");
                stage=active->req.write?AW:AR;
            }
            bus.av=active&&stage==AW;bus.wv=active&&stage==W;bus.arv=active&&stage==AR;
            bus.br=active&&stage==B&&(rng()%4!=0);bus.rr=active&&stage==R&&(rng()%4!=0);
            if(active) {
                AxChannel a{};a.id=id;a.addr=active->req.address;a.len=active->req.size/32-1;
                a.size=5;a.burst=1;bus.aw=a;bus.ra=a;
                WChannel w{};
                if(stage==W) {
                    for(unsigned j=0;j<32;++j){w.data[j]=active->req.data[beat*32+j];w.strb[j]=(active->req.byteen>>(beat*32+j))&1;}
                    w.last=beat+1==active->req.size/32;
                }
                bus.w=w;
            }
            if(running) {
                ++gpu_cycles;
                if(!vortex.cycle()){running=false;finished=true;}
            }
        }
    }
};
