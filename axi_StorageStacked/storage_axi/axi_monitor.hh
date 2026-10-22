#pragma once
#include "axi_signals.hh"
#include <fstream>
#include <map>
#include <vector>
namespace storage_axi {
// Optional master-side counters; storage-only users need not provide them.
struct AxiMasterStats {
    uint64_t accepted, completed, max_outstanding, capacity_denials;
    bool drained;
};
class AxiMonitor : public sc_core::sc_module {
 public:
    sc_core::sc_in<bool> clk{"clk"}, resetn{"resetn"};
    SC_HAS_PROCESS(AxiMonitor);
    AxiMonitor(sc_core::sc_module_name n, Signals& signals, const std::string& dir)
      : sc_module(n), wires(signals), events(dir + "/axi_events.csv"), directory(dir) {
        if (!events) throw std::runtime_error("cannot open AXI monitor log");
        events << "tick,cycle,channel,id,address,len,size,data,strb,last,resp\n";
        SC_METHOD(sample); sensitive << clk.pos(); dont_initialize();
    }
    void finish(uint64_t period, const AxiMasterStats* master = nullptr) {
        events.flush();
        std::ofstream f(directory + "/protocol_summary.json");
        f << '{';
        if (master) {
            f << "\"accepted\":" << master->accepted << ",\"completed\":" << master->completed
              << ",\"max_outstanding\":" << master->max_outstanding
              << ",\"capacity_denials\":" << master->capacity_denials
              << ",\"drained\":" << (master->drained ? "true" : "false") << ',';
        }
        f << "\"axi_data_bits\":256,\"ticks_per_second\":1000000000000000,\"period_ticks\":"
          << period << ",\"channels\":{";
        bool first=true;
        for (auto n : {"AW", "W", "B", "AR", "R"}) {
            if (!first) f << ',';
            first=false;
            f << '"' << n << "\":{\"handshakes\":" << handshakes[n]
              << ",\"stall_cycles\":" << stalled[n] << '}';
        }
        f << "}}\n";
    }
 private:
    Signals& wires;
    std::ofstream events;
    std::string directory;
    uint64_t cycle=0;
    std::map<std::string, std::vector<Data>> held;
    std::map<std::string, uint64_t> handshakes, stalled;
    void sample();
    void channel(const std::string&, bool, bool, const std::vector<Data>&);
    void row(const char*, uint64_t id=0, uint64_t addr=0, unsigned len=0,
             unsigned size=0, Data data=0, unsigned strb=0, bool last=false, unsigned resp=0);
};
}
