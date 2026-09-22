#include "Vstorage_bridge_top.h"
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
    void put(std::uint64_t v, unsigned width) {
        for (unsigned n = 0; n < width; ++n, ++bit_)
            if ((v >> (width - 1 - n)) & 1ULL)
                out_.at(bit_ / 8) |= std::uint8_t(1U << (7 - bit_ % 8));
    }
    void skip(unsigned width) { bit_ += width; }
private:
    std::vector<std::uint8_t>& out_;
    unsigned bit_ = 0;
};

std::vector<std::uint8_t> request(bool write, std::uint16_t user,
                                  std::uint16_t id, std::uint8_t size,
                                  std::uint8_t len, std::uint64_t addr,
                                  std::uint8_t rp = 0) {
    std::vector<std::uint8_t> out(15);
    BitWriter b(out);
    b.put(write ? 1 : 2, 4); b.put(rp, 2); b.skip(1); b.put(0, 1);
    b.put(user, 16); b.put(id, 10); b.put(size, 3); b.put(0, 3);
    b.put(len, 8); b.put(0, 4); b.put(3, 4); b.put(addr, 64);
    return out;
}

std::vector<std::uint8_t> write_full(std::uint16_t user,
                                     const std::array<std::uint8_t, 32>& lane,
                                     std::uint8_t rp = 0) {
    std::vector<std::uint8_t> out(35);
    out[0] = std::uint8_t(0x60 | (rp << 2));
    out[1] = std::uint8_t(user >> 8);
    out[2] = std::uint8_t(user);
    for (unsigned i = 0; i < 32; ++i) out[3 + i] = lane[31 - i];
    return out;
}

void start(std::array<std::uint8_t, kPlpBytes>& f, unsigned g) {
    if (g < 4) f[0] |= std::uint8_t(1U << (4 + g));
    else if (g < 12) f[1] |= std::uint8_t(1U << (g - 4));
    else if (g < 16) f[2] |= std::uint8_t(1U << (4 + g - 12));
    else if (g < 24) f[3] |= std::uint8_t(1U << (g - 16));
    else if (g < 28) f[6] |= std::uint8_t(1U << (4 + g - 24));
    else if (g < 36) f[7] |= std::uint8_t(1U << (g - 28));
    else if (g < 40) f[8] |= std::uint8_t(1U << (4 + g - 36));
    else f[9] |= std::uint8_t(1U << (g - 40));
}

void put(std::array<std::uint8_t, kPlpBytes>& f, unsigned g,
         const std::vector<std::uint8_t>& msg) {
    for (unsigned i = 0; i < msg.size(); ++i) f[kPayload + 5 * g + i] = msg[i];
}

void set_byte(WData* words, unsigned byte, std::uint8_t value) {
    const unsigned w = byte / 4, shift = 8 * (byte % 4);
    words[w] = (words[w] & ~(0xffU << shift)) | (std::uint32_t(value) << shift);
}

std::uint8_t get_byte(const WData* words, unsigned byte) {
    return std::uint8_t(words[byte / 4] >> (8 * (byte % 4)));
}

std::uint16_t response_id(const std::array<std::uint8_t, kPlpBytes>& frame) {
    return std::uint16_t((std::uint16_t(frame[kPayload + 3]) << 2) |
                         (frame[kPayload + 4] >> 6));
}

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
    std::cout << "[PASS] " << what << '\n';
}

class Sim {
public:
    Sim() {
        d.clk = 0; d.rst_n = 0; d.link_active = 1;
        d.fdi_rx_valid = 0; d.fdi_tx_ready = 0;
        d.mc_req_ready = 0; d.mc_rsp_valid = 0;
        tick(); tick(); d.rst_n = 1; tick();
    }
    void tick() {
        d.clk = 0; d.eval();
        d.clk = 1; d.eval();
        ++cycles;
    }
    template <class P> void until(P p, const char* what, unsigned limit = 500) {
        for (unsigned n = 0; n < limit; ++n) {
            d.eval();
            if (p()) return;
            tick();
        }
        throw std::runtime_error(what);
    }
    void send(const std::array<std::uint8_t, kPlpBytes>& f) {
        until([&] { return d.fdi_rx_ready; }, "FDI RX ready timeout");
        for (unsigned n = 0; n < 63; ++n) d.fdi_rx_plp[n] = 0;
        for (unsigned n = 0; n < f.size(); ++n) set_byte(d.fdi_rx_plp, n, f[n]);
        d.fdi_rx_valid = 1; tick(); d.fdi_rx_valid = 0;
    }
    struct Txn {
        bool write;
        std::uint64_t addr;
        unsigned bytes;
        std::uint32_t tag;
        std::array<std::uint8_t, 32> data{};
        std::uint32_t mask = 0;
    };
    Txn accept_request() {
        until([&] { return d.mc_req_valid; }, "MC request timeout");
        d.mc_req_ready = 0;
        Txn t{bool(d.mc_req_write), d.mc_req_addr, d.mc_req_bytes, d.mc_req_tag};
        std::array<std::uint8_t, 32> snapshot{};
        for (unsigned i = 0; i < 32; ++i) {
            snapshot[i] = get_byte(d.mc_req_data, i);
            t.data[i] = snapshot[i];
        }
        t.mask = d.mc_req_byte_mask;
        tick();
        require(d.mc_req_valid && d.mc_req_addr == t.addr && d.mc_req_tag == t.tag,
                "MC request remains stable under backpressure");
        bool data_stable = true;
        for (unsigned i = 0; i < 32; ++i)
            data_stable &= get_byte(d.mc_req_data, i) == snapshot[i];
        require(data_stable, "MC request data remains stable");
        d.mc_req_ready = 1; tick(); d.mc_req_ready = 0;
        return t;
    }
    void respond(const Txn& t, const std::vector<std::uint8_t>& data = {}) {
        d.mc_rsp_tag = t.tag; d.mc_rsp_status = 0;
        for (unsigned n = 0; n < 8; ++n) d.mc_rsp_data[n] = 0;
        for (unsigned n = 0; n < data.size(); ++n) set_byte(d.mc_rsp_data, n, data[n]);
        d.mc_rsp_valid = 1;
        until([&] { return d.mc_rsp_ready; }, "MC response ready timeout");
        tick(); d.mc_rsp_valid = 0;
    }
    std::array<std::uint8_t, kPlpBytes> receive() {
        if (!responses.empty()) {
            auto out = responses.front();
            responses.pop_front();
            return out;
        }
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            until([&] { return d.fdi_tx_valid; }, "FDI TX timeout");
            std::array<std::uint8_t, kPlpBytes> frame{};
            for (unsigned n = 0; n < frame.size(); ++n)
                frame[n] = get_byte(d.fdi_tx_plp, n);
            tick();
            ingest(frame);
            if (!responses.empty()) {
                auto out = responses.front();
                responses.pop_front();
                return out;
            }
        }
        throw std::runtime_error("only credit-only PLPs observed");
    }
    Vstorage_bridge_top d;
    std::uint64_t cycles = 0;
private:
    static bool has_start(const std::array<std::uint8_t, kPlpBytes>& frame,
                          unsigned g) {
        if (g < 4) return frame[0] & (1U << (4 + g));
        if (g < 12) return frame[1] & (1U << (g - 4));
        if (g < 16) return frame[2] & (1U << (4 + g - 12));
        if (g < 24) return frame[3] & (1U << (g - 16));
        if (g < 28) return frame[6] & (1U << (4 + g - 24));
        if (g < 36) return frame[7] & (1U << (g - 28));
        if (g < 40) return frame[8] & (1U << (4 + g - 36));
        return frame[9] & (1U << (g - 40));
    }
    static unsigned response_bytes(std::uint8_t header) {
        const unsigned type = header >> 4;
        if (type == 4) return 40;
        if (type == 5) return 5;
        throw std::runtime_error("unexpected TX message type");
    }
    void finish_message() {
        std::array<std::uint8_t, kPlpBytes> out{};
        start(out, 0);
        for (unsigned n = 0; n < carry.size(); ++n) out[kPayload + n] = carry[n];
        responses.push_back(out);
        carry.clear();
        carry_bytes = 0;
    }
    void ingest(const std::array<std::uint8_t, kPlpBytes>& frame) {
        for (unsigned g = 0; g < 48; ++g) {
            if (has_start(frame, g)) {
                if (!carry.empty())
                    throw std::runtime_error("TX MsgStart overlaps spill continuation");
                carry_bytes = response_bytes(frame[kPayload + 5 * g]);
            }
            if (carry_bytes != 0) {
                for (unsigned b = 0; b < 5 && carry.size() < carry_bytes; ++b)
                    carry.push_back(frame[kPayload + 5 * g + b]);
                if (carry.size() == carry_bytes) finish_message();
            }
        }
    }
    std::deque<std::array<std::uint8_t, kPlpBytes>> responses;
    std::vector<std::uint8_t> carry;
    unsigned carry_bytes = 0;
};
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        {
            Sim timing;
            timing.d.fdi_tx_ready = 1;
            for (unsigned n = 0; n < 8; ++n) timing.tick();
            require(!timing.d.fdi_tx_valid, "initial credit grants drain completely");

            std::array<std::uint8_t, kPlpBytes> blocked_read{};
            start(blocked_read, 0);
            put(blocked_read, 0, request(false, 0x0101, 0x11, 5, 0, 0x8000));
            timing.send(blocked_read);
            timing.until([&] { return timing.d.mc_req_valid; },
                         "blocked read did not reach MC output");
            bool no_early_req_credit = true;
            for (unsigned n = 0; n < 4; ++n) {
                no_early_req_credit &= !timing.d.fdi_tx_valid;
                timing.tick();
            }
            require(no_early_req_credit,
                    "ReadReq credit is held until the MC request handshake");
            timing.d.mc_req_ready = 1;
            timing.tick();
            timing.d.mc_req_ready = 0;
            timing.until([&] { return timing.d.fdi_tx_valid; },
                         "ReadReq credit return timeout");
            const std::uint16_t read_credit =
                std::uint16_t(get_byte(timing.d.fdi_tx_plp, 4)) |
                (std::uint16_t(get_byte(timing.d.fdi_tx_plp, 5)) << 8);
            require(((read_credit >> 3) & 7U) != 0,
                    "ReadReq credit returns after the MC request handshake");
        }

        {
            Sim timing;
            timing.d.fdi_tx_ready = 1;
            for (unsigned n = 0; n < 8; ++n) timing.tick();
            std::array<std::uint8_t, 32> lanes{};
            std::array<std::uint8_t, kPlpBytes> data_only{};
            start(data_only, 0);
            put(data_only, 0, write_full(0x0202, lanes));
            timing.send(data_only);
            bool no_early_wdata_credit = true;
            for (unsigned n = 0; n < 4; ++n) {
                no_early_wdata_credit &= !timing.d.fdi_tx_valid;
                timing.tick();
            }
            require(no_early_wdata_credit,
                    "WriteData credit is held while data waits in the ingress FIFO");

            std::array<std::uint8_t, kPlpBytes> write_req{};
            start(write_req, 0);
            put(write_req, 0, request(true, 0x0203, 0x12, 5, 0, 0x8020));
            timing.send(write_req);
            timing.until([&] { return timing.d.fdi_tx_valid; },
                         "WriteData credit return timeout");
            const std::uint16_t data_credit =
                std::uint16_t(get_byte(timing.d.fdi_tx_plp, 4)) |
                (std::uint16_t(get_byte(timing.d.fdi_tx_plp, 5)) << 8);
            require(((data_credit >> 6) & 7U) != 0,
                    "WriteData credit returns after pairing into the parent slot");
        }

#ifdef TEST_RP2
        {
            Sim planes;
            planes.d.fdi_tx_ready = 1;
            for (unsigned n = 0; n < 12; ++n) planes.tick();
            std::array<std::uint8_t, kPlpBytes> requests{};
            // Only RP1 receives one ReadData message worth of credit.
            requests[4] = 0x00;
            requests[5] = 0x46;
            start(requests, 0);
            put(requests, 0, request(false, 0x0300, 0x21, 5, 0, 0x9000, 0));
            start(requests, 3);
            put(requests, 3, request(false, 0x0301, 0x22, 5, 0, 0x9020, 1));
            planes.send(requests);
            auto a = planes.accept_request();
            auto b = planes.accept_request();
            const auto& rp0 = a.addr == 0x9000 ? a : b;
            const auto& rp1 = a.addr == 0x9020 ? a : b;
            planes.respond(rp0, std::vector<std::uint8_t>(32, 0x40));
            planes.respond(rp1, std::vector<std::uint8_t>(32, 0x80));
            auto rp1_first = planes.receive();
            require((rp1_first[kPayload] & 0x0c) == 0x04 &&
                        response_id(rp1_first) == 0x22,
                    "RP1 response bypasses completed RP0 response without RP0 credit");

            std::array<std::uint8_t, kPlpBytes> rp0_credit{};
            rp0_credit[4] = 0x00;
            rp0_credit[5] = 0x06;
            planes.send(rp0_credit);
            auto rp0_after_credit = planes.receive();
            require((rp0_after_credit[kPayload] & 0x0c) == 0x00 &&
                        response_id(rp0_after_credit) == 0x21,
                    "RP0 response resumes when its own credit arrives");
        }
#endif

        {
            Sim aggregate;
            aggregate.d.fdi_tx_ready = 1;
            for (unsigned n = 0; n < 8; ++n) aggregate.tick();

            std::array<std::uint8_t, kPlpBytes> req{};
            req[5] = 0x30;
            start(req, 0);
            put(req, 0, request(true, 0x0440, 0x44, 2, 7, 0x6000));
            aggregate.send(req);

            std::array<std::uint8_t, 32> expected{};
            for (unsigned beat = 0; beat < 8; ++beat) {
                std::array<std::uint8_t, 32> lanes{};
                for (unsigned byte = 0; byte < 4; ++byte) {
                    lanes[4 * beat + byte] =
                        std::uint8_t(0x40 + 4 * beat + byte);
                    expected[4 * beat + byte] = lanes[4 * beat + byte];
                }
                std::array<std::uint8_t, kPlpBytes> data{};
                start(data, 0);
                put(data, 0, write_full(std::uint16_t(0x0500 + beat), lanes));
                aggregate.send(data);
            }

            const auto txn = aggregate.accept_request();
            bool packed = txn.write && txn.addr == 0x6000 && txn.bytes == 32 &&
                          txn.mask == 0xffffffffU;
            for (unsigned byte = 0; byte < 32; ++byte)
                packed &= txn.data[byte] == expected[byte];
            require(packed,
                    "eight adjacent 4-byte beats merge into one compact 32-byte MC transaction");
            aggregate.respond(txn);
            const auto response = aggregate.receive();
            require(response_id(response) == 0x44,
                    "aggregated narrow write retires as one parent response");
            aggregate.tick();
            require(!aggregate.d.mc_req_valid,
                    "aggregated narrow write emits no duplicate child transaction");
        }

        Sim s;
        require(s.d.fdi_tx_valid && (get_byte(s.d.fdi_tx_plp, 0) & 0xf0) == 0 &&
                    (get_byte(s.d.fdi_tx_plp, 4) != 0 || get_byte(s.d.fdi_tx_plp, 5) != 0),
                "Bridge publishes initial request/data credits in a credit-only PLP");
        s.d.fdi_tx_ready = 1;
        s.tick();
        std::array<std::uint8_t, kPlpBytes> read_frame{};
        // Header grants only one ReadData message. The following CrdtGrant must
        // supply the second RDATA credit and WRESP credit used later in the test.
        read_frame[4] = 0x00;
        read_frame[5] = 0x06;
        const std::vector<std::uint8_t> credit_msg{
            0x08, 0x88, 0x0c, 0x01, 0x38, 0x17, 0x00, 0xc0, 0x00, 0x00};
        start(read_frame, 0);
        put(read_frame, 0, credit_msg);
        start(read_frame, 2);
        put(read_frame, 2, request(false, 0x1122, 0x155, 4, 1, 0x1010));
        s.send(read_frame);

        auto r0 = s.accept_request();
        require(!r0.write && r0.addr == 0x1010 && r0.bytes == 16,
                "narrow read becomes compact MC transaction");
        std::vector<std::uint8_t> d0(16);
        for (unsigned i = 0; i < d0.size(); ++i) d0[i] = std::uint8_t(0x20 + i);
        s.respond(r0, d0);
        auto f0 = s.receive();
        require(f0[0] == 0x10 && f0[kPayload] == 0x40,
                "read response is packed as AoU ReadData");
        bool lane_ok = true;
        for (unsigned lane = 0; lane < 32; ++lane) {
            const std::uint8_t expected = lane >= 16 ? d0[lane - 16] : 0;
            lane_ok &= f0[kPayload + 5 + (31 - lane)] == expected;
        }
        require(lane_ok, "compact read data is scattered back to AXI lanes");

        auto r1 = s.accept_request();
        require(r1.addr == 0x1020 && r1.bytes == 16,
                "second read beat advances by AxSIZE");
        std::vector<std::uint8_t> d1(16, 0x5a);
        s.respond(r1, d1);
        auto f1 = s.receive();
        require((f1[kPayload + 4] & 0x08) != 0, "last read beat carries RLAST");

        std::array<std::uint8_t, 32> lanes{};
        for (unsigned i = 0; i < lanes.size(); ++i) lanes[i] = std::uint8_t(i + 1);
        std::array<std::uint8_t, kPlpBytes> write_frame{};
        start(write_frame, 0); put(write_frame, 0, request(true, 0x3344, 7, 5, 0, 0x2000));
        start(write_frame, 3); put(write_frame, 3, write_full(0x7788, lanes));
        s.send(write_frame);
        auto w = s.accept_request();
        require(w.write && w.addr == 0x2000 && w.bytes == 32,
                "full write becomes one 32-byte MC transaction");
        bool write_ok = true;
        for (unsigned i = 0; i < 32; ++i)
            write_ok &= get_byte(s.d.mc_req_data, i) == lanes[i] &&
                        ((s.d.mc_req_byte_mask >> i) & 1U);
        require(write_ok, "write data and byte mask are compact and ordered");
        s.respond(w);
        auto wf = s.receive();
        require(wf[kPayload] == 0x50 && wf[kPayload + 1] == 0x33 &&
                    wf[kPayload + 2] == 0x44,
                "completed write returns AoU WriteResp");

        std::array<std::uint8_t, kPlpBytes> parallel_frame{};
        start(parallel_frame, 0);
        put(parallel_frame, 0, request(false, 0x4001, 0x21, 5, 0, 0x3000));
        start(parallel_frame, 3);
        put(parallel_frame, 3, request(false, 0x4002, 0x22, 5, 0, 0x3020));
        start(parallel_frame, 6);
        put(parallel_frame, 6, request(false, 0x4003, 0x23, 5, 0, 0x3040));
        s.send(parallel_frame);
        auto p0 = s.accept_request();
        auto p1 = s.accept_request();
        auto p2 = s.accept_request();
        require(p0.addr == 0x3000 && p1.addr == 0x3020 && p2.addr == 0x3040 &&
                    p0.tag != p1.tag && p1.tag != p2.tag && p0.tag != p2.tag,
                "three parent requests issue with distinct child tags before completion");

        std::vector<std::uint8_t> pd0(32, 0xa1);
        std::vector<std::uint8_t> pd1(32, 0xb2);
        std::vector<std::uint8_t> pd2(32, 0xc3);
        s.respond(p2, pd2);
        s.respond(p1, pd1);
        s.respond(p0, pd0);
        auto pf0 = s.receive();
        auto pf1 = s.receive();
        auto pf2 = s.receive();
        require(response_id(pf0) == 0x21 && response_id(pf1) == 0x22 &&
                    response_id(pf2) == 0x23,
                "out-of-order MC completions retire in parent acceptance order");
        require(pf0[kPayload + 36] == 0xa1 && pf1[kPayload + 36] == 0xb2 &&
                    pf2[kPayload + 36] == 0xc3,
                "reorder buffer keeps each read payload with its parent");

        std::array<std::uint8_t, kPlpBytes> saturation_a{};
        std::array<std::uint8_t, kPlpBytes> saturation_b{};
        // RP0 header grant: 128 ReadData granules, enough for all eight replies.
        saturation_a[4] = 0x00;
        saturation_a[5] = 0x0e;
        for (unsigned i = 0; i < 4; ++i) {
            start(saturation_a, 3 * i);
            put(saturation_a, 3 * i,
                request(false, std::uint16_t(0x5000 + i),
                        std::uint16_t(0x100 + i), 5, 0, 0x4000 + 32 * i));
            start(saturation_b, 3 * i);
            put(saturation_b, 3 * i,
                request(false, std::uint16_t(0x5004 + i),
                        std::uint16_t(0x104 + i), 5, 0, 0x4080 + 32 * i));
        }
        s.send(saturation_a);
        s.send(saturation_b);
        std::array<Sim::Txn, 8> full{};
        for (auto& txn : full) txn = s.accept_request();
        bool unique_tags = true;
        for (unsigned i = 0; i < full.size(); ++i) {
            unique_tags &= full[i].addr == 0x4000 + 32 * i;
            for (unsigned j = 0; j < i; ++j) unique_tags &= full[i].tag != full[j].tag;
        }
        require(unique_tags && s.d.busy,
                "MAX_OUTSTANDING=8 and MC_INFLIGHT=8 fill without early completion");
        for (int i = 7; i >= 0; --i)
            s.respond(full[unsigned(i)], std::vector<std::uint8_t>(32, std::uint8_t(0xd0 + i)));
        bool saturation_order = true;
        for (unsigned i = 0; i < full.size(); ++i) {
            auto frame = s.receive();
            saturation_order &= response_id(frame) == 0x100 + i;
            saturation_order &= frame[kPayload + 36] == std::uint8_t(0xd0 + i);
        }
        require(saturation_order,
                "eight reverse-order completions retire with ordered IDs and payloads");

        std::array<std::uint8_t, 32> write_a{};
        std::array<std::uint8_t, 32> write_b{};
        for (unsigned i = 0; i < 32; ++i) {
            write_a[i] = std::uint8_t(0x20 + i);
            write_b[i] = std::uint8_t(0x80 + i);
        }
        std::array<std::uint8_t, kPlpBytes> parallel_writes{};
        // RP0 header grant: 8 WriteResp granules.
        parallel_writes[4] = 0x00;
        parallel_writes[5] = 0x30;
        start(parallel_writes, 0);
        put(parallel_writes, 0, request(true, 0x6101, 0x181, 5, 0, 0x5000));
        start(parallel_writes, 3);
        put(parallel_writes, 3, request(true, 0x6102, 0x182, 5, 0, 0x5020));
        start(parallel_writes, 6);
        put(parallel_writes, 6, write_full(0x7101, write_a));
        start(parallel_writes, 13);
        put(parallel_writes, 13, write_full(0x7102, write_b));
        s.send(parallel_writes);
        auto pw0 = s.accept_request();
        auto pw1 = s.accept_request();
        bool write_pairing = pw0.write && pw1.write && pw0.mask == 0xffffffffU &&
                             pw1.mask == 0xffffffffU;
        const auto& got_a = pw0.addr == 0x5000 ? pw0 : pw1;
        const auto& got_b = pw0.addr == 0x5020 ? pw0 : pw1;
        for (unsigned i = 0; i < 32; ++i)
            write_pairing &= got_a.data[i] == write_a[i] && got_b.data[i] == write_b[i];
        require(write_pairing,
                "WriteData messages pair with two WriteReq parents in receive order");
        s.respond(pw1);
        s.respond(pw0);
        auto pwr0 = s.receive();
        auto pwr1 = s.receive();
        require(response_id(pwr0) == 0x181 && response_id(pwr1) == 0x182,
                "reverse-order MC writes return ordered WriteResp IDs");

        std::array<std::uint8_t, kPlpBytes> early_data{};
        start(early_data, 0); put(early_data, 0, write_full(0x7201, write_a));
        start(early_data, 7); put(early_data, 7, write_full(0x7202, write_b));
        s.send(early_data);
        std::array<std::uint8_t, kPlpBytes> late_requests{};
        start(late_requests, 0);
        put(late_requests, 0, request(true, 0x6201, 0x191, 5, 0, 0x5100));
        start(late_requests, 3);
        put(late_requests, 3, request(true, 0x6202, 0x192, 5, 0, 0x5120));
        s.send(late_requests);
        auto ew0 = s.accept_request();
        auto ew1 = s.accept_request();
        bool early_pairing = ew0.addr == 0x5100 && ew1.addr == 0x5120;
        for (unsigned i = 0; i < 32; ++i)
            early_pairing &= ew0.data[i] == write_a[i] && ew1.data[i] == write_b[i];
        require(early_pairing,
                "WData FIFO accepts data before WriteReq without RX head-of-line deadlock");
        s.respond(ew1);
        s.respond(ew0);
        auto ewr0 = s.receive();
        auto ewr1 = s.receive();
        require(response_id(ewr0) == 0x191 && response_id(ewr1) == 0x192,
                "early WData requests retain ordered WriteResp IDs");
#ifdef TEST_RP2
        std::array<std::uint8_t, 32> rp0_data{};
        std::array<std::uint8_t, 32> rp1_data{};
        for (unsigned i = 0; i < 32; ++i) {
            rp0_data[i] = std::uint8_t(0x30 + i);
            rp1_data[i] = std::uint8_t(0xa0 + i);
        }
        std::array<std::uint8_t, kPlpBytes> cross_rp{};
        // RP1 gets WriteResp credit; RP0 intentionally has WDATA but no request.
        cross_rp[4] = 0x00;
        cross_rp[5] = 0x50;
        start(cross_rp, 0); put(cross_rp, 0, write_full(0x7300, rp0_data, 0));
        start(cross_rp, 7); put(cross_rp, 7, write_full(0x7301, rp1_data, 1));
        start(cross_rp, 14);
        put(cross_rp, 14, request(true, 0x6301, 0x1a1, 5, 0, 0x5200, 1));
        s.send(cross_rp);
        auto rp1_write = s.accept_request();
        bool rp1_pairing = rp1_write.write && rp1_write.addr == 0x5200;
        for (unsigned i = 0; i < 32; ++i)
            rp1_pairing &= rp1_write.data[i] == rp1_data[i];
        require(rp1_pairing,
                "RP1 WDATA bypasses an unmatched RP0 WDATA queue head");
        s.respond(rp1_write);
        auto rp1_rsp = s.receive();
        require((rp1_rsp[kPayload] & 0x0c) == 0x04 &&
                    response_id(rp1_rsp) == 0x1a1,
                "RP1 write returns a response on RP1");
#endif
        require(!s.d.protocol_error, "closed-loop traffic has no protocol error");
        std::cout << "RTL closed-loop regression passed in " << s.cycles << " cycles\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
