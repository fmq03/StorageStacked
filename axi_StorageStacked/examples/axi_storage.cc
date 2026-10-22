#include "aou_backend.hh"
#include "axi_monitor.hh"
#include <boost/property_tree/json_parser.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using namespace sc_core;
using namespace storage_axi;
using boost::property_tree::ptree;

struct Transaction {
    bool write;
    uint64_t address;
    unsigned id, size, beats;
    std::vector<std::string> data;
    std::vector<uint32_t> masks;
};

class FileMaster : public sc_module {
 public:
    sc_in<bool> clk{"clk"}, resetn{"resetn"};
    MasterPorts axi;
    bool done=false;
    SC_HAS_PROCESS(FileMaster);
    FileMaster(sc_module_name n, std::vector<Transaction> input,
               const std::string& directory, unsigned stalls)
      : sc_module(n), input(std::move(input)), output(directory+"/responses.json"), stalls(stalls) {
        if (!output) throw std::runtime_error("cannot open responses.json");
        SC_THREAD(run);
    }
 private:
    std::vector<Transaction> input;
    std::ofstream output;
    unsigned stalls;
    void edge() { wait(clk.posedge_event()); }
    void writeBeat(const Transaction& t, unsigned beat) {
        Data value=0;
        unsigned lane=(t.address+(uint64_t(beat)<<t.size))%DataBytes;
        for (unsigned j=0;j<(1u<<t.size);++j)
            value.range(8*(lane+j)+7,8*(lane+j))=std::stoul(t.data[beat].substr(2*j,2),nullptr,16);
        axi.wdata=value; axi.wstrb=uint32_t(t.masks[beat]<<lane);
        axi.wlast=beat+1==t.beats;
    }
    void run() {
#define CLEAR(T, field) axi.field=0;
        AXI_M2S(CLEAR)
#undef CLEAR
        while (!resetn.read()) edge();
        output << "[\n";
        bool first=true;
        for (const auto& t:input) {
            if (!first) output << ",\n";
            first=false;
            uint64_t begin=sc_time_stamp().value();
            unsigned response=0;
            std::vector<std::string> data;
            if (t.write) {
                axi.awaddr=t.address; axi.awid=t.id; axi.awlen=t.beats-1;
                axi.awsize=t.size; axi.awburst=1; axi.awvalid=true;
                writeBeat(t,0); axi.wvalid=true;
                bool address=false; unsigned beat=0;
                while (!address || beat<t.beats) {
                    edge();
                    if (!address && axi.awready.read()) {address=true;axi.awvalid=false;}
                    if (beat<t.beats && axi.wready.read()) {
                        ++beat;
                        if (beat==t.beats) axi.wvalid=false;
                        else writeBeat(t,beat);
                    }
                }
                // Hold READY low after VALID to exercise response stability.
                do {edge();} while(!axi.bvalid.read());
                for(unsigned j=0;j<stalls;++j) edge();
                axi.bready=true; edge();
                if (!axi.bvalid.read() || axi.bid.read()!=t.id)
                    throw std::runtime_error("unexpected AXI B response");
                response=axi.bresp.read().to_uint(); axi.bready=false;
            } else {
                axi.araddr=t.address; axi.arid=t.id; axi.arlen=t.beats-1;
                axi.arsize=t.size; axi.arburst=1; axi.arvalid=true;
                do {edge();} while(!axi.arready.read());
                axi.arvalid=false;
                for(unsigned beat=0;beat<t.beats;++beat) {
                    do {edge();} while(!axi.rvalid.read());
                    for(unsigned j=0;j<stalls;++j) edge();
                    axi.rready=true; edge();
                    if (!axi.rvalid.read() || axi.rid.read()!=t.id || axi.rlast.read()!=(beat+1==t.beats))
                        throw std::runtime_error("unexpected AXI R response");
                    response |= axi.rresp.read().to_uint();
                    std::ostringstream bytes;
                    unsigned lane=(t.address+(uint64_t(beat)<<t.size))%DataBytes;
                    Data value=axi.rdata.read();
                    for(unsigned j=0;j<(1u<<t.size);++j)
                        bytes<<std::hex<<std::setfill('0')<<std::setw(2)<<value.range(8*(lane+j)+7,8*(lane+j)).to_uint();
                    data.push_back(bytes.str()); axi.rready=false;
                }
            }
            output << "{\"command\":\""<<(t.write?"write":"read")<<"\",\"id\":"<<t.id
                   <<",\"address\":"<<t.address<<",\"response\":"<<response
                   <<",\"begin_tick_fs\":"<<begin<<",\"end_tick_fs\":"<<sc_time_stamp().value()<<",\"data\":[";
            for(unsigned j=0;j<data.size();++j) {if(j) output<<',';output<<'"'<<data[j]<<'"';}
            output<<"]}";
            edge();
        }
        output<<"\n]\n";output.flush();done=true;
    }
};

int sc_main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"usage: axi_storage normalized-input.json output-directory\n";return 2;}
    try {
        sc_set_time_resolution(1,SC_FS);
        ptree input; boost::property_tree::read_json(argv[1],input);
        StorageConfig p;
        p.trace_dir=argv[2];p.base=input.get<uint64_t>("config.base");p.size=input.get<uint64_t>("config.size");
        p.planes=input.get<unsigned>("config.planes");p.replay=input.get<bool>("config.replay");
        p.memsim_standard=input.get<std::string>("config.standard");
        p.memsim_slots=input.get<unsigned>("config.slots");p.memsim_channels=input.get<unsigned>("config.channels");
        p.memsim_scale=input.get<unsigned>("config.scale");p.memsim_queue=input.get<unsigned>("config.queue");
        p.memsim_response_hold=input.get<unsigned>("config.response_hold");
        uint64_t period=input.get<uint64_t>("config.period_fs"), limit=input.get<uint64_t>("config.max_ticks");
        std::vector<Transaction> transactions;
        for(const auto& node:input.get_child("transactions")) {
            const auto& t=node.second;
            Transaction v{t.get<std::string>("command")=="write",t.get<uint64_t>("address"),
                t.get<unsigned>("id"),t.get<unsigned>("size"),t.get<unsigned>("beats"),{}, {}};
            if(v.write) {
                for(const auto& d:t.get_child("data")) v.data.push_back(d.second.get_value<std::string>());
                for(const auto& m:t.get_child("strobe")) v.masks.push_back(m.second.get_value<uint32_t>());
            }
            transactions.push_back(std::move(v));
        }
        Signals wires;
        sc_clock clock("aclk",sc_time::from_value(period));sc_signal<bool> resetn("resetn");
        AouBackend memory("storage",p);
        FileMaster master("input",std::move(transactions),p.trace_dir,input.get<unsigned>("config.response_stall_cycles"));
        AxiMonitor monitor("monitor",wires,p.trace_dir);
        memory.clk(clock);memory.resetn(resetn);memory.axi.bind(wires);
        master.clk(clock);master.resetn(resetn);master.axi.bind(wires);
        monitor.clk(clock);monitor.resetn(resetn);
        auto* vcd=sc_create_vcd_trace_file((p.trace_dir+"/axi_wave").c_str());
        vcd->set_time_unit(1,SC_FS);sc_trace(vcd,clock,"ACLK");sc_trace(vcd,resetn,"ARESETn");
        wires.trace(vcd);memory.trace(vcd);
        resetn=false;
        sc_start(sc_time::from_value(period*3+period/2));
        while(!memory.ready() && sc_time_stamp().value()<limit) sc_start(sc_time::from_value(period));
        if(!memory.ready()) throw std::runtime_error("link training timeout");
        resetn=true;
        while(!master.done && sc_time_stamp().value()<limit) sc_start(sc_time::from_value(period));
        if(!master.done) throw std::runtime_error("AXI transaction watchdog timeout");
        sc_start(sc_time::from_value(period*2));
        memory.finish(p.trace_dir);monitor.finish(period);sc_close_vcd_trace_file(vcd);
        std::cout<<"AXI input/output completed at "<<sc_time_stamp()<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
