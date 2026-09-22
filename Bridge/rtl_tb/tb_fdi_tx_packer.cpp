#include "Vfdi_tx_packer.h"
#include "verilated.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
constexpr unsigned kPlpBytes = 250;

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
    std::cout << "[PASS] " << what << '\n';
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

class Sim {
public:
    Sim() {
        d.clk = 0;
        d.rst_n = 0;
        d.link_active = 1;
        d.msg_valid = 0;
        d.credit_valid = 0;
        d.fdi_tx_ready = 1;
        for (unsigned n = 0; n < 38; ++n) d.msg_data[n] = 0;
        tick();
        tick();
        d.rst_n = 1;
        tick();
    }
    void tick() {
        d.clk = 0;
        d.eval();
        d.clk = 1;
        d.eval();
    }
    void set_message(unsigned granules, std::uint8_t base) {
        for (unsigned n = 0; n < 38; ++n) d.msg_data[n] = 0;
        for (unsigned n = 0; n < granules * 5; ++n)
            set_byte(d.msg_data, n, std::uint8_t(base + n));
        d.msg_granules = granules;
    }
    Vfdi_tx_packer d;
};
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        {
            Sim s;
            s.set_message(1, 0x10);
            s.d.credit_field = 0x4567;
            s.d.credit_valid = 1;
            s.d.msg_valid = 1;
            s.tick();
            s.d.credit_valid = 0;
            s.set_message(1, 0x20);
            s.tick();
            s.d.msg_valid = 0;
            s.tick();
            require(s.d.fdi_tx_valid, "consecutive messages flush as one PLP");
            require(get_byte(s.d.fdi_tx_plp, 0) == 0x30,
                    "aggregated PLP marks message starts at granules 0 and 1");
            bool payload_ok = true;
            for (unsigned n = 0; n < 5; ++n) {
                payload_ok &= get_byte(s.d.fdi_tx_plp, 10 + n) ==
                              std::uint8_t(0x10 + n);
                payload_ok &= get_byte(s.d.fdi_tx_plp, 15 + n) ==
                              std::uint8_t(0x20 + n);
            }
            require(payload_ok, "aggregated message bytes retain granule order");
            require(get_byte(s.d.fdi_tx_plp, 4) == 0x67 &&
                        get_byte(s.d.fdi_tx_plp, 5) == 0x45,
                    "credit is piggybacked on an aggregated data PLP");

            s.d.fdi_tx_ready = 0;
            std::array<std::uint8_t, kPlpBytes> snapshot{};
            for (unsigned n = 0; n < kPlpBytes; ++n)
                snapshot[n] = get_byte(s.d.fdi_tx_plp, n);
            s.tick();
            bool stable = s.d.fdi_tx_valid;
            for (unsigned n = 0; n < kPlpBytes; ++n)
                stable &= snapshot[n] == get_byte(s.d.fdi_tx_plp, n);
            require(stable, "aggregated PLP remains stable under TX backpressure");
        }

        {
            Sim s;
            s.set_message(27, 0x00);
            s.d.msg_valid = 1;
            s.tick();
            s.set_message(27, 0x80);
            s.tick();
            require(s.d.fdi_tx_valid, "full PLP is emitted when a message spans");
            require(get_byte(s.d.fdi_tx_plp, 0) == 0x10 &&
                        (get_byte(s.d.fdi_tx_plp, 6) & 0x80) != 0,
                    "spanning message start is marked at granule 27");
            bool first_fragment_ok = true;
            for (unsigned n = 0; n < 27 * 5; ++n)
                first_fragment_ok &= get_byte(s.d.fdi_tx_plp, 10 + n) ==
                                     std::uint8_t(n);
            for (unsigned n = 0; n < 21 * 5; ++n)
                first_fragment_ok &= get_byte(s.d.fdi_tx_plp, 10 + 27 * 5 + n) ==
                                     std::uint8_t(0x80 + n);
            require(first_fragment_ok, "first spanning fragment fills remaining PLP space");

            s.d.fdi_tx_ready = 0;
            s.d.msg_valid = 0;
            s.tick();
            s.d.fdi_tx_ready = 1;
            s.tick();
            require(s.d.fdi_tx_valid, "spill continuation is emitted after prior PLP");
            bool continuation_ok = true;
            for (unsigned n = 0; n < 10; ++n)
                continuation_ok &= get_byte(s.d.fdi_tx_plp, n) == 0;
            for (unsigned n = 0; n < 6 * 5; ++n)
                continuation_ok &= get_byte(s.d.fdi_tx_plp, 10 + n) ==
                                   std::uint8_t(0x80 + 21 * 5 + n);
            require(continuation_ok,
                    "spill continuation has no new MsgStart and preserves remaining bytes");
        }
        std::cout << "RTL TX aggregation/spill regression passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
