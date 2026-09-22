#pragma once

#include "simple_mem_if.h"
#include "ucie_fdi.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <systemc>

class Vstorage_bridge_top;

namespace storage_axi {

// gem5-native SystemC transactor around the Verilated storage-side Bridge.
// The RTL-facing MC interface is converted to the existing SimpleMem FIFO
// contract so both bridge implementations share the same memory backend.
class RtlAouTarget : public sc_core::sc_module {
  public:
    sc_core::sc_in<bool> clk{"clk"}, rst_n{"rst_n"};
    sc_core::sc_in<unsigned> link_state{"link_state"};
    sc_core::sc_fifo_in<FdiFlit> link_rx{"link_rx"};
    sc_core::sc_fifo_out<FdiFlit> link_tx{"link_tx"};
    sc_core::sc_fifo_out<SimpleMemRequest> mem_req{"mem_req"};
    sc_core::sc_fifo_in<SimpleMemResponse> mem_rsp{"mem_rsp"};

    uint64_t reads = 0, writes = 0, read_beats = 0, write_beats = 0;
    uint64_t submitted = 0, completed = 0, memory_stalls = 0;

    SC_HAS_PROCESS(RtlAouTarget);
    RtlAouTarget(sc_core::sc_module_name name, unsigned rp_count);
    ~RtlAouTarget() override;

    bool idle() const;

  private:
    static constexpr unsigned PlpBytes = 250;
    static constexpr unsigned DataBytes = 32;
    static constexpr unsigned IdCount = 1024;

    struct Live {
        bool valid = false;
        bool write = false;
        uint32_t rtl_tag = 0;
        uint8_t lane = 0;
        uint8_t bytes = 0;
    };

    std::unique_ptr<Vstorage_bridge_top> model;
    unsigned rp_count;
    std::array<Live, IdCount> live{};
    unsigned next_id = 1;
    unsigned live_count = 0;
    FdiFlit held_rx;
    bool rx_valid = false;
    SimpleMemResponse held_rsp;
    bool rsp_valid = false;
    uint64_t tx_trace_id = 0;

    void run();
    void drive_rx();
    void drive_response();
    unsigned available_id() const;
    void submit_request(unsigned id);
    void retire_response();
    void transmit(const std::array<uint8_t, PlpBytes>& bytes);
};

} // namespace storage_axi
