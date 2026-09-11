#pragma once
#include "axi_signals.hh"
#include <tlm>
#include <tlm_utils/simple_target_socket.h>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace storage_axi {
struct RequestAttributes : tlm::tlm_extension<RequestAttributes> {
    std::vector<unsigned char> enables;
    uint32_t requestor = 0, stream = 0, substream = 0;
    bool hasStream = false, hasSubstream = false;
    uint64_t payloadDelay = 0;
    tlm::tlm_extension_base* clone() const override {
        return new RequestAttributes(*this);
    }
    void copy_from(const tlm::tlm_extension_base& other) override {
        *this = static_cast<const RequestAttributes&>(other);
    }
};

class Master : public sc_core::sc_module {
  public:
    sc_core::sc_in<bool> clk{"clk"}, resetn{"resetn"};
    MasterPorts axi;
    // TLM socket width is a binding contract, not a limit on payload length.
    // Keep the existing gem5 bridge binding; the timed AXI bus is DataBits wide.
    tlm_utils::simple_target_socket<Master, 64> socket{"socket"};
    std::function<unsigned(tlm::tlm_generic_payload&)> functional;
    SC_HAS_PROCESS(Master);
    Master(sc_core::sc_module_name, unsigned slots, bool stalls,
           const std::string& traceDir);
    bool idle() const { return active.empty() && pending == nullptr; }
    unsigned maxId = 65535; // Inclusive wire-ID limit; set before simulation.
    uint64_t accepted = 0, completed = 0, maxActive = 0;
    uint64_t cycle = 0;
  private:
    struct Burst { uint64_t address; unsigned offset, bytes, beats, size; };
    struct Txn {
        tlm::tlm_generic_payload* gp;
        uint16_t id;
        std::vector<Burst> bursts;
        unsigned segment = 0, wbeat = 0, rbeat = 0;
        uint64_t awAfter = 0, wAfter = 0, arAfter = 0;
        uint64_t begin = 0, accept = 0, axiDone = 0;
    };
    unsigned slots;
    bool stalls;
    uint16_t nextId = 1;
    tlm::tlm_generic_payload* pending = nullptr;
    sc_core::sc_time pendingAt;
    uint64_t pendingBegin = 0;
    std::map<uint16_t, std::unique_ptr<Txn>> active;
    std::deque<uint16_t> awq, wq, arq, responses;
    tlm::tlm_generic_payload* responding = nullptr;
    std::ofstream trace;
    tlm::tlm_sync_enum transport(tlm::tlm_generic_payload&, tlm::tlm_phase&,
                                sc_core::sc_time&);
    void blocking(tlm::tlm_generic_payload&, sc_core::sc_time&);
    unsigned debug(tlm::tlm_generic_payload&);
    bool dmi(tlm::tlm_generic_payload&, tlm::tlm_dmi&) { return false; }
    void tick();
    void admit();
    void enqueueSegment(Txn&);
    void finishSegment(Txn&);
    void drive();
    void respond();
    static std::vector<Burst> split(uint64_t address, unsigned length);
};
}
