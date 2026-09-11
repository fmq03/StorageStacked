#include "memsim_backend.hh"
#include "axi_contract.h"
#include "sim/cur_tick.hh"
#include <algorithm>
#include <iomanip>
#include <stdexcept>
using namespace sc_core;

MemSimBackend::MemSimBackend(sc_module_name name,uint64_t base_,uint64_t size_,
 unsigned slots_,unsigned channels,unsigned scale,unsigned queue,unsigned hold_,const std::string& dir_)
 :sc_module(name),base(base_),size(size_),slots(slots_),hold(hold_),dir(dir_),log(dir+"/memsim_bridge.csv") {
 if(!slots || !size || base>UINT64_MAX-size)throw std::invalid_argument("invalid memory window/slots");
 mem=ss_mem_create("hbm4",channels,scale,queue,size,dir.c_str());
 if(!mem)throw std::runtime_error(ss_mem_error());
 period=ss_mem_period_fs(mem);
 log<<"tick,event,burst,rp,axi_id,command,address,bytes,offset,mem_id,issued_cycle,completion_cycle,status,data,mask\n";
 SC_THREAD(run);
}
MemSimBackend::~MemSimBackend(){ss_mem_destroy(mem);}
void MemSimBackend::checked(int result){if(result<0)SC_REPORT_FATAL("mem_sim",ss_mem_error());}
void MemSimBackend::event(const char* what,const Burst& b,unsigned off,unsigned n,
 uint64_t id,uint64_t issued,uint64_t completion,unsigned status,const uint8_t* data,const uint8_t* mask){
 const auto tick=gem5::curTick();sc_assert(tick==sc_time_stamp().value());
 log<<tick<<','<<what<<','<<b.serial<<','<<unsigned(b.req.rp)<<','<<b.req.address.id<<','
    <<(b.req.write?'W':'R')<<','<<(b.req.address.addr+off)<<','<<n<<','<<off<<','<<id<<','<<issued<<','<<completion<<','<<status<<',';
 if(data)for(unsigned i=0;i<n;++i)log<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(data[i]);
 log<<std::dec<<',';
 if(mask)for(unsigned i=0;i<n;++i)log<<(mask[i]?'1':'0');
 log<<'\n';
}
void MemSimBackend::accept(SimpleMemRequest req){
 auto b=std::make_shared<Burst>();b->serial=nextBurst++;b->arrived=gem5::curTick();b->req=std::move(req);
 const auto& a=b->req.address;
 if(axi_request_error(a) || (b->req.write && b->req.write_beats.size()!=b->req.beats()))
  SC_REPORT_FATAL("mem_sim","invalid AXI burst structure");
 const unsigned width=1u<<a.size;b->length=width*b->req.beats();
 b->data.resize(b->length);b->mask.resize(b->length,1);
 b->rsp.write=b->req.write;b->rsp.rp=b->req.rp;b->rsp.id=a.id;b->rsp.user=a.user;
 unsigned status=(a.addr<base || a.addr-base>=size || b->length>size-(a.addr-base))?3:(a.lock?2:0);
 if(b->req.write){
  for(unsigned beat=0;beat<b->req.beats();++beat){
   const unsigned lane=(a.addr+beat*width)%AXI_DATA_BYTES;
   const auto& w=b->req.write_beats[beat];
   for(unsigned j=0;j<AXI_DATA_BYTES;++j)if(w.strobe[j] && (j<lane || j>=lane+width))status=2;
   for(unsigned j=0;j<width;++j){b->data[beat*width+j]=w.data[lane+j];b->mask[beat*width+j]=w.strobe[lane+j]!=0;}
  }
 }else{
  b->rsp.read_beats.resize(b->req.beats());
  for(auto& r:b->rsp.read_beats){r.user=a.user;r.resp=status;}
 }
 b->rsp.resp=status;b->error=status!=0;
 if(b->error)b->sent=b->length;
 event("accept",*b,0,b->length,0,0,0,status,b->req.write?b->data.data():nullptr,b->req.write?b->mask.data():nullptr);
 bursts.push_back(b);
}
void MemSimBackend::run(){
 while(true){
  wait(sc_time::from_value(period));
  sc_assert((ss_mem_clock(mem)+1)*period==gem5::curTick());
  SimpleMemRequest req;
  if(bursts.size()<slots && request.nb_read(req))accept(std::move(req));
  // Finish submitting one burst before the next: preserves overlapping byte order.
  for(auto& b:bursts){
   if(b->sent==b->length)continue;
   const uint64_t addr=b->req.address.addr-base+b->sent;
   const unsigned granule=ss_mem_transaction_bytes(mem);
   const unsigned n=std::min<unsigned>(b->length-b->sent,granule-addr%granule);
   if(!b->candidate)b->candidate=nextMem++;
   int accepted=ss_mem_submit(mem,b->candidate,addr,n,b->req.write,b->data.data()+b->sent,b->mask.data()+b->sent);
   checked(accepted);
   if(accepted){
    event("submit",*b,b->sent,n,b->candidate,0,0,0,b->req.write?b->data.data()+b->sent:nullptr,b->req.write?b->mask.data()+b->sent:nullptr);
    children.emplace(b->candidate,Child{b,b->sent,n});++b->pending;b->sent+=n;b->candidate=0;
   }else{++submitStalls;event("submit_stall",*b,b->sent,n,b->candidate);}
   break;
  }
  checked(ss_mem_step(mem));
  sc_assert(ss_mem_clock(mem)*period==gem5::curTick());
  // The bounded set of admitted bursts owns the response storage. If all of
  // those responses wait for the FIFO, admission stops and backpressure flows up.
  ss_mem_response r;
  while(true){
   int got=ss_mem_pop(mem,&r);checked(got);if(!got)break;
   auto it=children.find(r.id);if(it==children.end())SC_REPORT_FATAL("mem_sim","unknown child ID");
   auto child=it->second;auto& b=*child.burst;
   sc_assert(r.bytes==child.bytes && r.completion_cycle*period<=gem5::curTick());
   event("complete",b,child.offset,child.bytes,r.id,r.issued_cycle,r.completion_cycle,r.status,b.req.write?nullptr:r.data);
   if(r.status>1){b.error=true;b.rsp.resp=2;for(auto& beat:b.rsp.read_beats)beat.resp=2;}
   if(!b.req.write){
    const unsigned width=1u<<b.req.address.size;
    for(unsigned j=0;j<child.bytes;++j){unsigned off=child.offset+j;unsigned beat=off/width;unsigned lane=(b.req.address.addr+off)%AXI_DATA_BYTES;b.rsp.read_beats[beat].data[lane]=r.data[j];b.data[off]=r.data[j];}
   }
   --b.pending;children.erase(it);
  }
  // Global FIFO completion is stronger than the required same-ID ordering.
  if(!bursts.empty()){
   auto& b=*bursts.front();
   if(b.sent==b.length && b.pending==0){
    if(!b.ready)b.ready=gem5::curTick()+uint64_t(hold)*period;
    if(gem5::curTick()>=b.ready && response.nb_write(b.rsp)){
     event("return",b,0,b.length,0,0,0,b.rsp.resp,b.req.write?nullptr:b.data.data());
     ++completed;if(b.error)++error_responses;bursts.pop_front();
    }else{++responseStalls;}
   }
  }
 }
}
void MemSimBackend::finish(){
 log.flush();sc_assert(bursts.empty() && children.empty() && request.num_available()==0);
 checked(ss_mem_finish(mem));
 std::ofstream f(dir+"/memsim_bridge_summary.json");
 f<<"{\"passed\":true,\"bursts\":"<<completed<<",\"errors\":"<<error_responses<<",\"children\":"<<(nextMem-1)
  <<",\"submit_stalls\":"<<submitStalls<<",\"response_stalls\":"<<responseStalls<<",\"period_fs\":"<<period<<"}\n";
}
