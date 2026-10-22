#include "coralnpu_native.h"
#include "hw_sim/core_mini_axi_wrapper.h"
#include "tests/verilator_sim/elf.h"
#include <fstream>
#include <iterator>
#include <cstring>
#include <map>
#include <cstdio>
#ifdef CORALNPU_PARAMETERS_FILE
#include CORALNPU_STRING(CORALNPU_PARAMETERS_FILE)
#else
#include "hdl/chisel/src/coralnpu/VRvvCoreMiniAxi_parameters.h"
#endif
#include <stdexcept>

namespace {
constexpr uint32_t itcmBytes=KP_itcmSizeKBytes*1024;
constexpr uint32_t dtcmBytes=KP_dtcmSizeKBytes*1024;
constexpr bool smallTcm=KP_itcmSizeKBytes==8 && KP_dtcmSizeKBytes==32;
constexpr uint32_t dtcmBase=smallTcm?0x10000:0x100000;
constexpr uint32_t csrBase=smallTcm?0x30000:0x200000;
}
struct coralnpu_device {
    VerilatedContext context;
    CoreMiniAxiWrapper wrapper{&context};
    coralnpu_request_fn request;
    coralnpu_ready_fn ready;
    void* owner;
    uint32_t entry=0;
    unsigned stage=3;
    uint64_t cycles=0, sequence=0;
    std::shared_ptr<bool> start;
    std::map<uint64_t, std::pair<uint8_t,bool>> pending;
    void access(const AxiAddr& a, const AxiWData* w) {
        if (a.addr_bits_len || a.addr_bits_size>4 || a.addr_bits_burst!=1 || a.addr_bits_lock || (w && !w->write_data_bits_last))
            throw std::runtime_error("Unsupported CoralNPU AXI burst; expected one unlocked INCR beat");
        uint32_t address=a.addr_bits_addr & ~15u;
        if (address==0xc0000000) {
            auto& mb=wrapper.mailbox();
            if(w) {
                auto* dst=reinterpret_cast<uint8_t*>(mb.message);
                const auto* src=reinterpret_cast<const uint8_t*>(&w->write_data_bits_data[0]);
                for(unsigned i=0;i<16;++i) if(w->write_data_bits_strb&(1u<<i)) dst[i]=src[i];
                AxiWResp b{}; b.write_resp_bits_id=a.addr_bits_id;wrapper.CompleteWrite(b);
            } else {
                AxiRData r{};std::memcpy(&r.read_data_bits_data[0],mb.message,16);
                r.read_data_bits_id=a.addr_bits_id;r.read_data_bits_last=1;wrapper.CompleteRead(r);
            }
            return;
        }
        if(address<0x90000000 || address>=0xc0000000)
            throw std::runtime_error("CoralNPU access outside online memory and mailbox");
        auto seq=++sequence;
        pending.emplace(seq,std::make_pair(a.addr_bits_id,w!=nullptr));
        request(owner,seq,address,a.addr_bits_id,w!=nullptr,
                w?reinterpret_cast<const uint8_t*>(&w->write_data_bits_data[0]):nullptr,
                w?w->write_data_bits_strb:0xffff);
    }
};
extern "C" {
const char* coralnpu_configuration(void) {
    static char text[384];
    std::snprintf(text,sizeof(text),
        "{\"fetch_l0_enabled\":%s,\"itcm_kib\":%d,\"dtcm_kib\":%d,\"rvv_enabled\":%s,\"float_enabled\":%s,\"itcm_base\":0,\"dtcm_base\":%u,\"csr_base\":%u}",
        KP_enableFetchL0?"true":"false",KP_itcmSizeKBytes,KP_dtcmSizeKBytes,
        KP_enableRvv?"true":"false",KP_enableFloat?"true":"false",dtcmBase,csrBase);
    return text;
}
coralnpu_handle coralnpu_create(coralnpu_request_fn request,coralnpu_ready_fn ready,void* owner) {
    if(!request || !ready) return nullptr;
    auto* d=new coralnpu_device;d->request=request;d->ready=ready;d->owner=owner;
    d->wrapper.RegisterAsyncReadCallback([d](const AxiAddr& a){d->access(a,nullptr);});
    d->wrapper.RegisterAsyncWriteCallback([d](const AxiAddr& a,const AxiWData& w){d->access(a,&w);});
    d->wrapper.RegisterRequestReady([d]{return d->ready(d->owner)!=0;});
    d->wrapper.Reset();return d;
}
void coralnpu_destroy(coralnpu_handle d){delete d;}
int coralnpu_load(coralnpu_handle d,const char* path) {
    std::ifstream f(path,std::ios::binary);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)),{});
    if(elf.size()<52 || std::memcmp(elf.data(),"\177ELF",4) || elf[4]!=1 || elf[5]!=1) return -1;
    CopyFn copy=[d](void* dest,const void* src,size_t n){
        auto address=reinterpret_cast<uintptr_t>(dest);
        // Bootstrap only local TCM, never silently initialize external memory.
        if(!((address<itcmBytes && n<=itcmBytes-address) ||
             (address>=dtcmBase && address<dtcmBase+dtcmBytes && n<=dtcmBase+dtcmBytes-address))) throw std::runtime_error("ELF segment outside local TCM");
        d->wrapper.Write(address,n,static_cast<const char*>(src));return dest;
    };
    d->entry=LoadElf(elf.data(),copy);d->stage=0;return 0;
}
int coralnpu_step(coralnpu_handle d) {
    if(d->stage<3) {
        if(d->start && *d->start){d->start.reset();++d->stage;}
        if(d->stage<3 && !d->start)
            d->start=d->wrapper.EnqueueWriteWord(d->stage==0?csrBase+4:csrBase,
                                                 d->stage==0?d->entry:(d->stage==1?1:0));
    }
    ++d->cycles;d->wrapper.Step();
    return d->stage<3 || (!d->wrapper.halted() && !d->wrapper.wfi());
}
void coralnpu_complete(coralnpu_handle d,uint64_t seq,uint8_t id,int write,const uint8_t data[16],uint8_t response) {
    auto it=d->pending.find(seq);
    if(it==d->pending.end() || it->second!=std::make_pair(id,write!=0))
        throw std::runtime_error("Unknown or duplicate native response");
    if(write){AxiWResp r{};r.write_resp_bits_id=id;r.write_resp_bits_resp=response;d->wrapper.CompleteWrite(r);}
    else {AxiRData r{};std::memcpy(&r.read_data_bits_data[0],data,16);r.read_data_bits_id=id;
          r.read_data_bits_resp=response;r.read_data_bits_last=1;d->wrapper.CompleteRead(r);}
    d->pending.erase(it);
}
uint32_t coralnpu_mailbox(coralnpu_handle d){return d->wrapper.mailbox().message[0];}
uint64_t coralnpu_cycles(coralnpu_handle d){return d->cycles;}
}
