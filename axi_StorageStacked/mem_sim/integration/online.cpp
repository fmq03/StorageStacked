#include "online.h"
#include "hbm_sim/config/model.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/validation/trace.hpp"
#include "hbm_sim/validation/validator.hpp"
#include "hbm_sim/validation/dfi.hpp"
#include "hbm_sim/stats/result.hpp"
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
using namespace hbm_sim;
static thread_local std::string error;
struct ss_mem {
 DramSpec spec;
 std::unique_ptr<MemorySystem> system;
 std::string dir;
 uint64_t size=0, period, submitted=0, returned=0, stalls=0;
 std::map<uint64_t,unsigned> live;
 std::set<uint64_t> initialized;
};
extern "C" const char* ss_mem_error() { return error.c_str(); }
extern "C" ss_mem* ss_mem_create(const char* standard, unsigned channels, unsigned scale,
 unsigned depth, uint64_t size, const char* outdir) {
 try {
  if (!channels || !scale || !depth || !size) throw std::invalid_argument("zero configuration value");
  auto h=std::make_unique<ss_mem>(); h->dir=outdir; h->size=size;
  h->spec=config::build_model(standard, {{"channels",std::to_string(channels)}});
  if(size>h->spec.addressable_capacity_bytes()) throw std::invalid_argument("window exceeds native DRAM capacity; increase channels");
  // Scale the host-to-model time mapping, keeping the native speed/tCK valid.
  double fs=h->spec.timing.tCK_ps*1000*scale/h->spec.tick_multiplier;
  h->period=std::llround(fs);
  if (!h->period || std::abs(fs-h->period)>1e-6 || h->spec.transaction_bytes()>64)
   throw std::invalid_argument("period must be integral fs; transaction must fit 64B");
  MemorySystemOptions o;
  o.stack_ingress_buffer_size=depth; o.stack_dispatch_width=1;
  o.host_response_queue_capacity=depth;
  o.response_delivery_mode=ResponseDeliveryMode::HostOnly;
  o.controller.read_buffer_size=depth; o.controller.write_buffer_size=depth;
  o.controller.response_queue_capacity=depth;
  o.controller.phy.mode=MemPhyMode::Behavioral;
  o.controller.phy.command_fifo_depth=depth;
  o.controller.phy.read_fifo_depth=depth; o.controller.phy.write_fifo_depth=depth;
  o.controller.phy.training_cycles=8;
  h->system=std::make_unique<MemorySystem>(h->spec,o);
  // Explicit power-on contents, in the model's only authoritative image.
  auto image=h->system->stack_memory_images().at(0);
  for(uint64_t a=0;size<=1024*1024 && a<size;) {
   auto n=std::min<uint64_t>(h->spec.transaction_bytes(),size-a);
   image->write(a,ByteVector(n,0)); h->initialized.insert(a); a+=n;
  }
  std::ofstream cfg(h->dir+"/memsim_config.json");
  cfg<<"{\"standard\":\""<<standard<<"\",\"channels\":"<<channels
     <<",\"timing_scale\":"<<scale<<",\"period_fs\":"<<h->period
     <<",\"transaction_bytes\":"<<h->spec.transaction_bytes()<<",\"queue_depth\":"<<depth
     <<",\"window_bytes\":"<<size<<",\"phy\":\"behavioral\",\"tick_multiplier\":"<<h->spec.tick_multiplier
     <<",\"tCK_ps\":"<<h->spec.timing.tCK_ps<<",\"effective_tCK_ps\":"<<h->spec.timing.tCK_ps*scale
     <<",\"density_gbit\":"<<h->spec.density_gb<<",\"data_bus_bits\":"<<h->spec.data_bus_bits
     <<",\"data_rate_mbps\":"<<h->spec.data_rate_mbps<<",\"capacity_bytes\":"<<h->spec.addressable_capacity_bytes()
     <<",\"provisional_timing_entries\":"<<h->spec.timing_table.provisional_count()<<"}\n";
  return h.release();
 } catch(const std::exception& e) { error=e.what();return nullptr; }
}
extern "C" void ss_mem_destroy(ss_mem* h) { delete h; }
extern "C" uint64_t ss_mem_period_fs(ss_mem* h) { return h->period; }
extern "C" uint64_t ss_mem_clock(ss_mem* h) { return h->system->clock(); }
extern "C" unsigned ss_mem_transaction_bytes(ss_mem* h) {return h->spec.transaction_bytes();}
extern "C" int ss_mem_submit(ss_mem* h,uint64_t id,uint64_t addr,unsigned bytes,int write,const uint8_t* data,const uint8_t* mask) {
 try {
  const unsigned granule=h->spec.transaction_bytes();
  if(!bytes || addr>=h->size || bytes>h->size-addr || bytes>granule || addr/granule!=(addr+bytes-1)/granule || h->live.count(id))
   throw std::invalid_argument("invalid/duplicate online transaction");
  const uint64_t line=addr-addr%granule;
  if(h->initialized.insert(line).second) {
   auto image=h->system->stack_memory_images().at(0);
   image->write(line,ByteVector(std::min<uint64_t>(granule,h->size-line),0));
  }
  Request r; r.id=r.host_request_id=id; r.address=addr; r.transfer_bytes=bytes;
  r.type=write?RequestType::Write:RequestType::Read;
  r.decoded=AddressMapper(h->spec).decode(addr); r.inject_cycle=h->system->clock();
  if(write){r.payload.assign(data,data+bytes);r.byte_mask.assign(mask,mask+bytes);r.has_payload=r.has_byte_mask=true;}
  if(!h->system->try_submit(r)){++h->stalls;return 0;}
  h->live.emplace(id,bytes);++h->submitted;return 1;
 }catch(const std::exception& e){error=e.what();return -1;}
}
extern "C" int ss_mem_step(ss_mem* h){try{h->system->step();return 0;}catch(const std::exception& e){error=e.what();return -1;}}
extern "C" int ss_mem_pop(ss_mem* h,ss_mem_response* out){
 try {
  if(!h->system->has_response())return 0;
  auto r=h->system->pop_response();
  auto it=h->live.find(r.host_request_id);
  if(it==h->live.end())throw std::runtime_error("unknown memory response");
  *out={};out->id=r.host_request_id;out->bytes=it->second;
  out->issued_cycle=r.first_issued_cycle;out->completion_cycle=r.completion_cycle;
  out->status=static_cast<unsigned>(r.status);
  if(r.type==RequestType::Read){
   if(r.data.size()!=out->bytes)throw std::runtime_error("read response size mismatch");
   std::copy(r.data.begin(),r.data.end(),out->data);
  }
  h->live.erase(it);++h->returned;return 1;
 }catch(const std::exception& e){error=e.what();return -1;}
}
extern "C" int ss_mem_finish(ss_mem* h){
 try {
  h->system->finish();
  const auto& commands=h->system->issued_commands();
  auto validation=validate_command_trace(h->spec,commands);
  auto dfi=build_dfi_trace(h->spec,commands);
  auto dv=validate_dfi_trace(h->spec,commands,dfi);
  write_command_trace_csv(h->dir+"/memsim_commands.csv",commands);
  write_dfi_trace_csv(h->dir+"/memsim_dfi.csv",dfi);
  write_dfi_signal_trace_csv(h->dir+"/memsim_dfi_signals.csv",dfi);
  std::ofstream stats(h->dir+"/memsim_stats.txt");
  print_diagnostics(stats,collect_stats(h->system->stats()));
  for(const auto& e:validation.errors)stats<<"COMMAND ERROR: "<<e<<'\n';
  for(const auto& e:dv.errors)stats<<"DFI ERROR: "<<e<<'\n';
  h->system->stack_memory_images()[0]->dump_csv(h->dir+"/memsim_image.csv");
  std::ofstream summary(h->dir+"/memsim_core.json");
  bool ok=h->live.empty() && h->system->responses_drained() && validation.ok() && dv.ok();
  summary<<"{\"passed\":"<<(ok?"true":"false")<<",\"submitted\":"<<h->submitted<<",\"returned\":"<<h->returned
   <<",\"submit_stalls\":"<<h->stalls<<",\"cycles\":"<<h->system->clock()<<",\"commands\":"<<commands.size()
   <<",\"dfi_events\":"<<dfi.size()<<",\"command_errors\":"<<validation.errors.size()<<",\"dfi_errors\":"<<dv.errors.size()<<"}\n";
  if(!ok)throw std::runtime_error("mem_sim final validation failed; inspect memsim_stats.txt");
  return 0;
 }catch(const std::exception& e){error=e.what();return -1;}
}
