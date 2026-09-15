#include "vortex_axi.h"
#include "logic_die.h"
#include "ucie_link.h"
#include "aou_target.h"
#include <filesystem>
#include <iomanip>
#include <sstream>
struct Query {uint32_t current,mask,count;std::vector<uint32_t> indices;std::vector<uint8_t> bytes;};
struct Case {
    uint32_t chunks,lg,k;std::vector<Query> queries;std::vector<uint8_t> keys;
    static uint32_t word(std::istream& f){uint32_t v;f.read((char*)&v,4);if(!f)throw std::runtime_error("short fixture");return v;}
    explicit Case(const std::string& path) {
        std::ifstream f(path,std::ios::binary);
        if(word(f)!=0x314d444c)throw std::runtime_error("bad fixture");
        chunks=word(f);lg=word(f);k=word(f);queries.resize(word(f));
        for(auto&q:queries){q.current=word(f);q.mask=word(f);q.count=word(f);
            for(unsigned i=0;i<q.count;i++)q.indices.push_back(word(f));
            q.bytes.resize(256);f.read((char*)q.bytes.data(),256);}
        keys.resize(chunks*(1u<<lg)*256);f.read((char*)keys.data(),keys.size());
        if(!f)throw std::runtime_error("short key payload");
    }
};
struct System:sc_module {
    Case testcase;
    std::string dir,kernel;
    bool baseline;
    sc_clock clk{"axi_clk",2,SC_NS},gclk{"logic_die_clk",4,SC_NS};
    sc_signal<bool> rst{"reset_n"};
    sc_signal<unsigned> state{"link_state"};
    AxiBus bus;
    sc_signal<FlitTransfer> tx,rx;
    sc_signal<bool> tx_ready,rx_ready;
    sc_signal<sc_biguint<256>> wdata,rdata;
    sc_signal<sc_biguint<32>> wstrb;
    static Config link_config() {
        auto c=make_aou_ucie_config();
        c.awgn_sigma=c.jitter_sigma_ui=c.isi_h1=c.isi_h2=0;
        c.lane_skew_max_ui=0;c.extra_flit_error_rate=0;
        return c;
    }
    Config cfg=link_config();
    Stats stats;
    sc_fifo<FdiFlit> soc_tx{"soc_tx",4},soc_rx{"soc_rx",4},mem_tx{"mem_tx",4},mem_rx{"mem_rx",4};
    sc_fifo<SimpleMemRequest> requests{"requests",4};
    sc_fifo<SimpleMemResponse> responses{"responses",4};
    VortexAxi source;
    Axi2Flit bridge;
    UcieAouAdapter adapter;
    UcieLink link;
    AouTarget target;
    LogicDie die;
    std::ofstream forward,reverse;
    bool passed=false;
    SC_HAS_PROCESS(System);
    System(sc_module_name n,const std::string& fixture,const std::string& bin,const std::string& output,bool base)
      :sc_module(n),testcase(fixture),dir(output),kernel(bin),baseline(base),
       source("vortex",bus,dir),bridge("axi2flit",1),adapter("adapter",cfg),
       link("ucie",cfg,&stats,sc_time(cfg.ui_fs(),SC_FS)),target("target",cfg,1),
       die("logic_die",dir),forward(dir+"/flits_forward.csv"),reverse(dir+"/flits_reverse.csv") {
        source.clk(clk);source.rst_n(rst);
        bridge.clk(clk);bridge.rst_n(rst);bus.bind(bridge);
        bridge.flit_out(tx);bridge.flit_ready(tx_ready);bridge.flit_in(rx);bridge.flit_in_ready(rx_ready);
        adapter.clk(clk);adapter.rst_n(rst);adapter.link_state(state);
        adapter.tx(tx);adapter.tx_ready(tx_ready);adapter.rx(rx);adapter.rx_ready(rx_ready);
        adapter.fifo_tx(soc_tx);adapter.fifo_rx(soc_rx);
        link.soc_tx_in(soc_tx);link.soc_rx_out(soc_rx);link.mem_rx_out(mem_rx);link.mem_tx_in(mem_tx);link.link_state(state);
        target.clk(clk);target.rst_n(rst);target.link_rx(mem_rx);target.link_tx(mem_tx);
        target.mem_req(requests);target.mem_rsp(responses);
        die.clk(gclk);die.rst_n(rst);die.request(requests);die.response(responses);
        auto observer=[&](std::ofstream& log) {
            log<<"time_fs,event,seq,id,replay,status,bytes_hex\n";
            return [&log](const char* event,uint64_t seq,uint64_t id,bool replay,const std::vector<uint8_t>&bytes,const char*status) {
                log<<sc_time_stamp().value()<<','<<event<<','<<seq<<','<<id<<','<<replay<<','<<status<<',';
                static const char*h="0123456789abcdef";
                for(auto b:bytes)log<<h[b>>4]<<h[b&15];
                log<<'\n';
            };
        };
        stats.forward.observer=observer(forward);stats.reverse.observer=observer(reverse);
        SC_THREAD(stimulus);
        SC_METHOD(wave_data);sensitive<<bus.w<<bus.r;
    }
    void trace(sc_trace_file*f) {
        sc_trace(f,clk,"axi.clk");sc_trace(f,gclk,"logic_die.clk");sc_trace(f,rst,"reset_n");
        sc_trace(f,state,"ucie.state");
        sc_trace(f,bus.av,"axi.awvalid");sc_trace(f,bus.ar,"axi.awready");
        sc_trace(f,bus.wv,"axi.wvalid");sc_trace(f,bus.wr,"axi.wready");
        sc_trace(f,bus.arv,"axi.arvalid");sc_trace(f,bus.arr,"axi.arready");
        sc_trace(f,bus.bv,"axi.bvalid");sc_trace(f,bus.br,"axi.bready");
        sc_trace(f,bus.rv,"axi.rvalid");sc_trace(f,bus.rr,"axi.rready");
        auto address=[&](const AxChannel&a,const std::string&n) {
            sc_trace(f,a.id,n+"id");sc_trace(f,a.addr,n+"addr");sc_trace(f,a.len,n+"len");sc_trace(f,a.size,n+"size");
        };
        address(bus.aw.read(),"axi.aw");address(bus.ra.read(),"axi.ar");
        sc_trace(f,wdata,"axi.wdata");sc_trace(f,wstrb,"axi.wstrb");sc_trace(f,bus.w.read().last,"axi.wlast");
        sc_trace(f,bus.b.read().id,"axi.bid");sc_trace(f,bus.b.read().resp,"axi.bresp");
        sc_trace(f,rdata,"axi.rdata");sc_trace(f,bus.r.read().id,"axi.rid");sc_trace(f,bus.r.read().resp,"axi.rresp");sc_trace(f,bus.r.read().last,"axi.rlast");
        sc_trace(f,die.gate_busy,"logic_die.busy");sc_trace(f,die.gate_done,"logic_die.done");
        sc_trace(f,die.gate_error,"logic_die.error");sc_trace(f,die.gate_state,"logic_die.state");sc_trace(f,die.gate_mask,"logic_die.mask");
    }
    void wave_data(){
        sc_biguint<256>w=0,r=0;sc_biguint<32>s=0;
        for(unsigned i=0;i<32;++i){w.range(i*8+7,i*8)=bus.w.read().data[i];r.range(i*8+7,i*8)=bus.r.read().data[i];s[i]=bus.w.read().strb[i]!=0;}
        wdata=w;rdata=r;wstrb=s;
    }
    void tick(){wait(clk.negedge_event());}
    void write(uint32_t addr,const std::vector<uint8_t>&data) {
        for(size_t off=0;off<data.size();off+=64) {
            ss::VortexRequest r{};r.address=addr+off;r.write=true;r.size=64;
            for(unsigned i=0;i<64&&off+i<data.size();++i){r.data[i]=data[off+i];r.byteen|=1ull<<i;}
            bool done=false;while(!source.enqueue(r,[&](const auto&){done=true;}))tick();
            while(!done)tick();
        }
    }
    void stimulus() {
        try {
            rst=0;for(int i=0;i<6;i++)tick();rst=1;
            std::ifstream f(kernel,std::ios::binary);
            if(!f)throw std::runtime_error("kernel binary missing");
            std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)),{});
            if(code.size()>4096)throw std::runtime_error("kernel overlaps query data");
            write(0x80000000,code);write(0x80010000,testcase.keys);
            std::vector<uint8_t> qbytes;
            for(auto&q:testcase.queries)qbytes.insert(qbytes.end(),q.bytes.begin(),q.bytes.end());
            write(0x80001000,qbytes);
            std::vector<uint8_t> meta(1024,0);
            auto word=[&](unsigned off,uint32_t value){for(unsigned i=0;i<4;i++)meta[off+i]=value>>(8*i);};
            word(4,testcase.chunks);word(8,testcase.lg);word(12,testcase.k);word(16,testcase.queries.size());
            word(20,baseline);word(24,0x80010000);word(28,0x80001000);
            for(unsigned j=0;j<testcase.queries.size();++j){word(256+j*8,testcase.queries[j].current);word(260+j*8,testcase.queries[j].mask);}
            write(LogicDie::MAILBOX,meta);
            auto start=sc_time_stamp().value();auto tx0=stats.forward.tx_new_flits,rx0=stats.reverse.tx_new_flits;
            auto host0=die.host_bytes,dma0=die.dma_bytes;
            source.start(0x80000000);
            for(uint64_t n=0;n<100000000;n++){
                if(source.finished&&source.idle()&&die.idle()&&target.idle())break;
                tick();
                if(n==99999999)throw std::runtime_error("VORTEX execution timeout");
            }
            auto elapsed=sc_time_stamp().value()-start;
            if(die.mailbox_word(32)!=0x600dd00d) {
                std::ostringstream s;s<<"VORTEX kernel reported failure 0x"<<std::hex<<die.mailbox_word(32);
                throw std::runtime_error(s.str());
            }
            if(die.mailbox_word(36)!=testcase.queries.size())throw std::runtime_error("incomplete VORTEX query count");
            for(unsigned j=0;j<testcase.queries.size();++j)
                if(die.mailbox_word(512+j*16)!=testcase.queries[j].mask||die.mailbox_word(516+j*16)!=testcase.queries[j].count)
                    throw std::runtime_error("independent output readback mismatch");
            for(int n=0;n<100;++n)tick();
            die.finish();
            std::ofstream out(dir+"/summary.json");
            out<<"{\"passed\":true,\"mode\":\""<<(baseline?"vortex_software":"logic_die")
               <<"\",\"chunks\":"<<testcase.chunks<<",\"chunk_log2\":"<<testcase.lg
               <<",\"top_k\":"<<testcase.k<<",\"queries\":"<<testcase.queries.size()
               <<",\"elapsed_fs\":"<<elapsed<<",\"vortex_cycles\":"<<source.gpu_cycles
               <<",\"vortex_requests\":"<<source.requests<<",\"vortex_read_bytes\":"<<source.read_bytes
               <<",\"vortex_write_bytes\":"<<source.write_bytes<<",\"local_dma_bytes\":"<<die.dma_bytes-dma0
               <<",\"host_dram_bytes\":"<<die.host_bytes-host0
               <<",\"forward_flits\":"<<stats.forward.tx_new_flits-tx0<<",\"reverse_flits\":"<<stats.reverse.tx_new_flits-rx0
               <<",\"response_stalls\":"<<source.r_stalls+source.b_stalls<<",\"prepare_cycles\":"<<die.mailbox_word(48)
               <<",\"query_results\":[";
            for(unsigned j=0;j<testcase.queries.size();++j) {
                if(j)out<<',';
                out<<"{\"mask\":"<<die.mailbox_word(512+j*16)<<",\"cycles\":"<<die.mailbox_word(520+j*16)
                   <<",\"dma_beats\":"<<die.mailbox_word(524+j*16)<<"}";
            }
            out<<"]}\n";passed=true;sc_stop();
        }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';sc_stop();}
    }
};
int sc_main(int argc,char**argv) {
    if(argc<4){std::cerr<<"usage: ss_sim fixture.bin kernel.bin output [software]\n";return 2;}
    try {
        sc_set_time_resolution(1,SC_FS);std::filesystem::create_directories(argv[3]);
        System s("system",argv[1],argv[2],argv[3],argc>4&&std::string(argv[4])=="software");
        auto*trace=sc_create_vcd_trace_file((std::string(argv[3])+"/axi_logic_die").c_str());
        trace->set_time_unit(1,SC_FS);s.trace(trace);sc_start();sc_close_vcd_trace_file(trace);
        return s.passed?0:1;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
