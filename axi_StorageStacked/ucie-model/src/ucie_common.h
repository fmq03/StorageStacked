// ucie_common.h - shared config, flit format, CRC, stats for the UCIe SystemC model.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "aou_format6.h"

// ---------------------------------------------------------------------------
// Modulation / flit format enums
// ---------------------------------------------------------------------------
enum class Modulation { PAM4, NRZ };
enum class FlitFormat { Standard256, Compact68, AouFormat6 };

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config {
    // Workload
    std::uint64_t flit_count = 2000;
    FlitFormat flit_format = FlitFormat::Standard256;
    std::string payload_file;              // empty = deterministic internal workload
    std::string response_file;             // optional returned-payload dump
    std::uint32_t memory_delay_ui = 32;     // fixed response-generation latency

    // Link geometry / rate
    std::uint32_t num_lanes = 16;
    double lane_rate_gtps = 24.0;
    Modulation modulation = Modulation::PAM4;

    // Queues / buffers
    std::uint32_t fdi_queue_size = 64;
    std::uint32_t retry_buffer_size = 64;   // max unacknowledged flits (<=127)

    // Pipeline delays in UI
    std::uint32_t tx_pipe_ui = 4;
    std::uint32_t rx_pipe_ui = 4;
    std::uint32_t channel_ui = 8;
    std::uint32_t feedback_ui = 32;
    std::uint32_t train_ui = 512;           // link training / initial CDR lock

    // FDI pacing: 0 = auto (one flit per serialization slot)
    std::uint32_t fdi_interval_ui = 0;

    // Channel impairments
    double awgn_sigma = 0.05;
    double jitter_sigma_ui = 0.01;
    double jitter_isi_gain = 0.40;
    double isi_h0 = 1.0;
    double isi_h1 = 0.08;
    double isi_h2 = 0.03;
    std::uint32_t lane_skew_max_ui = 2;
    std::uint32_t deskew_depth_ui = 8;
    double extra_flit_error_rate = 0.0;

    // CDR abstraction
    std::uint32_t cdr_relock_ui = 512;
    double cdr_loss_error_fraction = 0.10;
    std::uint32_t cdr_loss_bad_flit_threshold = 4;
    double cdr_unlocked_noise_multiplier = 4.0;

    // RX NAK suppression: re-send NAK for same expected seq after this many frames
    std::uint32_t nak_repeat_frames = 8;

    // Misc
    std::uint32_t seed = 7;
    std::uint64_t max_time_ui = 50'000'000;
    bool verbose = false;
    std::string csv_path;

    // ---- derived ----
    std::uint32_t bits_per_ui() const {
        return modulation == Modulation::PAM4 ? 2U : 1U;
    }
    std::uint32_t flit_bytes() const {
        return flit_format == FlitFormat::Compact68 ? 68U : 256U;
    }
    std::uint32_t payload_bytes() const {
        // Standard 256B: 2B flit hdr + 236B TLP + 4B DLLP + 10B reserved + 4B CRC
        // Compact 68B  : 2B flit hdr + 64B data + 2B CRC
        return flit_format == FlitFormat::AouFormat6 ? 250U :
               flit_format == FlitFormat::Standard256 ? 236U : 64U;
    }
    std::uint32_t flit_bits() const { return flit_bytes() * 8U; }
    std::uint32_t serialize_ui() const {
        const std::uint32_t per_ui = num_lanes * bits_per_ui();
        return (flit_bits() + per_ui - 1U) / per_ui;
    }
    double ui_fs() const { return 1.0e6 / lane_rate_gtps; }  // femtoseconds per UI
    double flit_efficiency() const {
        return static_cast<double>(payload_bytes()) / static_cast<double>(flit_bytes());
    }
};

inline void require_valid_config(const Config& cfg) {
    if (cfg.num_lanes == 0) throw std::runtime_error("num_lanes must be positive");
    if (cfg.lane_rate_gtps <= 0.0) throw std::runtime_error("lane_rate_gtps must be positive");
    if (cfg.fdi_queue_size == 0) throw std::runtime_error("fdi_queue_size must be positive");
    if (cfg.retry_buffer_size == 0 || cfg.retry_buffer_size > 127)
        throw std::runtime_error("retry_buffer_size must be in [1,127] (7-bit seq window)");
    if (cfg.flit_count == 0) throw std::runtime_error("flit_count must be positive");
    if (cfg.cdr_loss_bad_flit_threshold == 0)
        throw std::runtime_error("cdr_loss_bad_flit_threshold must be positive");
    if (cfg.nak_repeat_frames == 0)
        throw std::runtime_error("nak_repeat_frames must be positive");
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
inline void print_help(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "UCIe SystemC link simulator: FDI + D2D Adapter + behavioral PHY.\n"
        << "\n"
        << "  --flits N            request/response transactions (default 2000)\n"
        << "  --payload-file PATH  external FDI payloads, one hex payload per line\n"
        << "  --response-file PATH write returned payloads as transaction_id,vc,hex\n"
        << "  --memory-delay N     memory-side response latency in UI (default 32)\n"
        << "  --format F           standard256 | compact68 | aou256 (default standard256)\n"
        << "  --lanes N            number of mainband lanes (default 16)\n"
        << "  --rate-gtps X        per-lane rate in GT/s (default 24)\n"
        << "  --mod M              modulation: pam4 | nrz (default pam4)\n"
        << "  --fdi-queue N        FDI TX queue depth (default 64)\n"
        << "  --retry-buffer N     retry buffer depth in flits, <=127 (default 64)\n"
        << "  --tx-delay N         TX SerDes pipeline delay in UI (default 4)\n"
        << "  --rx-delay N         RX SerDes pipeline delay in UI (default 4)\n"
        << "  --channel-delay N    channel propagation delay in UI (default 8)\n"
        << "  --feedback-delay N   ACK/NAK feedback delay in UI (default 32)\n"
        << "  --train N            link training time in UI (default 512)\n"
        << "  --fdi-interval N     FDI injection interval in UI, 0=auto (default 0)\n"
        << "  --sigma X            AWGN sigma on symbol levels (default 0.05)\n"
        << "  --jitter X           sampling jitter sigma in UI (default 0.01)\n"
        << "  --isi1 X             first post-cursor ISI tap (default 0.08)\n"
        << "  --isi2 X             second post-cursor ISI tap (default 0.03)\n"
        << "  --skew N             max random lane skew in UI (default 2)\n"
        << "  --deskew N           RX deskew tolerance in UI (default 8)\n"
        << "  --extra-flit-error X extra flit error injection probability (default 0)\n"
        << "  --cdr-relock N      CDR relock duration in UI (default 512)\n"
        << "  --cdr-ser X         CDR bad-flit SER threshold (default 0.10)\n"
        << "  --cdr-bad-flits N   consecutive bad flits causing lock loss (default 4)\n"
        << "  --cdr-noise-mult X  unlocked-state noise multiplier (default 4)\n"
        << "  --nak-repeat N      repeated NAK interval in frames (default 8)\n"
        << "  --seed N             random seed (default 7)\n"
        << "  --max-time N         stop after N UI of simulated time\n"
        << "  --csv PATH           write summary metrics CSV\n"
        << "  --verbose            print selected link events\n"
        << "  --help               show this help\n";
}

inline Config parse_args(int argc, char** argv) {
    Config cfg;
    auto need = [&](int& i) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + argv[i]);
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { print_help(argv[0]); std::exit(0); }
        else if (a == "--flits") cfg.flit_count = std::stoull(need(i));
        else if (a == "--payload-file") cfg.payload_file = need(i);
        else if (a == "--response-file") cfg.response_file = need(i);
        else if (a == "--memory-delay")
            cfg.memory_delay_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--format") {
            const std::string v = need(i);
            if (v == "standard256") cfg.flit_format = FlitFormat::Standard256;
            else if (v == "compact68") cfg.flit_format = FlitFormat::Compact68;
            else if (v == "aou256") cfg.flit_format = FlitFormat::AouFormat6;
            else throw std::runtime_error("unknown flit format: " + v);
        }
        else if (a == "--lanes") cfg.num_lanes = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--rate-gtps") cfg.lane_rate_gtps = std::stod(need(i));
        else if (a == "--mod") {
            const std::string v = need(i);
            if (v == "pam4") cfg.modulation = Modulation::PAM4;
            else if (v == "nrz") cfg.modulation = Modulation::NRZ;
            else throw std::runtime_error("unknown modulation: " + v);
        }
        else if (a == "--fdi-queue") cfg.fdi_queue_size = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--retry-buffer") cfg.retry_buffer_size = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--tx-delay") cfg.tx_pipe_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--rx-delay") cfg.rx_pipe_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--channel-delay") cfg.channel_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--feedback-delay") cfg.feedback_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--train") cfg.train_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--fdi-interval") cfg.fdi_interval_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--sigma") cfg.awgn_sigma = std::stod(need(i));
        else if (a == "--jitter") cfg.jitter_sigma_ui = std::stod(need(i));
        else if (a == "--isi1") cfg.isi_h1 = std::stod(need(i));
        else if (a == "--isi2") cfg.isi_h2 = std::stod(need(i));
        else if (a == "--skew") cfg.lane_skew_max_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--deskew") cfg.deskew_depth_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--extra-flit-error") cfg.extra_flit_error_rate = std::stod(need(i));
        else if (a == "--cdr-relock")
            cfg.cdr_relock_ui = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--cdr-ser") cfg.cdr_loss_error_fraction = std::stod(need(i));
        else if (a == "--cdr-bad-flits")
            cfg.cdr_loss_bad_flit_threshold =
                static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--cdr-noise-mult")
            cfg.cdr_unlocked_noise_multiplier = std::stod(need(i));
        else if (a == "--nak-repeat")
            cfg.nak_repeat_frames = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--seed") cfg.seed = static_cast<std::uint32_t>(std::stoul(need(i)));
        else if (a == "--max-time") cfg.max_time_ui = std::stoull(need(i));
        else if (a == "--csv") cfg.csv_path = need(i);
        else if (a == "--verbose") cfg.verbose = true;
        else throw std::runtime_error("unknown option: " + a);
    }
    require_valid_config(cfg);
    return cfg;
}

// ---------------------------------------------------------------------------
// CRC-16/CCITT (poly 0x1021)
// ---------------------------------------------------------------------------
inline std::uint16_t crc16_ccitt(const std::uint8_t* data, std::size_t n) {
    std::uint16_t crc = 0xFFFFU;
    for (std::size_t i = 0; i < n; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(data[i]) << 8U));
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000U) ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                                  : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

// Deterministic payload generator for the built-in workload. The endpoints
// compare against the retained workload for end-to-end integrity checking.
inline void fill_payload(std::vector<std::uint8_t>& bytes, std::size_t off,
                         std::size_t len, std::uint64_t transaction_id) {
    std::uint64_t s = splitmix64(transaction_id ^ 0xC001D00D12345678ULL);
    for (std::size_t i = 0; i < len; ++i) {
        if (i % 8U == 0U) s = splitmix64(s + i);
        bytes[off + i] = static_cast<std::uint8_t>((s >> ((i % 8U) * 8U)) & 0xFFU);
    }
}

// ---------------------------------------------------------------------------
// Flit build / check
//
// Standard 256B layout:
//   [0..1]    flit header (byte0 = seq mod 256, byte1 = flags; bit0 = replay)
//   [2..237]  236B TLP payload
//   [238..241] 4B DLLP (zero in this model)
//   [242..251] 10B reserved
//   [252..253] CRC-16 group A over bytes [0..127]
//   [254..255] CRC-16 group B over bytes [128..251]
//
// Compact 68B layout:
//   [0..1]   flit header
//   [2..65]  64B payload
//   [66..67] CRC-16 over bytes [0..65]
// ---------------------------------------------------------------------------
inline std::vector<std::uint8_t> build_flit(const Config& cfg, std::uint64_t seq,
                                            const std::vector<std::uint8_t>& payload,
                                            bool replay) {
    if (payload.size() > cfg.payload_bytes()) {
        throw std::runtime_error("FDI payload exceeds configured flit payload capacity");
    }
    if (cfg.flit_format == FlitFormat::AouFormat6) {
        // FDI 必须提供完整 250 字节，包括纯credit帧；不接受隐式短包补零。
        if (payload.size() != 250U) throw std::runtime_error("AoU PLP must be exactly 250B");
        aou_format6::Payload plp{};
        std::copy(payload.begin(), payload.end(), plp.begin());
        auto frame = aou_format6::scatter(plp,
            {static_cast<std::uint8_t>(seq), static_cast<std::uint8_t>(replay ? 1 : 0)});
        // 两段 CRC16-CCITT 分别保护半帧中的 126 字节，校验值高字节先写。
        // 校验槽位为 126/127 和 254/255，帧头保存序号及重放标志。
        const auto ca = crc16_ccitt(frame.data(), 126);
        const auto cb = crc16_ccitt(frame.data() + 128, 126);
        frame[126] = ca >> 8; frame[127] = ca & 255;
        frame[254] = cb >> 8; frame[255] = cb & 255;
        return {frame.begin(), frame.end()};
    }
    std::vector<std::uint8_t> b(cfg.flit_bytes(), 0U);
    b[0] = static_cast<std::uint8_t>(seq & 0xFFU);
    b[1] = replay ? 0x01U : 0x00U;
    std::copy(payload.begin(), payload.end(), b.begin() + 2U);
    if (cfg.flit_format == FlitFormat::Standard256) {
        const std::uint16_t ca = crc16_ccitt(b.data(), 128U);
        const std::uint16_t cb = crc16_ccitt(b.data() + 128U, 124U);
        b[252] = static_cast<std::uint8_t>(ca >> 8U); b[253] = static_cast<std::uint8_t>(ca & 0xFFU);
        b[254] = static_cast<std::uint8_t>(cb >> 8U); b[255] = static_cast<std::uint8_t>(cb & 0xFFU);
    } else {
        const std::uint16_t c = crc16_ccitt(b.data(), 66U);
        b[66] = static_cast<std::uint8_t>(c >> 8U); b[67] = static_cast<std::uint8_t>(c & 0xFFU);
    }
    return b;
}

struct FlitCheck {
    bool crc_ok = false;
    std::uint8_t seq8 = 0;
    bool replay_flag = false;
};

inline FlitCheck check_flit(const Config& cfg, const std::vector<std::uint8_t>& b) {
    FlitCheck r;
    if (b.size() != cfg.flit_bytes()) return r;
    r.seq8 = b[0];
    r.replay_flag = (b[1] & 0x01U) != 0U;
    if (cfg.flit_format == FlitFormat::AouFormat6) {
        const auto ca = crc16_ccitt(b.data(), 126);
        const auto cb = crc16_ccitt(b.data() + 128, 126);
        r.crc_ok = ca == ((b[126] << 8) | b[127]) && cb == ((b[254] << 8) | b[255]);
    } else if (cfg.flit_format == FlitFormat::Standard256) {
        const std::uint16_t ca = crc16_ccitt(b.data(), 128U);
        const std::uint16_t cb = crc16_ccitt(b.data() + 128U, 124U);
        const std::uint16_t ga = static_cast<std::uint16_t>((b[252] << 8U) | b[253]);
        const std::uint16_t gb = static_cast<std::uint16_t>((b[254] << 8U) | b[255]);
        r.crc_ok = (ca == ga) && (cb == gb);
    } else {
        const std::uint16_t c = crc16_ccitt(b.data(), 66U);
        const std::uint16_t g = static_cast<std::uint16_t>((b[66] << 8U) | b[67]);
        r.crc_ok = (c == g);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Stats (single-threaded SystemC kernel: no locking needed)
// ---------------------------------------------------------------------------
// Optional passive observer: must not wait, notify, or modify model state.
#include <functional>
struct LinkStats {
    using Observer = std::function<void(const char*, std::uint64_t, std::uint64_t,
        bool, const std::vector<std::uint8_t>&, const char*)>;
    Observer observer;
    void observe(const char* event, std::uint64_t seq, std::uint64_t id,
                 bool replay, const std::vector<std::uint8_t>& bytes,
                 const char* status) const {
        if (observer) observer(event, seq, id, replay, bytes, status);
    }

    std::uint64_t tx_new_flits = 0;
    std::uint64_t tx_replay_flits = 0;
    std::uint64_t ack_count = 0;
    std::uint64_t nak_count = 0;
    std::uint64_t stale_nak_count = 0;
    std::uint64_t crc_fail_count = 0;
    std::uint64_t seq_fail_count = 0;
    std::uint64_t duplicate_drop_count = 0;
    std::uint64_t retry_buffer_full_events = 0;
    std::uint64_t max_retry_buffer_occupancy = 0;
    std::uint64_t replay_timeout_events = 0;

    std::uint64_t total_phy_frames = 0;
    std::uint64_t total_symbols = 0;
    std::uint64_t symbol_errors = 0;
    std::uint64_t total_bits = 0;
    std::uint64_t bit_errors = 0;
    std::uint64_t deskew_failures = 0;
    std::uint64_t extra_error_flits = 0;
    std::uint64_t cdr_lock_loss_count = 0;
};

struct Stats {
    // End-to-end FDI workload / business-response accounting.
    std::uint64_t fdi_flits_generated = 0;
    std::uint64_t fdi_backpressure_events = 0;
    std::uint64_t requests_delivered = 0;
    std::uint64_t responses_generated = 0;
    std::uint64_t delivered_flits = 0;    // responses delivered back to the SoC
    std::uint64_t completed_payload_bytes = 0;
    std::uint64_t integrity_errors = 0;
    std::uint64_t forward_integrity_errors = 0;
    std::uint64_t reverse_integrity_errors = 0;

    LinkStats forward;
    LinkStats reverse;

    std::vector<double> forward_latencies_ns;
    std::vector<double> reverse_latencies_ns;
    std::vector<double> latencies_ns;     // full request/response round-trip latency

    bool watchdog_fired = false;
    std::uint32_t final_link_state = 0;
    double t_link_active_ns = 0.0;
    double t_end_ns = 0.0;
};
