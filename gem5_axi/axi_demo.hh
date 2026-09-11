#pragma once
#include "axi_master.hh"
#include "axi_ram.hh"
#include "systemc/tlm_port_wrapper.hh"
#include "params/AxiDemo.hh"
#include <map>

namespace storage_axi {
class AouBackend;
class Demo : public sc_core::sc_module {
  public:
    SC_HAS_PROCESS(Demo);
    explicit Demo(const gem5::AxiDemoParams&);
    gem5::Port& gem5_getPort(const std::string&, int idx = -1) override;
    void finish();
    ~Demo();
  private:
    Demo(sc_core::sc_module_name, const gem5::AxiDemoParams&);
    Signals wires;
    sc_core::sc_clock clock;
    sc_core::sc_signal<bool> resetn{"resetn"};
    Master master;
    std::unique_ptr<Ram> ram;
    std::unique_ptr<AouBackend> aou;
    sc_gem5::TlmTargetWrapper<64> wrapper;
    sc_core::sc_trace_file* vcd = nullptr;
    std::ofstream events;
    std::string directory;
    uint64_t cycle = 0;
    bool finished = false;
    std::map<std::string, std::vector<Data>> held;
    std::map<std::string, uint64_t> handshakes, stalled;
    void reset();
    void sample();
    void channel(const std::string&, bool valid, bool ready,
                 const std::vector<Data>& payload);
    void row(const char*, uint64_t id = 0, uint64_t addr = 0,
             unsigned len = 0, unsigned size = 0, Data data = 0,
             unsigned strb = 0, bool last = false, unsigned resp = 0);
};
}
