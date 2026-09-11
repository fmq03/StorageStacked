#include "aou_backend.hh"
#include "axi2flit.h"
#include "ucie_link.h"
#include "aou_target.h"
#include "simple_burst_memory.h"
#include "memsim_backend.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"
#include <fstream>
#include <iomanip>
#include <deque>
#include <map>

namespace storage_axi {
using namespace sc_core;
static_assert(DataBytes == AXI_DATA_BYTES, "Master and AXI2Flit bus widths must match");
struct AouBackend::Fabric : sc_module {
    AouBackend& owner;
    Config cfg;
    unsigned planes;
    sc_signal<unsigned> state{"link_state"};
    sc_signal<bool> av, ar, wv, wr, arv, arr, bv, br, rv, rr;
    sc_signal<AxChannel> aw, ra;
    sc_signal<WChannel> w;
    sc_signal<BChannel> b;
    sc_signal<RChannel> r;
    sc_signal<FlitTransfer> tx, rx;
    sc_signal<bool> txr, rxr;
    Stats stats;
    sc_fifo<FdiFlit> soc_tx{"soc_tx", 8}, soc_rx{"soc_rx", 8};
    sc_fifo<FdiFlit> mem_tx{"mem_tx", 8}, mem_rx{"mem_rx", 8};
    sc_fifo<SimpleMemRequest> requests{"requests", 4};
    sc_fifo<SimpleMemResponse> responses{"responses", 4};
    Axi2Flit bridge;
    UcieAouAdapter adapter;
    UcieLink link;
    AouTarget target;
    std::unique_ptr<SimpleBurstMemory> simple;
    std::unique_ptr<MemSimBackend> memory;
    struct Cursor { uint64_t addr; unsigned size, left; };
    std::deque<Cursor> writes;
    std::map<unsigned, Cursor> reads;
    sc_event changed;
    uint64_t tx_count = 0, rx_count = 0;
    std::ofstream log, flit_log, soc_log, mem_log;
    uint64_t observation = 0;
    std::map<std::pair<std::string,uint64_t>,unsigned> sends, receives;
    sc_signal<sc_biguint<256>> wide_wdata, wide_rdata;
    sc_signal<sc_uint<32>> wide_strb;
    static Config config(bool replay) {
        auto c = make_aou_ucie_config();
        c.awgn_sigma = c.jitter_sigma_ui = c.isi_h1 = c.isi_h2 = 0;
        c.lane_skew_max_ui = 0; c.extra_flit_error_rate = replay ? 0.02 : 0;
        return c;
    }
    SC_HAS_PROCESS(Fabric);
    Fabric(sc_module_name n, AouBackend& o, const gem5::AxiDemoParams& p)
      : sc_module(n), owner(o), cfg(config(p.replay)), planes(p.planes),
        bridge("bridge", planes), adapter("adapter", cfg),
        link("link", cfg, &stats, sc_time(cfg.ui_fs(), SC_FS)),
        target("target", cfg, planes),
        log(p.trace_dir + "/aou_events.csv") {
        g_aou_verbose = false;
        flit_log.open(p.trace_dir + "/ucie_flits.csv");
        soc_log.open(p.trace_dir + "/ucie_soc.csv");
        mem_log.open(p.trace_dir + "/ucie_mem.csv");
        if (!flit_log || !soc_log || !mem_log) throw std::runtime_error("cannot open UCIe logs");
        const char* header = "record,tick_fs,time_ns,delta,endpoint,direction,event,seq,seq8,flit_id,attempt,replay,status,bytes,hex\n";
        flit_log << header; soc_log << header; mem_log << header;
        stats.forward.observer = [this](const char* e,uint64_t seq,uint64_t id,bool replay,
                                        const std::vector<uint8_t>& bytes,const char* status) {
            observe("FWD",e,seq,id,replay,bytes,status);
        };
        stats.reverse.observer = [this](const char* e,uint64_t seq,uint64_t id,bool replay,
                                        const std::vector<uint8_t>& bytes,const char* status) {
            observe("REV",e,seq,id,replay,bytes,status);
        };
        bridge.clk(o.clk); bridge.rst_n(o.resetn);
        bridge.aw_valid(av); bridge.aw_ready(ar); bridge.aw_ch(aw);
        bridge.w_valid(wv); bridge.w_ready(wr); bridge.w_ch(w);
        bridge.ar_valid(arv); bridge.ar_ready(arr); bridge.ar_ch(ra);
        bridge.b_valid(bv); bridge.b_ready(br); bridge.b_ch(b);
        bridge.r_valid(rv); bridge.r_ready(rr); bridge.r_ch(r);
        bridge.flit_out(tx); bridge.flit_ready(txr);
        bridge.flit_in(rx); bridge.flit_in_ready(rxr);
        adapter.clk(o.clk); adapter.rst_n(o.resetn); adapter.link_state(state);
        adapter.tx(tx); adapter.tx_ready(txr); adapter.rx(rx); adapter.rx_ready(rxr);
        adapter.fifo_tx(soc_tx); adapter.fifo_rx(soc_rx);
        link.soc_tx_in(soc_tx); link.soc_rx_out(soc_rx);
        link.mem_rx_out(mem_rx); link.mem_tx_in(mem_tx); link.link_state(state);
        target.clk(o.clk); target.rst_n(o.resetn);
        target.link_rx(mem_rx); target.link_tx(mem_tx);
        target.mem_req(requests); target.mem_rsp(responses);
        if (p.memory_backend == "memsim") {
            memory = std::make_unique<MemSimBackend>("memory", p.base, p.size,
                p.memsim_slots, p.memsim_channels, p.memsim_scale,
                p.memsim_queue, p.memsim_response_hold, p.trace_dir);
            memory->request(requests); memory->response(responses);
        } else if (p.memory_backend == "simple") {
            simple = std::make_unique<SimpleBurstMemory>("memory", p.base, p.size,
                sc_time::from_value(p.period * p.latency), sc_time::from_value(p.period));
            simple->request(requests); simple->response(responses);
        } else throw std::invalid_argument("unknown memory backend");
        SC_METHOD(comb);
#define SENSITIVE(T, name) sensitive << o.axi.name;
        AXI_M2S(SENSITIVE)
#undef SENSITIVE
        sensitive << o.resetn << ar << wr << arr << bv << b << rv << r << changed;
        SC_METHOD(tick); sensitive << o.clk.pos(); dont_initialize();
        log << "tick,channel,id,address,len,size,data_hex,strb_hex,last,resp\n";
    }
    void observe(const std::string& direction, const std::string& event, uint64_t seq,
                 uint64_t id, bool replay, const std::vector<uint8_t>& bytes, const char* status) {
        sc_assert(sc_time_stamp().value() == gem5::curTick());
        auto key=std::make_pair(direction,seq);
        unsigned attempt=0;
        if (event=="TX_FRAME") attempt=++sends[key];
        if (event=="RX_FRAME") attempt=++receives[key];
        if (event=="RX_FDI") attempt=receives[key];
        bool soc = (direction=="FWD") == (event.compare(0,2,"TX")==0);
        std::ostringstream row;
        const uint64_t tick=sc_time_stamp().value();
        row << ++observation << ',' << tick << ',' << tick/1000000 << '.'
            << std::setw(6) << std::setfill('0') << tick%1000000 << ',' << sc_delta_count()
            << ',' << (soc ? "SOC" : "MEM") << ',' << direction << ',' << event << ','
            << seq << ',' << (seq&255) << ',' << id << ',' << attempt << ',' << replay
            << ',' << status << ',' << bytes.size() << ',';
        for (auto byte:bytes) row << std::hex << std::setw(2) << unsigned(byte);
        row << '\n';
        flit_log << row.str(); (soc ? soc_log : mem_log) << row.str();
    }
    AxChannel address(bool read) {
        auto& a = owner.axi;
        AxChannel x;
        x.id = read ? a.arid.read().to_uint() : a.awid.read().to_uint();
        x.addr = read ? a.araddr.read().to_uint64() : a.awaddr.read().to_uint64();
        x.len = read ? a.arlen.read().to_uint() : a.awlen.read().to_uint();
        x.size = read ? a.arsize.read().to_uint() : a.awsize.read().to_uint();
        x.burst = read ? a.arburst.read().to_uint() : a.awburst.read().to_uint();
        // Explicit test routing policy; gem5 requestor/stream metadata remain in TLM CSV.
        x.qos = x.id % planes;
        return x;
    }
    void comb() {
        auto& a = owner.axi;
        const bool on = owner.resetn.read();
        aw.write(address(false)); ra.write(address(true));
        av = on && a.awvalid.read() && writes.size() < 1023;
        a.awready = on && ar.read() && writes.size() < 1023;
        arv = on && a.arvalid.read(); a.arready = on && arr.read();
        WChannel wd;
        if (!writes.empty()) {
            // Both sides now carry the same 32 byte lanes, including narrow
            // transfers. No narrowing, widening or address-dependent shifting.
            for (unsigned j = 0; j < DataBytes; ++j) {
                wd.data[j] = a.wdata.read().range(8*j+7, 8*j).to_uint();
                wd.strb[j] = (a.wstrb.read().to_uint() >> j) & 1;
            }
        }
        wd.last = a.wlast.read(); w.write(wd);
        wv = on && !writes.empty() && a.wvalid.read();
        a.wready = on && !writes.empty() && wr.read();
        br = on && a.bready.read(); a.bvalid = on && bv.read();
        a.bid = b.read().id; a.bresp = b.read().resp;
        rr = on && a.rready.read(); a.rvalid = on && rv.read();
        a.rid = r.read().id; a.rresp = r.read().resp; a.rlast = r.read().last;
        Data rd = 0;
        for (unsigned j = 0; j < DataBytes; ++j)
            rd.range(8*j+7, 8*j) = r.read().data[j];
        a.rdata = rd;
        sc_biguint<256> wd_wave = 0, rd_wave = 0;
        sc_uint<32> st = 0;
        for (unsigned j = 0; j < 32; ++j) {
            wd_wave.range(8*j+7,8*j) = wd.data[j];
            rd_wave.range(8*j+7,8*j) = r.read().data[j]; st[j] = wd.strb[j] != 0;
        }
        wide_wdata = wd_wave; wide_rdata = rd_wave; wide_strb = st;
    }
    void row(const char* ch, unsigned id, uint64_t addr, unsigned len, unsigned size,
             const uint8_t* data, const uint8_t* strb, bool last, unsigned resp) {
        static const char hex[] = "0123456789abcdef";
        log << sc_time_stamp().value() << ',' << ch << ',' << id << ',' << addr
            << ',' << len << ',' << size << ',';
        if (data) for (int j=31;j>=0;--j) log << hex[data[j]>>4] << hex[data[j]&15];
        log << ',';
        if (strb) { uint32_t mask=0; for (unsigned j=0;j<32;++j) mask |= uint32_t(!!strb[j])<<j;
            log << std::hex << mask << std::dec; }
        log << ',' << last << ',' << resp << '\n';
    }
    void tick() {
        sc_assert(sc_time_stamp().value() == gem5::curTick());
        if (!owner.resetn.read()) { writes.clear(); reads.clear(); changed.notify(SC_ZERO_TIME); return; }
        if (av && ar) {
            const auto& x=aw.read(); sc_assert(x.id && x.id <= 1023);
            writes.push_back({x.addr,x.size,unsigned(x.len)+1});
            row("AW",x.id,x.addr,x.len,x.size,nullptr,nullptr,false,0);
        }
        if (wv && wr) {
            sc_assert(!writes.empty()); auto& c=writes.front();
            sc_assert(w.read().last == (c.left==1));
            row("W",0,c.addr,0,c.size,w.read().data,w.read().strb,w.read().last,0);
            c.addr += 1u<<c.size; if (!--c.left) writes.pop_front();
        }
        if (arv && arr) {
            const auto& x=ra.read(); sc_assert(x.id && x.id <= 1023 && !reads.count(x.id));
            reads.emplace(x.id,Cursor{x.addr,x.size,unsigned(x.len)+1});
            row("AR",x.id,x.addr,x.len,x.size,nullptr,nullptr,false,0);
        }
        if (bv && br) row("B",b.read().id,0,0,0,nullptr,nullptr,false,b.read().resp);
        if (rv && rr) {
            auto it=reads.find(r.read().id); sc_assert(it!=reads.end()); auto& c=it->second;
            sc_assert(r.read().last == (c.left==1));
            row("R",r.read().id,c.addr,0,c.size,r.read().data,nullptr,r.read().last,r.read().resp);
            c.addr += 1u<<c.size; if (!--c.left) reads.erase(it);
        }
        if (tx.read().valid && txr) { ++tx_count; row("TX",0,0,0,0,nullptr,nullptr,false,0); }
        if (rx.read().valid && rxr) { ++rx_count; row("RX",0,0,0,0,nullptr,nullptr,false,0); }
        changed.notify(SC_ZERO_TIME);
    }
};
AouBackend::AouBackend(sc_module_name n, const gem5::AxiDemoParams& p)
    : sc_module(n), fabric(std::make_unique<Fabric>("fabric", *this, p)) {}
AouBackend::~AouBackend() = default;
bool AouBackend::ready() const {
    auto s=static_cast<LinkState>(fabric->state.read());
    if (s == LinkState::Failed) SC_REPORT_FATAL("AoU", "link training failed");
    return s==LinkState::Active || s==LinkState::Degraded;
}
unsigned AouBackend::access(tlm::tlm_generic_payload& gp) {
    // Functional access must never silently access the old, disconnected RAM.
    gp.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE); return 0;
}
void AouBackend::trace(sc_trace_file* f) {
    auto& s=*fabric;
    sc_trace(f,s.state,"aou.link_state");
#define TRACE(n) sc_trace(f,s.n,"aou." #n)
    TRACE(av); TRACE(ar); TRACE(wv); TRACE(wr); TRACE(arv); TRACE(arr);
    TRACE(bv); TRACE(br); TRACE(rv); TRACE(rr); TRACE(txr); TRACE(rxr);
    TRACE(wide_wdata); TRACE(wide_rdata); TRACE(wide_strb);
#undef TRACE
    sc_trace(f,s.aw,"aou.aw"); sc_trace(f,s.ra,"aou.ar");
    // Canonical signal names: embedded struct valid/ready are not handshakes.
    sc_trace(f,s.av,"axi256.awvalid"); sc_trace(f,s.ar,"axi256.awready");
    sc_trace(f,s.wv,"axi256.wvalid"); sc_trace(f,s.wr,"axi256.wready");
    sc_trace(f,s.arv,"axi256.arvalid"); sc_trace(f,s.arr,"axi256.arready");
    sc_trace(f,s.bv,"axi256.bvalid"); sc_trace(f,s.br,"axi256.bready");
    sc_trace(f,s.rv,"axi256.rvalid"); sc_trace(f,s.rr,"axi256.rready");
    auto addr = [&](const AxChannel& x,const std::string& prefix) {
        sc_trace(f,x.id,prefix+"id"); sc_trace(f,x.addr,prefix+"addr");
        sc_trace(f,x.len,prefix+"len"); sc_trace(f,x.size,prefix+"size");
        sc_trace(f,x.burst,prefix+"burst"); sc_trace(f,x.lock,prefix+"lock");
        sc_trace(f,x.cache,prefix+"cache"); sc_trace(f,x.prot,prefix+"prot");
        sc_trace(f,x.qos,prefix+"qos"); sc_trace(f,x.user,prefix+"user");
    };
    addr(s.aw.read(),"axi256.aw"); addr(s.ra.read(),"axi256.ar");
    sc_trace(f,s.wide_wdata,"axi256.wdata"); sc_trace(f,s.wide_strb,"axi256.wstrb");
    sc_trace(f,s.w.read().last,"axi256.wlast"); sc_trace(f,s.w.read().user,"axi256.wuser");
    sc_trace(f,s.b.read().id,"axi256.bid"); sc_trace(f,s.b.read().resp,"axi256.bresp");
    sc_trace(f,s.b.read().user,"axi256.buser");
    sc_trace(f,s.wide_rdata,"axi256.rdata"); sc_trace(f,s.r.read().id,"axi256.rid");
    sc_trace(f,s.r.read().last,"axi256.rlast"); sc_trace(f,s.r.read().resp,"axi256.rresp");
    sc_trace(f,s.r.read().user,"axi256.ruser");
    sc_trace(f,s.b,"aou.b"); sc_trace(f,s.r,"aou.r");
    sc_trace(f,s.tx,"aou.tx"); sc_trace(f,s.rx,"aou.rx");
}
void AouBackend::finish(const std::string& dir) {
    auto& s=*fabric; s.log.flush(); s.flit_log.flush(); s.soc_log.flush(); s.mem_log.flush();
    sc_assert(s.writes.empty() && s.reads.empty());
    sc_assert(s.bridge.order_violations()==0);
    if (s.memory) s.memory->finish();
    std::ofstream f(dir+"/aou_summary.json");
    f << "{\"width\":256,\"planes\":" << s.planes
      << ",\"memory_completed\":" << (s.memory ? s.memory->completed : s.simple->completed)
      << ",\"memory_errors\":" << (s.memory ? s.memory->error_responses : s.simple->error_responses)
      << ",\"target_reads\":" << s.target.reads << ",\"target_writes\":" << s.target.writes
      << ",\"target_read_beats\":" << s.target.read_beats
      << ",\"target_write_beats\":" << s.target.write_beats
      << ",\"tx_flits\":" << s.tx_count << ",\"rx_flits\":" << s.rx_count
      << ",\"forward_replays\":" << s.stats.forward.tx_replay_flits
      << ",\"reverse_replays\":" << s.stats.reverse.tx_replay_flits
      << ",\"crc_errors\":" << s.stats.forward.crc_fail_count+s.stats.reverse.crc_fail_count
      << ",\"order_violations\":" << s.bridge.order_violations() << "}\n";
}
}
