#pragma once
#include "axi_signals.hh"
#include <tlm>
#include <deque>
#include <vector>

namespace storage_axi {
class Ram : public sc_core::sc_module {
  public:
    sc_core::sc_in<bool> clk{"clk"}, resetn{"resetn"};
    SlavePorts axi;
    SC_HAS_PROCESS(Ram);
    Ram(sc_core::sc_module_name, uint64_t base, unsigned size,
        unsigned latency, bool stalls);
    unsigned access(tlm::tlm_generic_payload&);
  private:
    struct Address { uint64_t address; unsigned id, beats, size, seen = 0;
                     uint64_t ready = 0; bool error = false; };
    struct WriteBeat { Data data; uint32_t strb; bool last; };
    struct WriteResponse { unsigned id, resp; uint64_t ready; };
    uint64_t base, cycle = 0;
    unsigned latency;
    bool stalls;
    std::vector<uint8_t> bytes;
    std::deque<Address> writes, reads;
    std::deque<WriteBeat> data;
    std::deque<WriteResponse> responses;
    bool contains(uint64_t a, unsigned n) const;
    void tick();
};
}
