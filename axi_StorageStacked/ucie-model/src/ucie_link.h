// ucie_link.h - reusable bidirectional UCIe SystemC behavioral link module.
#pragma once

#include <systemc.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ucie_common.h"
#include "ucie_fdi.h"
#include "ucie_phy.h"

namespace ucie_detail {

// 检查公开 FDI 接口的载荷长度，供链路接收入口与组件测试共用。
inline void require_valid_fdi(const Config& cfg, const FdiFlit& flit) {
    if (flit.payload.size() != cfg.payload_bytes())
        throw std::runtime_error("FDI payload is not padded to configured size");
    if (flit.valid_bytes > flit.payload.size())
        throw std::runtime_error("FDI valid_bytes exceeds payload");
    if (cfg.flit_format == FlitFormat::AouFormat6 && flit.valid_bytes != 250U)
        throw std::runtime_error("AoU FDI valid_bytes must be exactly 250");
}

// Internal link frame. This type never crosses a public UcieLink FDI port.
struct Frame {
    std::vector<std::uint8_t> bytes;
    std::uint64_t seq = 0;
    std::uint64_t transaction_id = 0;
    std::uint8_t vc = 0;
    std::size_t valid_bytes = 0;
    BusinessKind kind = BusinessKind::Request;
    bool replay = false;
    sc_core::sc_time transaction_start;
    sc_core::sc_time fdi_time;
};

inline std::ostream& operator<<(std::ostream& os, const Frame& frame) {
    return os << "Frame{seq=" << frame.seq << ",id=" << frame.transaction_id << "}";
}

struct FbMsg {
    bool is_nak = false;
    std::uint64_t seq = 0;
};

inline std::ostream& operator<<(std::ostream& os, const FbMsg& msg) {
    return os << (msg.is_nak ? "NAK{" : "ACK{") << msg.seq << "}";
}

inline FdiFlit unpack_fdi(const Config& cfg, const Frame& frame) {
    if (frame.bytes.size() < 2U + cfg.payload_bytes()) {
        throw std::runtime_error("received frame is shorter than the configured FDI payload");
    }
    FdiFlit flit;
    if (cfg.flit_format == FlitFormat::AouFormat6) {
        if (frame.bytes.size() != 256U)
            throw std::runtime_error("AoU physical flit must be exactly 256B");
        aou_format6::Frame physical{};
        std::copy(frame.bytes.begin(), frame.bytes.end(), physical.begin());
        const auto plp = aou_format6::gather(physical);
        flit.payload.assign(plp.begin(), plp.end());
    } else {
        flit.payload.assign(frame.bytes.begin() + 2U,
                            frame.bytes.begin() + 2U + cfg.payload_bytes());
    }
    flit.valid_bytes = frame.valid_bytes;
    flit.transaction_id = frame.transaction_id;
    flit.vc = frame.vc;
    flit.kind = frame.kind;
    flit.transaction_start = frame.transaction_start;
    flit.fdi_time = frame.fdi_time;
    return flit;
}

SC_MODULE(TxAdapter) {
    sc_core::sc_fifo_in<FdiFlit> fdi_in;
    sc_core::sc_fifo_out<Frame> phy_out;
    sc_core::sc_fifo_in<FbMsg> fb_in;

    enum class TxState { Idle, AckReceived, NakReceived, NakInProgress };

    SC_HAS_PROCESS(TxAdapter);
    TxAdapter(sc_core::sc_module_name name, const Config& cfg, LinkStats* stats,
              sc_core::sc_time ui, std::string direction)
        : sc_module(name), cfg_(cfg), stats_(stats), ui_(ui), direction_(std::move(direction)) {
        replay_timeout_ui_ = 4U * (cfg_.serialize_ui() + cfg_.tx_pipe_ui + cfg_.channel_ui +
                                   cfg_.rx_pipe_ui + cfg_.feedback_ui + 64U);
        SC_THREAD(sender);
        SC_THREAD(feedback);
    }

    void sender() {
        wait(static_cast<double>(cfg_.train_ui) * ui_);
        for (;;) {
            bool sent = false;
            if (replay_active_) {
                auto it = retry_buffer_.lower_bound(replay_cursor_);
                if (it == retry_buffer_.end() || it->first > replay_end_) {
                    replay_active_ = false;
                    tx_state_ = TxState::Idle;
                } else {
                    send_one(it->first, it->second, true);
                    replay_cursor_ = it->first + 1U;
                    if (replay_cursor_ > replay_end_) {
                        replay_active_ = false;
                        tx_state_ = TxState::Idle;
                    }
                    sent = true;
                }
            }

            if (!sent) {
                if (fdi_in->num_available() > 0 &&
                    retry_buffer_.size() < cfg_.retry_buffer_size) {
                    FdiFlit flit = fdi_in->read();
                    require_valid_fdi(cfg_, flit);
                    const std::uint64_t seq = next_seq_++;
                    retry_buffer_[seq] = Entry{std::move(flit)};
                    stats_->max_retry_buffer_occupancy = std::max<std::uint64_t>(
                        stats_->max_retry_buffer_occupancy, retry_buffer_.size());
                    send_one(seq, retry_buffer_.at(seq), false);
                    sent = true;
                } else if (fdi_in->num_available() > 0) {
                    ++stats_->retry_buffer_full_events;
                }
            }

            if (!sent) {
                if (!retry_buffer_.empty()) {
                    wait(static_cast<double>(replay_timeout_ui_) * ui_,
                         wake_ev_ | fdi_in->data_written_event());
                    const sc_core::sc_time since = sc_core::sc_time_stamp() - last_progress_;
                    if (!replay_active_ && !retry_buffer_.empty() &&
                        since >= static_cast<double>(replay_timeout_ui_) * ui_) {
                        replay_active_ = true;
                        replay_cursor_ = retry_buffer_.begin()->first;
                        replay_end_ = retry_buffer_.rbegin()->first;
                        tx_state_ = TxState::NakInProgress;
                        ++stats_->replay_timeout_events;
                    }
                } else {
                    wait(wake_ev_ | fdi_in->data_written_event());
                }
            }
        }
    }

    void feedback() {
        for (;;) {
            const FbMsg fb = fb_in->read();
            last_progress_ = sc_core::sc_time_stamp();
            if (!fb.is_nak) {
                ++stats_->ack_count;
                tx_state_ = TxState::AckReceived;
                auto it = retry_buffer_.begin();
                while (it != retry_buffer_.end() && it->first <= fb.seq) {
                    it = retry_buffer_.erase(it);
                }
                if (!replay_active_) tx_state_ = TxState::Idle;
            } else {
                ++stats_->nak_count;
                tx_state_ = TxState::NakReceived;
                auto it = retry_buffer_.lower_bound(fb.seq);
                if (it == retry_buffer_.end()) {
                    ++stats_->stale_nak_count;
                    if (!replay_active_) tx_state_ = TxState::Idle;
                } else {
                    replay_active_ = true;
                    replay_cursor_ = it->first;
                    replay_end_ = retry_buffer_.rbegin()->first;
                    tx_state_ = TxState::NakInProgress;
                }
            }
            wake_ev_.notify(sc_core::SC_ZERO_TIME);
        }
    }

private:
    struct Entry { FdiFlit flit; };

    void send_one(std::uint64_t seq, const Entry& entry, bool replay) {
        Frame frame;
        frame.seq = seq;
        frame.transaction_id = entry.flit.transaction_id;
        frame.vc = entry.flit.vc;
        frame.valid_bytes = entry.flit.valid_bytes;
        frame.kind = entry.flit.kind;
        frame.replay = replay;
        frame.transaction_start = entry.flit.transaction_start;
        frame.fdi_time = entry.flit.fdi_time;
        if (!replay) stats_->observe("TX_FDI", seq, frame.transaction_id,
            false, entry.flit.payload, "accepted");
        frame.bytes = build_flit(cfg_, seq, entry.flit.payload, replay);
        stats_->observe("TX_FRAME", seq, frame.transaction_id, replay,
                        frame.bytes, "serialization_start");
        wait(static_cast<double>(cfg_.serialize_ui()) * ui_);
        phy_out->write(frame);
        last_progress_ = sc_core::sc_time_stamp();
        if (replay) ++stats_->tx_replay_flits; else ++stats_->tx_new_flits;
    }

    const Config& cfg_;
    LinkStats* stats_;
    sc_core::sc_time ui_;
    std::string direction_;
    std::uint32_t replay_timeout_ui_ = 0;
    TxState tx_state_ = TxState::Idle;
    std::uint64_t next_seq_ = 0;
    std::map<std::uint64_t, Entry> retry_buffer_;
    bool replay_active_ = false;
    std::uint64_t replay_cursor_ = 0;
    std::uint64_t replay_end_ = 0;
    sc_core::sc_time last_progress_;
    sc_core::sc_event wake_ev_;
};

SC_MODULE(PhyChannel) {
    sc_core::sc_fifo_in<Frame> in;
    sc_core::sc_fifo_out<Frame> out;

    SC_HAS_PROCESS(PhyChannel);
    PhyChannel(sc_core::sc_module_name name, const Config& cfg, LinkStats* stats,
               sc_core::sc_time ui)
        : sc_module(name), cfg_(cfg), stats_(stats), ui_(ui), phy_(cfg) {
        SC_THREAD(ingress);
        SC_THREAD(egress);
    }

    void ingress() {
        for (;;) {
            Frame frame = in->read();
            const double now_ui = sc_core::sc_time_stamp() / ui_;
            PhyResult result = phy_.transmit(frame.bytes, now_ui);
            ++stats_->total_phy_frames;
            stats_->total_symbols += result.total_symbols;
            stats_->symbol_errors += result.symbol_errors;
            stats_->total_bits += static_cast<std::uint64_t>(frame.bytes.size()) * 8U;
            stats_->bit_errors += result.bit_errors;
            if (result.deskew_failure) ++stats_->deskew_failures;
            if (result.extra_error) ++stats_->extra_error_flits;
            phy_.observe(now_ui, result);
            stats_->cdr_lock_loss_count = phy_.lock_loss_count();

            frame.bytes = std::move(result.rx_bytes);
            const std::uint32_t pipe_ui = cfg_.tx_pipe_ui + cfg_.channel_ui +
                                          result.max_skew_ui + cfg_.rx_pipe_ui;
            pending_.push_back({std::move(frame), sc_core::sc_time_stamp() +
                                                     static_cast<double>(pipe_ui) * ui_});
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
            const sc_core::sc_time now = sc_core::sc_time_stamp();
            if (item.second > now) wait(item.second - now);
            out->write(item.first);
        }
    }

private:
    const Config& cfg_;
    LinkStats* stats_;
    sc_core::sc_time ui_;
    BehavioralPhy phy_;
    std::deque<std::pair<Frame, sc_core::sc_time>> pending_;
    sc_core::sc_event arrived_;
};

SC_MODULE(RxAdapter) {
    sc_core::sc_fifo_in<Frame> phy_in;
    sc_core::sc_fifo_out<FbMsg> fb_out;
    sc_core::sc_fifo_out<FdiFlit> fdi_out;

    enum class RxState { NoRetry, RetryInProgress };

    SC_HAS_PROCESS(RxAdapter);
    RxAdapter(sc_core::sc_module_name name, const Config& cfg, LinkStats* stats,
              std::string direction)
        : sc_module(name), cfg_(cfg), stats_(stats), direction_(std::move(direction)) {
        SC_THREAD(run);
    }

    void run() {
        for (;;) {
            Frame frame = phy_in->read();
            const FlitCheck check = check_flit(cfg_, frame.bytes);
            const std::uint8_t expected8 = static_cast<std::uint8_t>(expected_seq_ & 0xFFU);
            const std::uint8_t diff =
                static_cast<std::uint8_t>((check.seq8 - expected8) & 0xFFU);

            stats_->observe("RX_FRAME", frame.seq, frame.transaction_id, frame.replay,
                frame.bytes, !check.crc_ok ? "crc_error" :
                diff == 0U ? "in_order" : diff > 127U ? "duplicate" : "seq_error");
            if (check.crc_ok && diff == 0U) {
                rx_state_ = RxState::NoRetry;
                frames_since_nak_ = 0;
                const auto delivered = unpack_fdi(cfg_, frame);
                fdi_out->write(delivered);
                stats_->observe("RX_FDI", frame.seq, frame.transaction_id,
                    frame.replay, delivered.payload, "delivered");
                fb_out->write(FbMsg{false, expected_seq_});
                ++expected_seq_;
            } else if (check.crc_ok && diff > 127U) {
                ++stats_->duplicate_drop_count;
                if (expected_seq_ > 0U) fb_out->write(FbMsg{false, expected_seq_ - 1U});
            } else {
                if (!check.crc_ok) ++stats_->crc_fail_count;
                else ++stats_->seq_fail_count;
                const bool send_nak = (rx_state_ == RxState::NoRetry) ||
                                      (++frames_since_nak_ >= cfg_.nak_repeat_frames);
                if (send_nak) {
                    fb_out->write(FbMsg{true, expected_seq_});
                    frames_since_nak_ = 0;
                }
                rx_state_ = RxState::RetryInProgress;
            }
        }
    }

private:
    const Config& cfg_;
    LinkStats* stats_;
    std::string direction_;
    RxState rx_state_ = RxState::NoRetry;
    std::uint64_t expected_seq_ = 0;
    std::uint32_t frames_since_nak_ = 0;
};

SC_MODULE(FeedbackPath) {
    sc_core::sc_fifo_in<FbMsg> in;
    sc_core::sc_fifo_out<FbMsg> out;

    SC_HAS_PROCESS(FeedbackPath);
    FeedbackPath(sc_core::sc_module_name name, const Config& cfg, sc_core::sc_time ui)
        : sc_module(name), cfg_(cfg), ui_(ui) {
        SC_THREAD(ingress);
        SC_THREAD(egress);
    }

    void ingress() {
        for (;;) {
            const FbMsg msg = in->read();
            pending_.push_back({msg, sc_core::sc_time_stamp() +
                                     static_cast<double>(cfg_.feedback_ui) * ui_});
            arrived_.notify(sc_core::SC_ZERO_TIME);
        }
    }

    void egress() {
        for (;;) {
            if (pending_.empty()) {
                wait(arrived_);
                continue;
            }
            const auto item = pending_.front();
            pending_.pop_front();
            const sc_core::sc_time now = sc_core::sc_time_stamp();
            if (item.second > now) wait(item.second - now);
            out->write(item.first);
        }
    }

private:
    const Config& cfg_;
    sc_core::sc_time ui_;
    std::deque<std::pair<FbMsg, sc_core::sc_time>> pending_;
    sc_core::sc_event arrived_;
};

}  // namespace ucie_detail

// Reusable public link boundary:
//   soc_tx_in  -> forward link -> mem_rx_out
//   mem_tx_in  -> reverse link -> soc_rx_out
// Bounded sc_fifo channels provide blocking backpressure in both directions.
SC_MODULE(UcieLink) {
    sc_core::sc_fifo_in<FdiFlit> soc_tx_in;
    sc_core::sc_fifo_out<FdiFlit> soc_rx_out;
    sc_core::sc_fifo_in<FdiFlit> mem_tx_in;
    sc_core::sc_fifo_out<FdiFlit> mem_rx_out;
    // Encodes LinkState as an unsigned value so standard SystemC tracing works.
    sc_core::sc_out<unsigned> link_state;

    SC_HAS_PROCESS(UcieLink);
    UcieLink(sc_core::sc_module_name name, const Config& cfg, Stats* stats,
             sc_core::sc_time ui)
        : sc_module(name), cfg_(cfg), reverse_cfg_([&cfg] {
              Config reverse = cfg;
              reverse.seed ^= 0xA5A55A5AU;
              return reverse;
          }()), stats_(stats), ui_(ui),
          fwd_tx_to_phy_("fwd_tx_to_phy", 4), fwd_phy_to_rx_("fwd_phy_to_rx", 4),
          fwd_rx_to_feedback_("fwd_rx_to_feedback", 64),
          fwd_feedback_to_tx_("fwd_feedback_to_tx", 64),
          rev_tx_to_phy_("rev_tx_to_phy", 4), rev_phy_to_rx_("rev_phy_to_rx", 4),
          rev_rx_to_feedback_("rev_rx_to_feedback", 64),
          rev_feedback_to_tx_("rev_feedback_to_tx", 64),
          fwd_tx_("forward_tx_adapter", cfg_, &stats_->forward, ui_, "FWD"),
          fwd_phy_("forward_phy", cfg_, &stats_->forward, ui_),
          fwd_rx_("forward_rx_adapter", cfg_, &stats_->forward, "FWD"),
          fwd_feedback_("forward_feedback", cfg_, ui_),
          rev_tx_("reverse_tx_adapter", reverse_cfg_, &stats_->reverse, ui_, "REV"),
          rev_phy_("reverse_phy", reverse_cfg_, &stats_->reverse, ui_),
          rev_rx_("reverse_rx_adapter", reverse_cfg_, &stats_->reverse, "REV"),
          rev_feedback_("reverse_feedback", reverse_cfg_, ui_) {
        fwd_tx_.fdi_in(soc_tx_in);
        fwd_tx_.phy_out(fwd_tx_to_phy_);
        fwd_tx_.fb_in(fwd_feedback_to_tx_);
        fwd_phy_.in(fwd_tx_to_phy_);
        fwd_phy_.out(fwd_phy_to_rx_);
        fwd_rx_.phy_in(fwd_phy_to_rx_);
        fwd_rx_.fb_out(fwd_rx_to_feedback_);
        fwd_rx_.fdi_out(mem_rx_out);
        fwd_feedback_.in(fwd_rx_to_feedback_);
        fwd_feedback_.out(fwd_feedback_to_tx_);

        rev_tx_.fdi_in(mem_tx_in);
        rev_tx_.phy_out(rev_tx_to_phy_);
        rev_tx_.fb_in(rev_feedback_to_tx_);
        rev_phy_.in(rev_tx_to_phy_);
        rev_phy_.out(rev_phy_to_rx_);
        rev_rx_.phy_in(rev_phy_to_rx_);
        rev_rx_.fb_out(rev_rx_to_feedback_);
        rev_rx_.fdi_out(soc_rx_out);
        rev_feedback_.in(rev_rx_to_feedback_);
        rev_feedback_.out(rev_feedback_to_tx_);

        SC_THREAD(status_thread);
    }

    // Supervisor hook: wake the state process so Failed is observable before
    // the surrounding testbench stops the SystemC kernel.
    void fail() {
        failed_ = true;
        status_event_.notify(sc_core::SC_ZERO_TIME);
    }

private:
    void status_thread() {
        link_state.write(static_cast<unsigned>(LinkState::Training));
        wait(static_cast<double>(cfg_.train_ui) * ui_, status_event_);
        if (failed_) {
            link_state.write(static_cast<unsigned>(LinkState::Failed));
            return;
        }
        link_state.write(static_cast<unsigned>(LinkState::Active));
        for (;;) {
            wait(static_cast<double>(std::max(1U, cfg_.serialize_ui())) * ui_, status_event_);
            if (failed_ || stats_->watchdog_fired) {
                link_state.write(static_cast<unsigned>(LinkState::Failed));
                return;
            } else if (stats_->forward.cdr_lock_loss_count != 0U ||
                       stats_->reverse.cdr_lock_loss_count != 0U ||
                       stats_->forward.deskew_failures != 0U ||
                       stats_->reverse.deskew_failures != 0U) {
                link_state.write(static_cast<unsigned>(LinkState::Degraded));
            }
        }
    }

    const Config& cfg_;
    Config reverse_cfg_;
    Stats* stats_;
    sc_core::sc_time ui_;
    bool failed_ = false;
    sc_core::sc_event status_event_;

    sc_core::sc_fifo<ucie_detail::Frame> fwd_tx_to_phy_;
    sc_core::sc_fifo<ucie_detail::Frame> fwd_phy_to_rx_;
    sc_core::sc_fifo<ucie_detail::FbMsg> fwd_rx_to_feedback_;
    sc_core::sc_fifo<ucie_detail::FbMsg> fwd_feedback_to_tx_;
    sc_core::sc_fifo<ucie_detail::Frame> rev_tx_to_phy_;
    sc_core::sc_fifo<ucie_detail::Frame> rev_phy_to_rx_;
    sc_core::sc_fifo<ucie_detail::FbMsg> rev_rx_to_feedback_;
    sc_core::sc_fifo<ucie_detail::FbMsg> rev_feedback_to_tx_;

    ucie_detail::TxAdapter fwd_tx_;
    ucie_detail::PhyChannel fwd_phy_;
    ucie_detail::RxAdapter fwd_rx_;
    ucie_detail::FeedbackPath fwd_feedback_;
    ucie_detail::TxAdapter rev_tx_;
    ucie_detail::PhyChannel rev_phy_;
    ucie_detail::RxAdapter rev_rx_;
    ucie_detail::FeedbackPath rev_feedback_;
};
