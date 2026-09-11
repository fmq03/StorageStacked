// Deterministic unit tests for protocol primitives and the behavioral PHY.
#include <systemc.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ucie_common.h"
#include "ucie_fdi.h"
#include "ucie_phy.h"

namespace {

int pass_count = 0;
int fail_count = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        ++pass_count;
        std::cout << "PASS  " << name << "\n";
    } else {
        ++fail_count;
        std::cout << "FAIL  " << name << "\n";
    }
}

template <typename F>
void check_throws(F&& fn, const std::string& name) {
    try {
        fn();
        check(false, name);
    } catch (const std::exception&) {
        check(true, name);
    }
}

PhyResult make_phy_result(std::uint64_t errors, std::uint64_t total = 100U) {
    PhyResult result;
    result.symbol_errors = errors;
    result.total_symbols = total;
    return result;
}

}  // namespace

int sc_main(int, char**) {
    const std::string canonical = "123456789";
    check(crc16_ccitt(reinterpret_cast<const std::uint8_t*>(canonical.data()),
                      canonical.size()) == 0x29B1U,
          "CRC-16/CCITT-FALSE golden vector 123456789 -> 0x29B1");

    Config standard;
    standard.flit_format = FlitFormat::Standard256;
    std::vector<std::uint8_t> standard_payload(standard.payload_bytes());
    for (std::size_t i = 0; i < standard_payload.size(); ++i) {
        standard_payload[i] = static_cast<std::uint8_t>(i);
    }
    std::vector<std::uint8_t> standard_flit =
        build_flit(standard, 0x1FFU, standard_payload, true);
    const FlitCheck standard_check = check_flit(standard, standard_flit);
    check(standard_flit.size() == 256U, "Standard flit size is 256 bytes");
    check(standard_flit[0] == 0xFFU && standard_flit[1] == 0x01U,
          "Standard header carries seq8 and replay flag");
    check(standard_check.crc_ok && standard_check.seq8 == 0xFFU &&
              standard_check.replay_flag,
          "Standard flit CRC and header decode");
    standard_flit[17] ^= 0x04U;
    check(!check_flit(standard, standard_flit).crc_ok,
          "Standard CRC detects a fixed payload bit flip");

    Config compact;
    compact.flit_format = FlitFormat::Compact68;
    std::vector<std::uint8_t> compact_payload(compact.payload_bytes(), 0xA5U);
    std::vector<std::uint8_t> compact_flit = build_flit(compact, 7U, compact_payload, false);
    check(compact_flit.size() == 68U && check_flit(compact, compact_flit).crc_ok,
          "Compact 68-byte flit construction and CRC");
    compact_flit[67] ^= 0x01U;
    check(!check_flit(compact, compact_flit).crc_ok,
          "Compact CRC detects a fixed CRC bit flip");

    const std::uint64_t wrap_seq[] = {254U, 255U, 256U, 257U};
    const std::uint8_t wrap_expected[] = {254U, 255U, 0U, 1U};
    bool wrap_ok = true;
    for (std::size_t i = 0; i < 4U; ++i) {
        const auto frame = build_flit(compact, wrap_seq[i], compact_payload, false);
        wrap_ok = wrap_ok && check_flit(compact, frame).seq8 == wrap_expected[i];
    }
    check(wrap_ok, "8-bit wire sequence wraps 254,255,0,1");

    check(BehavioralPhy::pam4_encode(0, 0) == -3 &&
              BehavioralPhy::pam4_encode(0, 1) == -1 &&
              BehavioralPhy::pam4_encode(1, 1) == 1 &&
              BehavioralPhy::pam4_encode(1, 0) == 3,
          "PAM4 Gray encoding golden mapping");
    check(BehavioralPhy::pam4_decide(-2.01) == -3 &&
              BehavioralPhy::pam4_decide(-2.0) == -1 &&
              BehavioralPhy::pam4_decide(0.0) == 1 &&
              BehavioralPhy::pam4_decide(2.0) == 3,
          "PAM4 decision threshold boundaries");
    check(BehavioralPhy::nrz_decide(-0.001) == -1 &&
              BehavioralPhy::nrz_decide(0.0) == 1,
          "NRZ zero-threshold boundaries");

    const std::vector<std::uint8_t> lane_bytes = {0x1BU, 0xE4U, 0x55U, 0xAAU};
    const auto pam4_lanes = BehavioralPhy::stripe_bytes(lane_bytes, 2U, 2U);
    const std::vector<int> lane0_expected = {-3, -1, 3, 1, -1, -1, -1, -1};
    const std::vector<int> lane1_expected = {1, 3, -1, -3, 3, 3, 3, 3};
    check(pam4_lanes.size() == 2U && pam4_lanes[0] == lane0_expected &&
              pam4_lanes[1] == lane1_expected,
          "Byte-round-robin lane striping golden vector");
    check(BehavioralPhy::destripe_bytes(pam4_lanes, lane_bytes.size(), 2U, 2U) ==
              lane_bytes,
          "PAM4 lane de-striping restores bytes");
    const auto nrz_lanes = BehavioralPhy::stripe_bytes(lane_bytes, 3U, 1U);
    check(BehavioralPhy::destripe_bytes(nrz_lanes, lane_bytes.size(), 3U, 1U) ==
              lane_bytes,
          "NRZ lane striping/de-striping restores bytes");

    Config cdr_cfg;
    BehavioralPhy cdr(cdr_cfg);
    const PhyResult bad = make_phy_result(11U);
    const PhyResult good = make_phy_result(0U);
    cdr.observe(0.0, bad);
    cdr.observe(1.0, bad);
    cdr.observe(2.0, bad);
    check(cdr.is_cdr_locked(3.0) && cdr.lock_loss_count() == 0U,
          "CDR remains locked after three bad flits");
    cdr.observe(3.0, bad);
    check(!cdr.is_cdr_locked(514.0) && cdr.lock_loss_count() == 1U,
          "CDR loses lock after configured fourth bad flit");
    check(cdr.is_cdr_locked(515.0), "CDR relocks after exactly 512 UI");

    BehavioralPhy cdr_reset(cdr_cfg);
    cdr_reset.observe(0.0, bad);
    cdr_reset.observe(1.0, bad);
    cdr_reset.observe(2.0, good);
    cdr_reset.observe(3.0, bad);
    cdr_reset.observe(4.0, bad);
    cdr_reset.observe(5.0, bad);
    check(cdr_reset.lock_loss_count() == 0U,
          "A good flit resets the CDR consecutive-bad counter");

    Config deskew_cfg;
    deskew_cfg.num_lanes = 8U;
    deskew_cfg.lane_skew_max_ui = 4U;
    deskew_cfg.deskew_depth_ui = 2U;
    deskew_cfg.awgn_sigma = 0.0;
    deskew_cfg.jitter_sigma_ui = 0.0;
    deskew_cfg.isi_h1 = 0.0;
    deskew_cfg.isi_h2 = 0.0;
    BehavioralPhy deskew_phy(deskew_cfg);
    bool saw_deskew_failure = false;
    for (unsigned i = 0; i < 32U; ++i) {
        saw_deskew_failure = saw_deskew_failure ||
                             deskew_phy.transmit(compact_payload, i * 100.0).deskew_failure;
    }
    check(saw_deskew_failure, "Deskew limit violation is observable");

    Config invalid = standard;
    invalid.retry_buffer_size = 128U;
    check_throws([&] { require_valid_config(invalid); },
                 "Retry window rejects the ambiguous 128-flit size");
    invalid = standard;
    invalid.fdi_queue_size = 0U;
    check_throws([&] { require_valid_config(invalid); },
                 "Zero-depth public FDI queue is rejected");

    FdiFlit public_flit;
    public_flit.payload = compact_payload;
    public_flit.valid_bytes = 4U;
    public_flit.transaction_id = 42U;
    public_flit.vc = 3U;
    public_flit.kind = BusinessKind::Response;
    check(public_flit.payload.size() == compact.payload_bytes() &&
              public_flit.valid_bytes == 4U && public_flit.kind == BusinessKind::Response,
          "Public FdiFlit carries payload, valid length, VC, ID, and kind");

    std::cout << "\n==============================\n"
              << "UNIT_PASS=" << pass_count << " UNIT_FAIL=" << fail_count << "\n"
              << "==============================\n";
    return fail_count == 0 ? 0 : 1;
}
