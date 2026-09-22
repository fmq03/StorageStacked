#include "Vstorage_bridge_top.h"
#include "mem_backend.h"
#include "verilated.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned kPlpBytes = 250;
constexpr unsigned kPayload = 10;

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint64_t value, unsigned width) {
        for (unsigned n = 0; n < width; ++n, ++bit_)
            if ((value >> (width - 1 - n)) & 1ULL)
                out_.at(bit_ / 8) |= std::uint8_t(1U << (7 - bit_ % 8));
    }
    void skip(unsigned width) { bit_ += width; }
private:
    std::vector<std::uint8_t>& out_;
    unsigned bit_ = 0;
};

std::vector<std::uint8_t> request(bool write, std::uint16_t user,
                                  std::uint16_t id, std::uint64_t addr) {
    std::vector<std::uint8_t> out(15);
    BitWriter b(out);
    b.put(write ? 1 : 2, 4); b.put(0, 2); b.skip(1); b.put(0, 1);
    b.put(user, 16); b.put(id, 10); b.put(5, 3); b.put(0, 3);
    b.put(0, 8); b.put(0, 4); b.put(0, 4); b.put(addr, 64);
    return out;
}

std::vector<std::uint8_t> write_full(
    const std::array<std::uint8_t, 32>& lanes) {
    std::vector<std::uint8_t> out(35);
    out[0] = 0x60;
    out[1] = 0x12;
    out[2] = 0x34;
    for (unsigned i = 0; i < lanes.size(); ++i) out[3 + i] = lanes[31 - i];
    return out;
}

void set_start(std::array<std::uint8_t, kPlpBytes>& frame, unsigned granule) {
    if (granule < 4) frame[0] |= std::uint8_t(1U << (4 + granule));
    else if (granule < 12) frame[1] |= std::uint8_t(1U << (granule - 4));
    else if (granule < 16) frame[2] |= std::uint8_t(1U << (4 + granule - 12));
    else if (granule < 24) frame[3] |= std::uint8_t(1U << (granule - 16));
    else if (granule < 28) frame[6] |= std::uint8_t(1U << (4 + granule - 24));
    else if (granule < 36) frame[7] |= std::uint8_t(1U << (granule - 28));
    else if (granule < 40) frame[8] |= std::uint8_t(1U << (4 + granule - 36));
    else frame[9] |= std::uint8_t(1U << (granule - 40));
}

void put(std::array<std::uint8_t, kPlpBytes>& frame, unsigned granule,
         const std::vector<std::uint8_t>& message) {
    for (unsigned i = 0; i < message.size(); ++i)
        frame[kPayload + granule * 5 + i] = message[i];
}

void set_byte(WData* words, unsigned byte, std::uint8_t value) {
    const unsigned word = byte / 4;
    const unsigned shift = 8 * (byte % 4);
    words[word] = (words[word] & ~(0xffU << shift)) |
                  (std::uint32_t(value) << shift);
}

std::uint8_t get_byte(const WData* words, unsigned byte) {
    return std::uint8_t(words[byte / 4] >> (8 * (byte % 4)));
}

void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
    std::cout << "[PASS] " << what << '\n';
}

class Sim {
public:
    Sim() : memory(options()) {
        require(memory.transaction_bytes() == 32,
                "mem_sim transaction width matches RTL MC port");
        dut.clk = 0;
        dut.rst_n = 0;
        dut.link_active = 1;
        dut.fdi_rx_valid = 0;
        dut.fdi_tx_ready = 1;
        dut.mc_rsp_valid = 0;
        tick(); tick();
        dut.rst_n = 1;
        tick();
    }

    ~Sim() { memory.finish(0); }

    void tick() {
        dut.clk = 0;
        dut.eval();

        dut.mc_req_ready = 1;
        if (dut.rst_n && dut.mc_req_valid) {
            bridge::MemTxn txn;
            txn.write = dut.mc_req_write;
            txn.address = dut.mc_req_addr;
            txn.bytes = dut.mc_req_bytes;
            txn.host_request_id = dut.mc_req_tag;
            if (txn.write) {
                txn.payload.resize(txn.bytes);
                txn.byte_mask.resize(txn.bytes);
                txn.has_byte_mask = true;
                for (unsigned i = 0; i < txn.bytes; ++i) {
                    txn.payload[i] = get_byte(dut.mc_req_data, i);
                    txn.byte_mask[i] = (dut.mc_req_byte_mask >> i) & 1U ? 0xff : 0;
                }
            }
            const int rc = memory.submit(txn);
            if (rc < 0) throw std::runtime_error(memory.last_error());
            dut.mc_req_ready = rc == 0;
            if (rc == 0) ++submitted;
        }

        if (dut.rst_n) {
            memory.step(8);
            bridge::MemResp response;
            while (memory.poll(response)) responses.push_back(std::move(response));
        }

        dut.mc_rsp_valid = !responses.empty();
        if (!responses.empty()) {
            const auto& response = responses.front();
            dut.mc_rsp_tag = std::uint32_t(response.host_request_id);
            dut.mc_rsp_status = response.status <= 2 ? 0 : 2;
            for (unsigned w = 0; w < 8; ++w) dut.mc_rsp_data[w] = 0;
            for (unsigned i = 0; i < response.data.size() && i < 32; ++i)
                set_byte(dut.mc_rsp_data, i, response.data[i]);
        }
        dut.eval();
        const bool response_fire = dut.mc_rsp_valid && dut.mc_rsp_ready;

        dut.clk = 1;
        dut.eval();
        if (response_fire) responses.pop_front();
        if (dut.protocol_error) throw std::runtime_error("RTL protocol_error asserted");
        ++cycles;
    }

    template <class Predicate>
    void until(Predicate predicate, const char* what, unsigned limit = 20000) {
        for (unsigned n = 0; n < limit; ++n) {
            dut.eval();
            if (predicate()) return;
            tick();
        }
        throw std::runtime_error(what);
    }

    void send(const std::array<std::uint8_t, kPlpBytes>& frame) {
        until([&] { return dut.fdi_rx_ready; }, "FDI input timeout");
        for (unsigned w = 0; w < 63; ++w) dut.fdi_rx_plp[w] = 0;
        for (unsigned i = 0; i < frame.size(); ++i)
            set_byte(dut.fdi_rx_plp, i, frame[i]);
        dut.fdi_rx_valid = 1;
        tick();
        dut.fdi_rx_valid = 0;
    }

    std::array<std::uint8_t, kPlpBytes> receive() {
        for (;;) {
            until([&] { return dut.fdi_tx_valid; }, "FDI output timeout");
            std::array<std::uint8_t, kPlpBytes> frame{};
            for (unsigned i = 0; i < frame.size(); ++i)
                frame[i] = get_byte(dut.fdi_tx_plp, i);
            tick();
            if ((frame[0] & 0xf0) != 0) return frame;
        }
    }

    static bridge::MemBackend::Options options() {
        bridge::MemBackend::Options opt;
        opt.standard = "hbm4";
        opt.channels = 1;
        opt.transaction_bytes = 32;
        opt.window_bytes = 1U << 20;
        opt.retain_command_trace = false;
        return opt;
    }

    Vstorage_bridge_top dut;
    bridge::MemBackend memory;
    std::deque<bridge::MemResp> responses;
    std::uint64_t cycles = 0;
    std::uint64_t submitted = 0;
};
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Sim sim;
        std::array<std::uint8_t, 32> expected{};
        for (unsigned i = 0; i < expected.size(); ++i)
            expected[i] = std::uint8_t(0x31 + 3 * i);

        std::array<std::uint8_t, kPlpBytes> write_frame{};
        // RP0: grant 64 RDATA granules and 8 WRESP granules.
        write_frame[4] = 0x00;
        write_frame[5] = 0x3c;
        set_start(write_frame, 0);
        put(write_frame, 0, request(true, 0x2201, 0x91, 0x1000));
        set_start(write_frame, 3);
        put(write_frame, 3, write_full(expected));
        sim.send(write_frame);
        const auto write_response = sim.receive();
        require(write_response[kPayload] == 0x50,
                "real mem_sim write completion becomes WriteResp");

        std::array<std::uint8_t, kPlpBytes> read_frame{};
        set_start(read_frame, 0);
        put(read_frame, 0, request(false, 0x2202, 0x92, 0x1000));
        sim.send(read_frame);
        const auto read_response = sim.receive();
        require(read_response[kPayload] == 0x40,
                "real mem_sim read completion becomes ReadData");
        bool same = true;
        for (unsigned lane = 0; lane < expected.size(); ++lane)
            same &= read_response[kPayload + 5 + (31 - lane)] == expected[lane];
        require(same, "RTL -> mem_sim -> RTL readback is byte-exact");
        require(sim.submitted == 2, "one write and one read reached mem_sim");

        sim.until([&] {
            return !sim.dut.busy && sim.responses.empty() && sim.memory.quiescent();
        }, "mem_sim did not quiesce");
        require(sim.memory.clock() > 0, "mem_sim timing model advanced");
        std::cout << "RTL mem_sim regression passed in " << sim.cycles
                  << " Bridge cycles and " << sim.memory.clock() << " mem_sim ticks\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
