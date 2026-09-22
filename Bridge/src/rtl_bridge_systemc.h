#pragma once

#include "mem_backend.h"
#include "ucie_fdi.h"

#include <systemc.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Vstorage_bridge_top;

SC_MODULE(RtlBridgeSystemC) {
    sc_in<bool> clk, rst_n;
    sc_in<unsigned> link_state;
    sc_fifo_in<FdiFlit> link_rx;
    sc_fifo_out<FdiFlit> link_tx;

    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t retries = 0;
    std::uint64_t synthesized_errors = 0;
    std::uint64_t tx_read_responses = 0;
    std::uint64_t tx_write_responses = 0;
    std::uint64_t tx_credit_only = 0;
    std::uint64_t granted_wreq = 0;
    std::uint64_t granted_rreq = 0;
    std::uint64_t granted_wdata = 0;

    SC_HAS_PROCESS(RtlBridgeSystemC);
    RtlBridgeSystemC(sc_module_name name, std::uint64_t base, std::uint64_t size);
    ~RtlBridgeSystemC() override;

    bool idle() const;
    void finish();
    std::string debug_state() const;

private:
    struct Meta {
        std::uint64_t tag = 0;
        unsigned offset = 0;
        unsigned bytes = 0;
        bool write = false;
    };
    struct Response {
        std::uint64_t tag = 0;
        unsigned status = 0;
        std::vector<std::uint8_t> data;
    };

    std::uint64_t base_;
    std::uint64_t size_;
    std::unique_ptr<Vstorage_bridge_top> model_;
    bridge::MemBackend memory_;
    std::unordered_map<std::uint64_t, Meta> live_;
    std::deque<Response> responses_;
    FdiFlit held_rx_;
    bool rx_valid_ = false;
    bool finished_ = false;
    std::uint64_t tx_trace_id_ = 0;

    static bridge::MemBackend::Options options(std::uint64_t size);
    void run();
    void drive_rx();
    void accept_request();
    void advance_memory();
    void drive_response();
};
