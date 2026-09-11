// ucie_systemc_main.cpp
// Bidirectional UCIe SystemC behavioral link model.
//
// Request direction:
//   FdiSource -> TX Adapter -> PHY -> RX Adapter -> MemoryResponder
// Response direction:
//   MemoryResponder -> TX Adapter -> PHY -> RX Adapter -> FdiSink
//
// Each business direction owns an independent sequence/retry/ACK-NAK path.
// The FDI transaction carries caller-provided payload bytes; the built-in source
// either generates deterministic payloads or reads them from a text file.

#include <systemc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ucie_common.h"
#include "ucie_fdi.h"
#include "ucie_link.h"
#include "ucie_phy.h"

using sc_core::sc_event;
using sc_core::sc_time;
using sc_core::sc_time_stamp;
using sc_core::SC_FS;

struct WorkloadItem {
    std::vector<std::uint8_t> payload;  // padded to Config::payload_bytes()
    std::size_t valid_bytes = 0;
    std::uint64_t transaction_id = 0;
    std::uint8_t vc = 0;
};

// ---------------------------------------------------------------------------
// Workload file support
//
// Accepted non-comment line forms:
//   HEX_PAYLOAD
//   TRANSACTION_ID,VC,HEX_PAYLOAD
// Whitespace and '_' inside hexadecimal payloads are ignored. Short payloads
// are zero-padded; payloads larger than the configured capacity are rejected.
// ---------------------------------------------------------------------------
static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(trim(field));
    return fields;
}

static std::uint64_t parse_uint(const std::string& text, const char* field,
                                std::size_t line_no) {
    std::size_t pos = 0;
    try {
        const std::uint64_t value = std::stoull(text, &pos, 0);
        if (pos != text.size()) throw std::runtime_error("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid " + std::string(field) + " at payload file line " +
                                 std::to_string(line_no));
    }
}

static std::pair<std::vector<std::uint8_t>, std::size_t>
parse_hex_payload(std::string text, std::size_t capacity, std::size_t line_no) {
    std::string hex;
    hex.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isspace(c) || c == '_') continue;
        hex.push_back(static_cast<char>(c));
    }
    if (hex.size() >= 2U && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.erase(0U, 2U);
    }
    if (hex.empty() || (hex.size() & 1U) != 0U) {
        throw std::runtime_error("payload hex must contain a non-empty even number of digits at line " +
                                 std::to_string(line_no));
    }

    const std::size_t valid_bytes = hex.size() / 2U;
    if (valid_bytes > capacity) {
        throw std::runtime_error("payload at line " + std::to_string(line_no) +
                                 " exceeds configured capacity of " +
                                 std::to_string(capacity) + " bytes");
    }

    std::vector<std::uint8_t> payload(capacity, 0U);
    for (std::size_t i = 0; i < valid_bytes; ++i) {
        const std::string byte_text = hex.substr(i * 2U, 2U);
        std::size_t pos = 0;
        try {
            const unsigned long value = std::stoul(byte_text, &pos, 16);
            if (pos != 2U) throw std::runtime_error("invalid hex");
            payload[i] = static_cast<std::uint8_t>(value);
        } catch (const std::exception&) {
            throw std::runtime_error("invalid payload hex at line " +
                                     std::to_string(line_no));
        }
    }
    return {std::move(payload), valid_bytes};
}

static std::vector<WorkloadItem> make_workload(Config& cfg) {
    std::vector<WorkloadItem> items;
    if (cfg.payload_file.empty()) {
        items.reserve(static_cast<std::size_t>(cfg.flit_count));
        for (std::uint64_t i = 0; i < cfg.flit_count; ++i) {
            WorkloadItem item;
            item.payload.assign(cfg.payload_bytes(), 0U);
            fill_payload(item.payload, 0U, item.payload.size(), i);
            item.valid_bytes = item.payload.size();
            item.transaction_id = i;
            item.vc = 0;
            items.push_back(std::move(item));
        }
        return items;
    }

    std::ifstream in(cfg.payload_file);
    if (!in) throw std::runtime_error("failed to open payload file: " + cfg.payload_file);

    std::set<std::uint64_t> transaction_ids;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;

        const std::vector<std::string> fields = split_csv_line(line);
        WorkloadItem item;
        std::string payload_text;
        if (fields.size() == 1U) {
            item.transaction_id = static_cast<std::uint64_t>(items.size());
            item.vc = 0U;
            payload_text = fields[0];
        } else if (fields.size() == 3U) {
            item.transaction_id = parse_uint(fields[0], "transaction_id", line_no);
            const std::uint64_t vc = parse_uint(fields[1], "VC", line_no);
            if (vc > 255U) {
                throw std::runtime_error("VC must be in [0,255] at payload file line " +
                                         std::to_string(line_no));
            }
            item.vc = static_cast<std::uint8_t>(vc);
            payload_text = fields[2];
        } else {
            throw std::runtime_error("payload file line " + std::to_string(line_no) +
                                     " must be HEX or TRANSACTION_ID,VC,HEX");
        }

        if (!transaction_ids.insert(item.transaction_id).second) {
            throw std::runtime_error("duplicate transaction_id at payload file line " +
                                     std::to_string(line_no));
        }
        auto parsed = parse_hex_payload(payload_text, cfg.payload_bytes(), line_no);
        item.payload = std::move(parsed.first);
        item.valid_bytes = parsed.second;
        items.push_back(std::move(item));
    }

    if (items.empty()) throw std::runtime_error("payload file contains no payload records");
    cfg.flit_count = static_cast<std::uint64_t>(items.size());
    return items;
}

static std::string payload_to_hex(const std::vector<std::uint8_t>& payload,
                                  std::size_t valid_bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    const std::size_t count = std::min(valid_bytes, payload.size());
    for (std::size_t i = 0; i < count; ++i) {
        out << std::setw(2) << static_cast<unsigned>(payload[i]);
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Built-in FDI source. It is a testbench producer and can be replaced by any
// SystemC module that writes FdiFlit objects to the bounded FIFO.
// ---------------------------------------------------------------------------
SC_MODULE(FdiSource) {
    sc_core::sc_fifo_out<FdiFlit> out;

    SC_HAS_PROCESS(FdiSource);
    FdiSource(sc_core::sc_module_name name, const Config& cfg, Stats* stats, sc_time ui,
              const std::vector<WorkloadItem>& workload)
        : sc_module(name), cfg_(cfg), stats_(stats), ui_(ui), workload_(workload) {
        SC_THREAD(run);
    }

    void run() {
        const std::uint32_t interval =
            cfg_.fdi_interval_ui != 0U ? cfg_.fdi_interval_ui : cfg_.serialize_ui();
        wait(static_cast<double>(cfg_.train_ui) * ui_);
        for (const WorkloadItem& item : workload_) {
            while (out->num_free() == 0) {
                ++stats_->fdi_backpressure_events;
                wait(out->data_read_event());
            }
            FdiFlit flit;
            flit.payload = item.payload;
            flit.valid_bytes = item.valid_bytes;
            flit.transaction_id = item.transaction_id;
            flit.vc = item.vc;
            flit.kind = BusinessKind::Request;
            flit.transaction_start = sc_time_stamp();
            flit.fdi_time = flit.transaction_start;
            out->write(flit);
            ++stats_->fdi_flits_generated;
            wait(static_cast<double>(interval) * ui_);
        }
    }

private:
    const Config& cfg_;
    Stats* stats_;
    sc_time ui_;
    const std::vector<WorkloadItem>& workload_;
};

// ---------------------------------------------------------------------------
// Testbench memory-side endpoint. It checks the received request and returns an
// echo response after a configurable, pipelined service latency.
// ---------------------------------------------------------------------------
SC_MODULE(MemoryResponder) {
    sc_core::sc_fifo_in<FdiFlit> request_in;
    sc_core::sc_fifo_out<FdiFlit> response_out;

    SC_HAS_PROCESS(MemoryResponder);
    MemoryResponder(sc_core::sc_module_name name, const Config& cfg, Stats* stats,
                    sc_time ui, const std::vector<WorkloadItem>& expected)
        : sc_module(name), cfg_(cfg), stats_(stats), ui_(ui), expected_(expected) {
        SC_THREAD(ingress);
        SC_THREAD(egress);
    }

    void ingress() {
        for (;;) {
            FdiFlit frame = request_in->read();
            std::vector<std::uint8_t> payload = frame.payload;
            bool ok = next_request_ < expected_.size() &&
                      frame.kind == BusinessKind::Request;
            if (ok) {
                const WorkloadItem& expected = expected_[next_request_];
                ok = frame.transaction_id == expected.transaction_id &&
                     frame.vc == expected.vc && frame.valid_bytes == expected.valid_bytes &&
                     payload == expected.payload;
            }
            if (!ok) {
                ++stats_->integrity_errors;
                ++stats_->forward_integrity_errors;
            }
            ++next_request_;
            ++stats_->requests_delivered;
            stats_->forward_latencies_ns.push_back(
                (sc_time_stamp() - frame.transaction_start).to_seconds() * 1e9);

            FdiFlit response;
            response.payload = std::move(payload);
            response.valid_bytes = frame.valid_bytes;
            response.transaction_id = frame.transaction_id;
            response.vc = frame.vc;
            response.kind = BusinessKind::Response;
            response.transaction_start = frame.transaction_start;
            pending_.push_back({std::move(response),
                                sc_time_stamp() +
                                    static_cast<double>(cfg_.memory_delay_ui) * ui_});
            arrived_.notify(sc_core::SC_ZERO_TIME);
        }
    }

    void egress() {
        for (;;) {
            if (pending_.empty()) {
                wait(arrived_);
                continue;
            }
            auto item = std::move(pending_.front());
            pending_.pop_front();
            const sc_time now = sc_time_stamp();
            if (item.second > now) wait(item.second - now);
            while (response_out->num_free() == 0) wait(response_out->data_read_event());
            item.first.fdi_time = sc_time_stamp();
            response_out->write(item.first);
            ++stats_->responses_generated;
        }
    }

private:
    const Config& cfg_;
    Stats* stats_;
    sc_time ui_;
    const std::vector<WorkloadItem>& expected_;
    std::size_t next_request_ = 0;
    std::deque<std::pair<FdiFlit, sc_time>> pending_;
    sc_event arrived_;
};

// ---------------------------------------------------------------------------
// SoC-side response sink: verifies transaction/VC/payload and round-trip order.
// ---------------------------------------------------------------------------
SC_MODULE(FdiSink) {
    sc_core::sc_fifo_in<FdiFlit> response_in;

    SC_HAS_PROCESS(FdiSink);
    FdiSink(sc_core::sc_module_name name, const Config& cfg, Stats* stats,
            const std::vector<WorkloadItem>& expected)
        : sc_module(name), cfg_(cfg), stats_(stats), expected_(expected) {
        if (!cfg_.response_file.empty()) {
            response_file_.open(cfg_.response_file);
            if (!response_file_) {
                throw std::runtime_error("failed to open response file: " + cfg_.response_file);
            }
        }
        SC_THREAD(run);
    }

    void run() {
        for (;;) {
            FdiFlit frame = response_in->read();
            const std::vector<std::uint8_t>& payload = frame.payload;
            bool ok = next_response_ < expected_.size() &&
                      frame.kind == BusinessKind::Response;
            if (ok) {
                const WorkloadItem& expected = expected_[next_response_];
                ok = frame.transaction_id == expected.transaction_id &&
                     frame.vc == expected.vc && frame.valid_bytes == expected.valid_bytes &&
                     payload == expected.payload;
            }
            if (!ok) {
                ++stats_->integrity_errors;
                ++stats_->reverse_integrity_errors;
            }

            if (response_file_) {
                response_file_ << frame.transaction_id << ","
                               << static_cast<unsigned>(frame.vc) << ","
                               << payload_to_hex(payload, frame.valid_bytes) << "\n";
            }

            stats_->reverse_latencies_ns.push_back(
                (sc_time_stamp() - frame.fdi_time).to_seconds() * 1e9);
            stats_->latencies_ns.push_back(
                (sc_time_stamp() - frame.transaction_start).to_seconds() * 1e9);
            stats_->completed_payload_bytes += frame.valid_bytes;
            ++stats_->delivered_flits;
            ++next_response_;

            if (cfg_.verbose && stats_->delivered_flits <= 8U) {
                std::cout << "[SOC] response id=" << frame.transaction_id << " vc="
                          << static_cast<unsigned>(frame.vc) << " @" << sc_time_stamp() << "\n";
            }
            if (stats_->delivered_flits >= expected_.size()) {
                stats_->t_end_ns = sc_time_stamp().to_seconds() * 1e9;
                if (response_file_) response_file_.flush();
                sc_core::sc_stop();
                return;
            }
        }
    }

private:
    const Config& cfg_;
    Stats* stats_;
    const std::vector<WorkloadItem>& expected_;
    std::size_t next_response_ = 0;
    std::ofstream response_file_;
};

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------
SC_MODULE(Watchdog) {
    SC_HAS_PROCESS(Watchdog);
    Watchdog(sc_core::sc_module_name name, const Config& cfg, Stats* stats, sc_time ui,
             UcieLink* link)
        : sc_module(name), cfg_(cfg), stats_(stats), ui_(ui), link_(link) {
        SC_THREAD(run);
    }
    void run() {
        wait(static_cast<double>(cfg_.max_time_ui) * ui_);
        stats_->watchdog_fired = true;
        stats_->t_end_ns = sc_time_stamp().to_seconds() * 1e9;
        link_->fail();
        wait(sc_core::SC_ZERO_TIME);
        std::cerr << "WATCHDOG: simulation exceeded " << cfg_.max_time_ui << " UI\n";
        sc_core::sc_stop();
    }
private:
    const Config& cfg_;
    Stats* stats_;
    sc_time ui_;
    UcieLink* link_;
};

// ---------------------------------------------------------------------------
// Summary and compatibility metrics
// ---------------------------------------------------------------------------
static double mean_of(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

static double pct_of(std::vector<double> values, double pct) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = pct / 100.0 * static_cast<double>(values.size() - 1U);
    return values[std::min(values.size() - 1U,
                           static_cast<std::size_t>(std::llround(pos)))];
}

static std::uint64_t sum(std::uint64_t LinkStats::*field, const Stats& stats) {
    return stats.forward.*field + stats.reverse.*field;
}

struct Summary {
    double active_ns = 0.0;
    double throughput_gbps = 0.0;
    double capacity_gbps = 0.0;
    double utilization = 0.0;
    double avg_lat_ns = 0.0;
    double p50_lat_ns = 0.0;
    double p95_lat_ns = 0.0;
    double p99_lat_ns = 0.0;
    double forward_avg_lat_ns = 0.0;
    double forward_p95_lat_ns = 0.0;
    double forward_p99_lat_ns = 0.0;
    double reverse_avg_lat_ns = 0.0;
    double reverse_p95_lat_ns = 0.0;
    double reverse_p99_lat_ns = 0.0;
    double ser = 0.0;
    double ber = 0.0;
    double flit_fail_rate = 0.0;
};

static Summary summarize(const Config& cfg, const Stats& stats) {
    Summary summary;
    const double train_ns = static_cast<double>(cfg.train_ui) * (cfg.ui_fs() * 1e-6);
    summary.active_ns = std::max(1e-9, stats.t_end_ns - train_ns);
    summary.throughput_gbps =
        static_cast<double>(stats.completed_payload_bytes) * 8.0 / summary.active_ns;
    summary.capacity_gbps = static_cast<double>(cfg.num_lanes) *
                            static_cast<double>(cfg.bits_per_ui()) * cfg.lane_rate_gtps;
    summary.utilization = summary.capacity_gbps > 0.0
                              ? summary.throughput_gbps / summary.capacity_gbps
                              : 0.0;
    summary.avg_lat_ns = mean_of(stats.latencies_ns);
    summary.p50_lat_ns = pct_of(stats.latencies_ns, 50.0);
    summary.p95_lat_ns = pct_of(stats.latencies_ns, 95.0);
    summary.p99_lat_ns = pct_of(stats.latencies_ns, 99.0);
    summary.forward_avg_lat_ns = mean_of(stats.forward_latencies_ns);
    summary.forward_p95_lat_ns = pct_of(stats.forward_latencies_ns, 95.0);
    summary.forward_p99_lat_ns = pct_of(stats.forward_latencies_ns, 99.0);
    summary.reverse_avg_lat_ns = mean_of(stats.reverse_latencies_ns);
    summary.reverse_p95_lat_ns = pct_of(stats.reverse_latencies_ns, 95.0);
    summary.reverse_p99_lat_ns = pct_of(stats.reverse_latencies_ns, 99.0);

    const std::uint64_t total_symbols = sum(&LinkStats::total_symbols, stats);
    const std::uint64_t symbol_errors = sum(&LinkStats::symbol_errors, stats);
    const std::uint64_t total_bits = sum(&LinkStats::total_bits, stats);
    const std::uint64_t bit_errors = sum(&LinkStats::bit_errors, stats);
    const std::uint64_t total_frames = sum(&LinkStats::total_phy_frames, stats);
    const std::uint64_t failed_frames = sum(&LinkStats::crc_fail_count, stats) +
                                        sum(&LinkStats::seq_fail_count, stats);
    summary.ser = total_symbols ? static_cast<double>(symbol_errors) /
                                      static_cast<double>(total_symbols) : 0.0;
    summary.ber = total_bits ? static_cast<double>(bit_errors) /
                                  static_cast<double>(total_bits) : 0.0;
    summary.flit_fail_rate = total_frames ? static_cast<double>(failed_frames) /
                                                static_cast<double>(total_frames) : 0.0;
    return summary;
}

static void print_direction(const char* name, const LinkStats& stats) {
    std::cout << name << " TX new/replay, ACK/NAK : " << stats.tx_new_flits << "/"
              << stats.tx_replay_flits << ", " << stats.ack_count << "/"
              << stats.nak_count << "\n";
    std::cout << name << " CRC/seq fail, PHY frames: " << stats.crc_fail_count << "/"
              << stats.seq_fail_count << ", " << stats.total_phy_frames << "\n";
}

static void print_summary(const Config& cfg, const Stats& stats, const Summary& summary) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "=== Bidirectional UCIe SystemC Simulation ===\n";
    std::cout << "workload=" << (cfg.payload_file.empty() ? "internal" : "external-file")
              << ", transactions=" << cfg.flit_count
              << ", memory_delay=" << cfg.memory_delay_ui << " UI\n";
    std::cout << "flit_format="
              << (cfg.flit_format == FlitFormat::AouFormat6 ? "AoU Format 6" :
                  cfg.flit_format == FlitFormat::Standard256 ? "Standard 256B" : "Compact 68B")
              << ", payload_capacity=" << cfg.payload_bytes() << "B\n";
    std::cout << "lanes=" << cfg.num_lanes << ", rate=" << cfg.lane_rate_gtps
              << " GT/s, mod=" << (cfg.modulation == Modulation::PAM4 ? "PAM4" : "NRZ")
              << ", UI=" << cfg.ui_fs() / 1000.0 << " ps, serialize="
              << cfg.serialize_ui() << " UI\n\n";

    std::cout << "Requests delivered          : " << stats.requests_delivered << "/"
              << cfg.flit_count << "\n";
    std::cout << "Responses delivered         : " << stats.delivered_flits << "/"
              << cfg.flit_count << (stats.watchdog_fired ? "  [WATCHDOG]" : "") << "\n";
    std::cout << "Completed payload bytes     : " << stats.completed_payload_bytes << "\n";
    std::cout << "Active link time            : " << summary.active_ns << " ns\n";
    std::cout << "Completed payload throughput: " << summary.throughput_gbps << " Gbps\n";
    std::cout << "One-direction raw capacity  : " << summary.capacity_gbps << " Gbps\n";
    std::cout << "Bandwidth utilization       : " << summary.utilization * 100.0 << " %\n";
    std::cout << "Round-trip Avg/P50/P95/P99   : " << summary.avg_lat_ns << "/"
              << summary.p50_lat_ns << "/" << summary.p95_lat_ns << "/"
              << summary.p99_lat_ns << " ns\n";
    std::cout << "Forward/reverse avg latency : " << summary.forward_avg_lat_ns << "/"
              << summary.reverse_avg_lat_ns << " ns\n\n";

    print_direction("FWD", stats.forward);
    print_direction("REV", stats.reverse);
    std::cout << "\nAggregate symbol error rate : " << std::setprecision(9)
              << summary.ser << "\n";
    std::cout << "Aggregate bit error rate    : " << summary.ber << "\n";
    std::cout << "Observed flit fail rate     : " << summary.flit_fail_rate << "\n"
              << std::setprecision(6);
    std::cout << "Forward/reverse integrity   : " << stats.forward_integrity_errors << "/"
              << stats.reverse_integrity_errors << "\n";
    std::cout << "Total integrity errors      : " << stats.integrity_errors << "\n";
}

static void write_csv(const Config& cfg, const Stats& stats, const Summary& summary) {
    if (cfg.csv_path.empty()) return;
    std::ofstream out(cfg.csv_path);
    if (!out) throw std::runtime_error("failed to open CSV path: " + cfg.csv_path);

    out << "metric,value\n";
    out << "input_mode," << (cfg.payload_file.empty() ? "internal" : "external") << "\n";
    out << "flits_requested," << cfg.flit_count << "\n";
    out << "requests_delivered," << stats.requests_delivered << "\n";
    out << "responses_generated," << stats.responses_generated << "\n";
    out << "delivered_flits," << stats.delivered_flits << "\n";
    out << "fdi_backpressure_events," << stats.fdi_backpressure_events << "\n";
    out << "completed_payload_bytes," << stats.completed_payload_bytes << "\n";
    out << "flit_bytes," << cfg.flit_bytes() << "\n";
    out << "payload_bytes," << cfg.payload_bytes() << "\n";
    out << "flit_efficiency," << cfg.flit_efficiency() << "\n";
    out << "memory_delay_ui," << cfg.memory_delay_ui << "\n";
    out << "num_lanes," << cfg.num_lanes << "\n";
    out << "lane_rate_gtps," << cfg.lane_rate_gtps << "\n";
    out << "modulation," << (cfg.modulation == Modulation::PAM4 ? "pam4" : "nrz") << "\n";
    out << "serialize_ui," << cfg.serialize_ui() << "\n";
    out << "active_ns," << summary.active_ns << "\n";
    out << "throughput_gbps," << summary.throughput_gbps << "\n";
    out << "capacity_gbps," << summary.capacity_gbps << "\n";
    out << "bandwidth_utilization," << summary.utilization << "\n";
    out << "avg_latency_ns," << summary.avg_lat_ns << "\n";
    out << "p50_latency_ns," << summary.p50_lat_ns << "\n";
    out << "p95_latency_ns," << summary.p95_lat_ns << "\n";
    out << "p99_latency_ns," << summary.p99_lat_ns << "\n";
    out << "forward_avg_latency_ns," << summary.forward_avg_lat_ns << "\n";
    out << "forward_p95_latency_ns," << summary.forward_p95_lat_ns << "\n";
    out << "forward_p99_latency_ns," << summary.forward_p99_lat_ns << "\n";
    out << "reverse_avg_latency_ns," << summary.reverse_avg_lat_ns << "\n";
    out << "reverse_p95_latency_ns," << summary.reverse_p95_lat_ns << "\n";
    out << "reverse_p99_latency_ns," << summary.reverse_p99_lat_ns << "\n";

    out << "forward_tx_new_flits," << stats.forward.tx_new_flits << "\n";
    out << "reverse_tx_new_flits," << stats.reverse.tx_new_flits << "\n";
    out << "forward_tx_replay_flits," << stats.forward.tx_replay_flits << "\n";
    out << "reverse_tx_replay_flits," << stats.reverse.tx_replay_flits << "\n";
    out << "forward_ack_count," << stats.forward.ack_count << "\n";
    out << "reverse_ack_count," << stats.reverse.ack_count << "\n";
    out << "forward_nak_count," << stats.forward.nak_count << "\n";
    out << "reverse_nak_count," << stats.reverse.nak_count << "\n";
    out << "forward_crc_fail_count," << stats.forward.crc_fail_count << "\n";
    out << "reverse_crc_fail_count," << stats.reverse.crc_fail_count << "\n";
    out << "forward_integrity_errors," << stats.forward_integrity_errors << "\n";
    out << "reverse_integrity_errors," << stats.reverse_integrity_errors << "\n";

    // Aggregate legacy keys retained for existing scripts.
    out << "tx_new_flits," << sum(&LinkStats::tx_new_flits, stats) << "\n";
    out << "tx_replay_flits," << sum(&LinkStats::tx_replay_flits, stats) << "\n";
    out << "ack_count," << sum(&LinkStats::ack_count, stats) << "\n";
    out << "nak_count," << sum(&LinkStats::nak_count, stats) << "\n";
    out << "stale_nak_count," << sum(&LinkStats::stale_nak_count, stats) << "\n";
    out << "replay_timeout_events," << sum(&LinkStats::replay_timeout_events, stats) << "\n";
    out << "crc_fail_count," << sum(&LinkStats::crc_fail_count, stats) << "\n";
    out << "seq_fail_count," << sum(&LinkStats::seq_fail_count, stats) << "\n";
    out << "duplicate_drop_count," << sum(&LinkStats::duplicate_drop_count, stats) << "\n";
    out << "max_retry_buffer_occupancy,"
        << std::max(stats.forward.max_retry_buffer_occupancy,
                    stats.reverse.max_retry_buffer_occupancy) << "\n";
    out << "retry_buffer_full_events,"
        << sum(&LinkStats::retry_buffer_full_events, stats) << "\n";
    out << "total_phy_frames," << sum(&LinkStats::total_phy_frames, stats) << "\n";
    out << "symbol_error_rate," << summary.ser << "\n";
    out << "bit_error_rate," << summary.ber << "\n";
    out << "observed_flit_fail_rate," << summary.flit_fail_rate << "\n";
    out << "deskew_failures," << sum(&LinkStats::deskew_failures, stats) << "\n";
    out << "extra_error_flits," << sum(&LinkStats::extra_error_flits, stats) << "\n";
    out << "cdr_lock_loss_count," << sum(&LinkStats::cdr_lock_loss_count, stats) << "\n";
    out << "integrity_errors," << stats.integrity_errors << "\n";
    out << "watchdog_fired," << (stats.watchdog_fired ? 1 : 0) << "\n";
    out << "link_state," << to_string(static_cast<LinkState>(stats.final_link_state)) << "\n";
}

// ---------------------------------------------------------------------------
// sc_main
// ---------------------------------------------------------------------------
int sc_main(int argc, char* argv[]) {
    try {
        sc_core::sc_set_time_resolution(1.0, SC_FS);
        Config cfg = parse_args(argc, argv);
        const std::vector<WorkloadItem> workload = make_workload(cfg);
        Stats stats;
        const sc_time ui(cfg.ui_fs(), SC_FS);

        // The testbench accesses the reusable UcieLink only through four
        // symmetric transaction-level FDI FIFOs and a link-state signal.
        sc_core::sc_fifo<FdiFlit> soc_tx(static_cast<int>(cfg.fdi_queue_size));
        sc_core::sc_fifo<FdiFlit> soc_rx(16);
        sc_core::sc_fifo<FdiFlit> mem_tx(static_cast<int>(cfg.fdi_queue_size));
        sc_core::sc_fifo<FdiFlit> mem_rx(16);
        sc_core::sc_signal<unsigned> link_state(
            "link_state", static_cast<unsigned>(LinkState::Reset));

        FdiSource source("fdi_source", cfg, &stats, ui, workload);
        UcieLink link("ucie_link", cfg, &stats, ui);
        MemoryResponder memory("memory_responder", cfg, &stats, ui, workload);
        FdiSink sink("fdi_sink", cfg, &stats, workload);
        Watchdog watchdog("watchdog", cfg, &stats, ui, &link);

        source.out(soc_tx);
        link.soc_tx_in(soc_tx);
        link.soc_rx_out(soc_rx);
        link.mem_tx_in(mem_tx);
        link.mem_rx_out(mem_rx);
        link.link_state(link_state);
        memory.request_in(mem_rx);
        memory.response_out(mem_tx);
        sink.response_in(soc_rx);

        sc_core::sc_start();
        stats.final_link_state = link_state.read();

        const Summary summary = summarize(cfg, stats);
        print_summary(cfg, stats, summary);
        write_csv(cfg, stats, summary);

        if (stats.delivered_flits < workload.size()) {
            std::cerr << "WARNING: not all business responses were delivered.\n";
            return 2;
        }
        if (stats.integrity_errors != 0U) {
            std::cerr << "ERROR: end-to-end payload or ordering error observed.\n";
            return 3;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
