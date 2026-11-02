// ucie_phy.h - behavioral PHY: serialization/lane striping, PAM4/NRZ encode,
// channel impairments (ISI FIR + AWGN + jitter), threshold decision, lane skew,
// CDR lock abstraction. Plain C++, driven by the SystemC channel module.
#pragma once

#include <algorithm>
#include <array>
#include <random>
#include <vector>

#include "ucie_common.h"

struct PhyResult {
    std::vector<std::uint8_t> rx_bytes;
    std::uint64_t total_symbols = 0;
    std::uint64_t symbol_errors = 0;
    std::uint64_t bit_errors = 0;
    std::uint32_t max_skew_ui = 0;
    bool deskew_failure = false;
    bool extra_error = false;
    bool cdr_locked = true;
};

class BehavioralPhy {
public:
    using LaneLevels = std::vector<std::vector<int>>;

    explicit BehavioralPhy(const Config& cfg)
        : cfg_(cfg),
          rng_(cfg.seed),
          gauss_(0.0, 1.0),
          unit_(0.0, 1.0),
          skew_dist_(0, cfg.lane_skew_max_ui) {}

    // Transmit one flit worth of bytes through the impaired link.
    // now_ui is the TX start time in UI (used for CDR lock state).
    PhyResult transmit(const std::vector<std::uint8_t>& tx_bytes, double now_ui) {
        PhyResult r;
        r.cdr_locked = is_cdr_locked(now_ui);

        const std::uint32_t lanes = cfg_.num_lanes;
        const std::uint32_t bps = cfg_.bits_per_ui();   // bits per symbol (2=PAM4, 1=NRZ)

        // UCIe-style byte-granular round-robin mapping: byte i -> lane i % lanes,
        // sent MSB-first as 8/bps consecutive symbols on that lane.
        const LaneLevels lane_levels = stripe_bytes(tx_bytes, lanes, bps);

        // Per-lane random skew; deskew failure if beyond RX tolerance.
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            const std::uint32_t skew = skew_dist_(rng_);
            r.max_skew_ui = std::max(r.max_skew_ui, skew);
            if (skew > cfg_.deskew_depth_ui) r.deskew_failure = true;
        }

        const double sigma = r.cdr_locked
                                 ? cfg_.awgn_sigma
                                 : cfg_.awgn_sigma * cfg_.cdr_unlocked_noise_multiplier;

        // Channel + decision per lane symbol stream.
        std::vector<std::vector<int>> lane_rx(lanes);
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            const auto& lv = lane_levels[lane];
            lane_rx[lane].resize(lv.size());
            for (std::size_t k = 0; k < lv.size(); ++k) {
                const double x0 = static_cast<double>(lv[k]);
                const double x1 = (k >= 1) ? static_cast<double>(lv[k - 1]) : 0.0;
                const double x2 = (k >= 2) ? static_cast<double>(lv[k - 2]) : 0.0;
                const double jitter_ui = cfg_.jitter_sigma_ui * gauss_(rng_);
                double y = cfg_.isi_h0 * x0 + cfg_.isi_h1 * x1 + cfg_.isi_h2 * x2;
                y += sigma * gauss_(rng_);
                y += cfg_.jitter_isi_gain * jitter_ui * (x0 - x1);

                const int decided = (bps == 2U) ? pam4_decide(y) : nrz_decide(y);
                lane_rx[lane][k] = decided;
                ++r.total_symbols;
                if (decided != lv[k]) ++r.symbol_errors;
            }
        }

        // De-stripe back to bytes.
        r.rx_bytes = destripe_bytes(lane_rx, tx_bytes.size(), lanes, bps);

        if (r.deskew_failure) flip_random_bit(r.rx_bytes);
        if (unit_(rng_) < cfg_.extra_flit_error_rate) {
            r.extra_error = true;
            flip_random_bit(r.rx_bytes);
        }

        for (std::size_t i = 0; i < tx_bytes.size(); ++i) {
            r.bit_errors += static_cast<std::uint64_t>(
                __builtin_popcount(static_cast<unsigned>(tx_bytes[i] ^ r.rx_bytes[i])));
        }
        return r;
    }

    // RX-side CDR observation: sustained bad frames -> lock loss + relock window.
    void observe(double arrival_ui, const PhyResult& r) {
        const double frac = r.total_symbols == 0
                                ? 0.0
                                : static_cast<double>(r.symbol_errors) /
                                      static_cast<double>(r.total_symbols);
        if (frac > cfg_.cdr_loss_error_fraction) {
            ++bad_streak_;
        } else {
            bad_streak_ = 0;
        }
        if (bad_streak_ >= cfg_.cdr_loss_bad_flit_threshold) {
            ++lock_loss_count_;
            relock_until_ui_ = arrival_ui + static_cast<double>(cfg_.cdr_relock_ui);
            bad_streak_ = 0;
        }
    }

    bool is_cdr_locked(double now_ui) const { return now_ui >= relock_until_ui_; }
    std::uint64_t lock_loss_count() const { return lock_loss_count_; }

public:
    static LaneLevels stripe_bytes(const std::vector<std::uint8_t>& bytes,
                                   std::uint32_t lanes, std::uint32_t bits_per_symbol) {
        if (lanes == 0U || (bits_per_symbol != 1U && bits_per_symbol != 2U)) {
            throw std::runtime_error("invalid lane mapping geometry");
        }
        LaneLevels levels(lanes);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const std::uint32_t lane = static_cast<std::uint32_t>(i % lanes);
            for (int bit = 8 - static_cast<int>(bits_per_symbol); bit >= 0;
                 bit -= static_cast<int>(bits_per_symbol)) {
                if (bits_per_symbol == 2U) {
                    levels[lane].push_back(pam4_encode((bytes[i] >> (bit + 1)) & 1U,
                                                       (bytes[i] >> bit) & 1U));
                } else {
                    levels[lane].push_back(((bytes[i] >> bit) & 1U) ? 1 : -1);
                }
            }
        }
        return levels;
    }

    static std::vector<std::uint8_t> destripe_bytes(const LaneLevels& levels,
                                                    std::size_t byte_count,
                                                    std::uint32_t lanes,
                                                    std::uint32_t bits_per_symbol) {
        if (lanes == 0U || levels.size() != lanes ||
            (bits_per_symbol != 1U && bits_per_symbol != 2U)) {
            throw std::runtime_error("invalid lane de-mapping geometry");
        }
        std::vector<std::uint8_t> bytes(byte_count, 0U);
        std::vector<std::size_t> lane_pos(lanes, 0U);
        const int symbols_per_byte = 8 / static_cast<int>(bits_per_symbol);
        for (std::size_t i = 0; i < byte_count; ++i) {
            const std::uint32_t lane = static_cast<std::uint32_t>(i % lanes);
            std::uint8_t byte = 0;
            for (int symbol = 0; symbol < symbols_per_byte; ++symbol) {
                if (lane_pos[lane] >= levels[lane].size()) {
                    throw std::runtime_error("lane stream is shorter than requested byte count");
                }
                const int level = levels[lane][lane_pos[lane]++];
                if (bits_per_symbol == 2U) {
                    const auto bits = pam4_bits(level);
                    byte = static_cast<std::uint8_t>((byte << 2U) |
                                                     (bits[0] << 1U) | bits[1]);
                } else {
                    byte = static_cast<std::uint8_t>((byte << 1U) |
                                                     (level > 0 ? 1U : 0U));
                }
            }
            bytes[i] = byte;
        }
        return bytes;
    }

    static int pam4_encode(std::uint8_t b0, std::uint8_t b1) {
        // Gray mapping: 00 -> -3, 01 -> -1, 11 -> +1, 10 -> +3
        if (b0 == 0 && b1 == 0) return -3;
        if (b0 == 0 && b1 == 1) return -1;
        if (b0 == 1 && b1 == 1) return 1;
        return 3;
    }
    static int pam4_decide(double y) {
        if (y < -2.0) return -3;
        if (y < 0.0) return -1;
        if (y < 2.0) return 1;
        return 3;
    }
    static std::array<std::uint8_t, 2> pam4_bits(int lvl) {
        if (lvl == -3) return {0, 0};
        if (lvl == -1) return {0, 1};
        if (lvl == 1) return {1, 1};
        return {1, 0};
    }
    static int nrz_decide(double y) { return y < 0.0 ? -1 : 1; }

private:
    void flip_random_bit(std::vector<std::uint8_t>& bytes) {
        if (bytes.empty()) return;
        std::uniform_int_distribution<std::size_t> d(0, bytes.size() * 8U - 1U);
        const std::size_t idx = d(rng_);
        bytes[idx / 8U] ^= static_cast<std::uint8_t>(1U << (idx % 8U));
    }

    const Config& cfg_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> gauss_;
    std::uniform_real_distribution<double> unit_;
    std::uniform_int_distribution<std::uint32_t> skew_dist_;

    double relock_until_ui_ = 0.0;
    std::uint32_t bad_streak_ = 0;
    std::uint64_t lock_loss_count_ = 0;
};
