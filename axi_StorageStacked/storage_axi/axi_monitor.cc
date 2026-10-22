#include "axi_monitor.hh"
namespace storage_axi {
using namespace sc_core;
void AxiMonitor::channel(const std::string& n, bool valid, bool ready,
                   const std::vector<Data>& payload) {
    auto it = held.find(n);
    if (it != held.end()) {
        if (!valid || payload != it->second)
            SC_REPORT_FATAL("AXI stability", n.c_str());
    }
    if (valid && !ready) { held[n] = payload; ++stalled[n]; }
    else held.erase(n);
    if (valid && ready) ++handshakes[n];
}
void AxiMonitor::row(const char* n, uint64_t id, uint64_t a, unsigned len,
               unsigned size, Data d, unsigned strb, bool last, unsigned resp) {
    events << sc_time_stamp().value() << ',' << cycle << ',' << n << ',' << id << ','
           << a << ',' << len << ',' << size << ',' << d.to_string(sc_dt::SC_DEC, false) << ',' << strb << ','
           << last << ',' << resp << '\n';
}
void AxiMonitor::sample() {
    ++cycle;
    if (!resetn.read()) return;
    auto& w = wires;
    channel("AW", w.awvalid, w.awready, {Data(w.awid.read()), Data(w.awaddr.read()), Data(w.awlen.read()), Data(w.awsize.read()), Data(w.awburst.read())});
    channel("W", w.wvalid, w.wready, {w.wdata.read(), Data(w.wstrb.read()), Data(w.wlast.read())});
    channel("B", w.bvalid, w.bready, {Data(w.bid.read()), Data(w.bresp.read())});
    channel("AR", w.arvalid, w.arready, {Data(w.arid.read()), Data(w.araddr.read()), Data(w.arlen.read()), Data(w.arsize.read()), Data(w.arburst.read())});
    channel("R", w.rvalid, w.rready, {Data(w.rid.read()), w.rdata.read(), Data(w.rresp.read()), Data(w.rlast.read())});
    if (w.awvalid && w.awready) row("AW", w.awid.read(), w.awaddr.read(), w.awlen.read(), w.awsize.read());
    if (w.wvalid && w.wready) row("W", 0, 0, 0, 0, w.wdata.read(), w.wstrb.read(), w.wlast.read());
    if (w.bvalid && w.bready) row("B", w.bid.read(), 0, 0, 0, 0, 0, false, w.bresp.read());
    if (w.arvalid && w.arready) row("AR", w.arid.read(), w.araddr.read(), w.arlen.read(), w.arsize.read());
    if (w.rvalid && w.rready) row("R", w.rid.read(), 0, 0, 0, w.rdata.read(), 0, w.rlast.read(), w.rresp.read());
    events.flush();
}
}
