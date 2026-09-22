#include "Vbridge_credit_mgr.h"
#include "verilated.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
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

class Sim {
public:
    Sim() {
        d.clk = 0;
        d.rst_n = 0;
        d.link_active = 1;
        d.grant_ready = 0;
        d.req_release0_valid = 0;
        d.req_release1_valid = 0;
        d.wdata_release_valid = 0;
        d.header_credit_valid = 0;
        d.misc_valid = 0;
        d.rsp_valid = 0;
        d.rsp_fire = 0;
        for (unsigned n = 0; n < 38; ++n) d.misc_data[n] = 0;
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

    Vbridge_credit_mgr d;
};
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Sim s;
        s.d.grant_ready = 1;
        std::array<unsigned, 4> rp_sequence{};
        for (unsigned n = 0; n < rp_sequence.size(); ++n) {
            s.d.eval();
            require(s.d.grant_valid, "initial per-RP credit grant is present");
            rp_sequence[n] = (s.d.grant_field >> 14) & 3U;
            s.tick();
        }
        require(rp_sequence == std::array<unsigned, 4>{0, 1, 0, 1},
                "header credit grants round-robin across RP0 and RP1");
        s.d.eval();
        require(!s.d.grant_valid, "all initial RP capacities are published");

        // Header credit belongs only to RP1: RDATA=8 and WRESP=1.
        s.d.header_credit = std::uint16_t((1U << 14) | (1U << 12) | (3U << 9));
        s.d.header_credit_valid = 1;
        s.tick();
        s.d.header_credit_valid = 0;
        s.d.rsp_valid = 1;
        s.d.rsp_type = 4;
        s.d.rsp_granules = 8;
        s.d.rsp_rp = 0;
        s.d.eval();
        require(!s.d.rsp_credit_ok, "RP0 cannot consume RP1 header credit");
        s.d.rsp_rp = 1;
        s.d.eval();
        require(s.d.rsp_credit_ok, "RP1 consumes its own header credit");
        s.d.rsp_fire = 1;
        s.tick();
        s.d.rsp_fire = 0;
        s.d.eval();
        require(!s.d.rsp_credit_ok, "RP1 header credit is decremented independently");
        s.d.rsp_valid = 0;

        const std::array<std::uint8_t, 10> grant{
            0x08, 0x88, 0x0c, 0x01, 0x38, 0x17, 0x00, 0xc0, 0x00, 0x00};
        for (unsigned n = 0; n < grant.size(); ++n)
            set_byte(s.d.misc_data, n, grant[n]);
        s.d.misc_valid = 1;
        s.tick();
        s.d.misc_valid = 0;
        require(s.d.debug_available_rdata == 32,
                "CrdtGrant decodes RP0 ReadData credit");
        require(s.d.debug_available_wresp == 1,
                "CrdtGrant decodes RP0 WriteResp credit");
        s.d.rsp_valid = 1;
        s.d.rsp_rp = 1;
        s.d.rsp_type = 4;
        s.d.rsp_granules = 32;
        s.d.eval();
        require(s.d.rsp_credit_ok, "CrdtGrant decodes RP1 ReadData credit");
        s.d.rsp_type = 5;
        s.d.rsp_granules = 4;
        s.d.eval();
        require(s.d.rsp_credit_ok, "CrdtGrant decodes RP1 WriteResp credit");
        s.d.rsp_valid = 0;

        s.d.req_release0_valid = 1;
        s.d.req_release0_write = 0;
        s.d.req_release0_rp = 1;
        s.d.req_release0_granules = 3;
        s.tick();
        s.d.req_release0_valid = 0;
        s.d.eval();
        require(s.d.grant_valid && ((s.d.grant_field >> 14) & 3U) == 1 &&
                    ((s.d.grant_field >> 3) & 7U) != 0,
                "released request credit is returned on the matching RP");
        require(!s.d.error, "multi-RP credit traffic has no accounting error");
        std::cout << "RTL multi-RP credit regression passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
