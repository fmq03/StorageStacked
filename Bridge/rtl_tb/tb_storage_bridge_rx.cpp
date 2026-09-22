#include "Vstorage_bridge_rx.h"
#include "verilated.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned kPlpBytes = 250;
constexpr unsigned kPayloadOffset = 10;
constexpr unsigned kGranuleBytes = 5;

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}
    void put(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit_) {
            if ((value >> (width - 1 - i)) & 1ULL)
                bytes_.at(bit_ / 8) |= std::uint8_t(1U << (7 - bit_ % 8));
        }
    }
    void skip(unsigned width) { bit_ += width; }
private:
    std::vector<std::uint8_t>& bytes_;
    unsigned bit_ = 0;
};

std::vector<std::uint8_t> make_req(bool write, unsigned rp, std::uint16_t user,
                                   std::uint16_t id, std::uint8_t size,
                                   std::uint8_t len, std::uint8_t qos,
                                   std::uint64_t addr) {
    std::vector<std::uint8_t> out(15);
    BitWriter bw(out);
    bw.put(write ? 1 : 2, 4); bw.put(rp, 2); bw.skip(1); bw.put(0, 1);
    bw.put(user, 16); bw.put(id, 10); bw.put(size, 3); bw.put(0, 3);
    bw.put(len, 8); bw.put(0, 4); bw.put(qos, 4); bw.put(addr, 64);
    return out;
}

std::vector<std::uint8_t> make_wdata_full(unsigned rp, std::uint16_t user,
                                          const std::array<std::uint8_t, 32>& lanes) {
    std::vector<std::uint8_t> out(35);
    out[0] = std::uint8_t(0x60 | ((rp & 3) << 2));
    out[1] = std::uint8_t(user >> 8);
    out[2] = std::uint8_t(user);
    for (unsigned i = 0; i < lanes.size(); ++i) out[3 + i] = lanes[31 - i];
    return out;
}

void set_start(std::array<std::uint8_t, kPlpBytes>& f, unsigned g) {
    if (g < 4) f[0] |= std::uint8_t(1U << (4 + g));
    else if (g < 12) f[1] |= std::uint8_t(1U << (g - 4));
    else if (g < 16) f[2] |= std::uint8_t(1U << (4 + g - 12));
    else if (g < 24) f[3] |= std::uint8_t(1U << (g - 16));
    else if (g < 28) f[6] |= std::uint8_t(1U << (4 + g - 24));
    else if (g < 36) f[7] |= std::uint8_t(1U << (g - 28));
    else if (g < 40) f[8] |= std::uint8_t(1U << (4 + g - 36));
    else f[9] |= std::uint8_t(1U << (g - 40));
}

void put_message(std::array<std::uint8_t, kPlpBytes>& f, unsigned granule,
                 const std::vector<std::uint8_t>& msg, unsigned begin = 0) {
    const unsigned capacity = (48 - granule) * kGranuleBytes;
    const unsigned count = std::min<unsigned>(capacity, msg.size() - begin);
    for (unsigned i = 0; i < count; ++i)
        f[kPayloadOffset + granule * kGranuleBytes + i] = msg[begin + i];
}

void set_byte(WData* words, unsigned byte, std::uint8_t value) {
    const unsigned word = byte / 4;
    const unsigned shift = (byte % 4) * 8;
    words[word] = (words[word] & ~(0xffU << shift)) | (std::uint32_t(value) << shift);
}

std::uint8_t get_byte(const WData* words, unsigned byte) {
    return std::uint8_t(words[byte / 4] >> ((byte % 4) * 8));
}

class Sim {
public:
    Sim() {
        dut.req_ready = 1;
        dut.wdata_ready = 1;
        dut.misc_ready = 1;
        dut.link_active = 1;
        dut.fdi_rx_valid = 0;
        dut.rst_n = 0;
        tick(); tick();
        dut.rst_n = 1;
        tick();
    }

    void tick() {
        dut.clk = 0; dut.eval();
        dut.clk = 1; dut.eval();
        ++cycles;
    }

    void send(const std::array<std::uint8_t, kPlpBytes>& frame) {
        while (!dut.fdi_rx_ready) tick();
        for (unsigned i = 0; i < 63; ++i) dut.fdi_rx_plp[i] = 0;
        for (unsigned i = 0; i < frame.size(); ++i) set_byte(dut.fdi_rx_plp, i, frame[i]);
        dut.fdi_rx_valid = 1;
        tick();
        dut.fdi_rx_valid = 0;
    }

    template <typename Predicate>
    void until(Predicate predicate, const char* what, unsigned limit = 300) {
        for (unsigned i = 0; i < limit; ++i) {
            if (predicate()) return;
            tick();
        }
        throw std::runtime_error(what);
    }

    Vstorage_bridge_rx dut;
    std::uint64_t cycles = 0;
};

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
    std::cout << "[PASS] " << what << '\n';
}
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Sim sim;

        auto read = make_req(false, 0, 0x1234, 0x155, 5, 3, 9,
                             0x0123456789abcdefULL);
        auto write = make_req(true, 0, 0x5678, 0x2aa, 5, 0, 4,
                              0x0000000012345000ULL);
        std::array<std::uint8_t, kPlpBytes> first{};
        set_start(first, 0); put_message(first, 0, read);
        set_start(first, 3); put_message(first, 3, write);
        sim.send(first);

        sim.until([&] { return sim.dut.req_valid; }, "read request timeout");
        require(!sim.dut.req_write, "ReadReq direction decoded");
        require(sim.dut.req_user == 0x1234 && sim.dut.req_id == 0x155,
                "ReadReq user/id decoded");
        require(sim.dut.req_size == 5 && sim.dut.req_len == 3 && sim.dut.req_qos == 9,
                "ReadReq shape decoded");
        require(sim.dut.req_addr == 0x0123456789abcdefULL, "ReadReq address decoded");
        sim.tick();

        sim.until([&] { return sim.dut.req_valid; }, "write request timeout");
        require(sim.dut.req_write && sim.dut.req_addr == 0x12345000ULL,
                "second message in one PLP decoded");
        sim.tick();

        std::array<std::uint8_t, 32> lanes{};
        for (unsigned i = 0; i < lanes.size(); ++i) lanes[i] = std::uint8_t(0x80 + i);
        auto wdata = make_wdata_full(0, 0xbeef, lanes);
        std::array<std::uint8_t, kPlpBytes> tail{};
        std::array<std::uint8_t, kPlpBytes> head{};
        set_start(tail, 45);
        put_message(tail, 45, wdata);
        put_message(head, 0, wdata, 15);
        sim.send(tail);
        sim.send(head);
        sim.until([&] { return sim.dut.wdata_valid; }, "spanning WriteData timeout");
        require(sim.dut.wdata_user == 0xbeef, "spanning WriteData metadata decoded");
        require(sim.dut.wdata_mask == 0xffffffffU, "WriteDataFull creates full byte mask");
        bool bytes_ok = true;
        for (unsigned i = 0; i < lanes.size(); ++i)
            bytes_ok &= get_byte(sim.dut.wdata_data, i) == lanes[i];
        require(bytes_ok, "spanning WriteData preserves all 32 lanes");

        sim.dut.wdata_ready = 0;
        sim.tick();
        const auto held_user = sim.dut.wdata_user;
        const auto held_first = get_byte(sim.dut.wdata_data, 0);
        sim.tick();
        require(sim.dut.wdata_valid && sim.dut.wdata_user == held_user &&
                    get_byte(sim.dut.wdata_data, 0) == held_first,
                "decoded message remains stable under backpressure");
        sim.dut.wdata_ready = 1;
        sim.tick();

        std::cout << "RTL RX regression passed in " << sim.cycles << " cycles\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
