#include "rtl_bridge_systemc.h"

#include "Vstorage_bridge_top.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>

namespace {
constexpr unsigned kPlpBytes = 250;
constexpr unsigned kMcBytes = 32;

unsigned decode_credit(unsigned encoding) {
    static constexpr unsigned amount[] = {0, 1, 4, 8, 16, 32, 64, 128};
    return amount[encoding & 7];
}

void clear_words(WData* words, unsigned count) {
    for (unsigned i = 0; i < count; ++i) words[i] = 0;
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
} // namespace

bridge::MemBackend::Options RtlBridgeSystemC::options(std::uint64_t size) {
    constexpr std::uint64_t kHbm4BytesPerChannel = 1ull << 30;
    bridge::MemBackend::Options opt;
    opt.standard = "hbm4";
    const std::uint64_t required_channels =
        size / kHbm4BytesPerChannel + (size % kHbm4BytesPerChannel != 0);
    opt.channels = int(std::max<std::uint64_t>(1, std::min<std::uint64_t>(
        32, required_channels)));
    opt.transaction_bytes = kMcBytes;
    opt.window_bytes = size;
    opt.retain_command_trace = false;
    return opt;
}

RtlBridgeSystemC::RtlBridgeSystemC(sc_module_name name, std::uint64_t base,
                                   std::uint64_t size)
    : sc_module(name), base_(base), size_(size),
      model_(std::make_unique<Vstorage_bridge_top>()), memory_(options(size)) {
    if (size == 0 || base > UINT64_MAX - (size - 1))
        throw std::invalid_argument("RtlBridgeSystemC invalid address window");
    if (memory_.transaction_bytes() != kMcBytes)
        throw std::runtime_error("RTL and mem_sim transaction widths differ");
    model_->clk = 0;
    model_->rst_n = 0;
    model_->link_active = 0;
    model_->fdi_rx_valid = 0;
    model_->fdi_tx_ready = 0;
    model_->mc_req_ready = 0;
    model_->mc_rsp_valid = 0;
    clear_words(model_->fdi_rx_plp, 63);
    clear_words(model_->mc_rsp_data, 8);
    model_->eval();
    SC_THREAD(run);
    sensitive << clk.pos();
}

RtlBridgeSystemC::~RtlBridgeSystemC() {
    if (!finished_) {
        memory_.finish(0);
        finished_ = true;
    }
    model_->final();
}

bool RtlBridgeSystemC::idle() const {
    return !rx_valid_ && !model_->busy && live_.empty() && responses_.empty() &&
           memory_.quiescent();
}

void RtlBridgeSystemC::finish() {
    if (!finished_) {
        memory_.finish(0);
        finished_ = true;
    }
}

std::string RtlBridgeSystemC::debug_state() const {
    std::ostringstream out;
    out << "busy=" << unsigned(model_->busy)
        << " response_count=" << unsigned(model_->debug_response_count)
        << " head(write/len/wdata/issue/done)=" << unsigned(model_->debug_head_write)
        << '/' << unsigned(model_->debug_head_len)
        << '/' << unsigned(model_->debug_head_wdata)
        << '/' << unsigned(model_->debug_head_issue)
        << '/' << unsigned(model_->debug_head_done)
        << " credits(rdata/wresp)=" << unsigned(model_->debug_available_rdata)
        << '/' << unsigned(model_->debug_available_wresp);
    return out.str();
}

void RtlBridgeSystemC::drive_rx() {
    if (!rx_valid_) {
        FdiFlit incoming;
        if (link_rx.nb_read(incoming)) {
            if (incoming.payload.size() != kPlpBytes ||
                incoming.valid_bytes != kPlpBytes)
                throw std::runtime_error("RTL Bridge requires a 250-byte FDI PLP");
            held_rx_ = std::move(incoming);
            rx_valid_ = true;
        }
    }
    model_->fdi_rx_valid = rx_valid_;
    if (rx_valid_) {
        clear_words(model_->fdi_rx_plp, 63);
        for (unsigned i = 0; i < kPlpBytes; ++i)
            set_byte(model_->fdi_rx_plp, i, held_rx_.payload[i]);
    }
}

void RtlBridgeSystemC::accept_request() {
    model_->mc_req_ready = 1;
    if (!model_->mc_req_valid) return;

    const std::uint64_t tag = model_->mc_req_tag;
    const std::uint64_t address = model_->mc_req_addr;
    const unsigned bytes = model_->mc_req_bytes;
    const bool write = model_->mc_req_write;
    if (bytes == 0 || bytes > kMcBytes || (address % kMcBytes) + bytes > kMcBytes) {
        model_->mc_req_ready = 1;
        responses_.push_back({tag, 2, {}});
        ++synthesized_errors;
        return;
    }
    if (address < base_ || address - base_ > size_ - std::min<std::uint64_t>(size_, bytes)) {
        model_->mc_req_ready = 1;
        responses_.push_back({tag, 3, {}});
        ++synthesized_errors;
        return;
    }

    const std::uint64_t local = address - base_;
    const unsigned offset = unsigned(local % kMcBytes);
    bridge::MemTxn txn;
    txn.write = write;
    txn.address = local - offset;
    txn.bytes = kMcBytes;
    txn.host_request_id = tag;
    if (write) {
        txn.payload.assign(kMcBytes, 0);
        txn.byte_mask.assign(kMcBytes, 0);
        txn.has_byte_mask = true;
        for (unsigned i = 0; i < bytes; ++i) {
            txn.payload[offset + i] = get_byte(model_->mc_req_data, i);
            txn.byte_mask[offset + i] =
                (model_->mc_req_byte_mask >> i) & 1U ? 0xff : 0;
        }
    }
    const int rc = memory_.submit(txn);
    if (rc < 0) throw std::runtime_error(memory_.last_error());
    if (rc > 0) {
        model_->mc_req_ready = 0;
        ++retries;
        return;
    }
    live_.emplace(tag, Meta{tag, offset, bytes, write});
    ++submitted;
}

void RtlBridgeSystemC::advance_memory() {
    memory_.step(8);
    bridge::MemResp response;
    while (memory_.poll(response)) {
        const auto it = live_.find(response.host_request_id);
        if (it == live_.end())
            throw std::runtime_error("mem_sim returned an unknown RTL tag");
        Response compact;
        compact.tag = response.host_request_id;
        compact.status = response.status <= 2 ? 0 : 2;
        if (!it->second.write) {
            compact.data.assign(it->second.bytes, 0);
            for (unsigned i = 0; i < it->second.bytes; ++i)
                if (it->second.offset + i < response.data.size())
                    compact.data[i] = response.data[it->second.offset + i];
        }
        responses_.push_back(std::move(compact));
        live_.erase(it);
    }
}

void RtlBridgeSystemC::drive_response() {
    model_->mc_rsp_valid = !responses_.empty();
    clear_words(model_->mc_rsp_data, 8);
    if (responses_.empty()) return;
    const auto& response = responses_.front();
    model_->mc_rsp_tag = std::uint32_t(response.tag);
    model_->mc_rsp_status = response.status;
    for (unsigned i = 0; i < response.data.size() && i < kMcBytes; ++i)
        set_byte(model_->mc_rsp_data, i, response.data[i]);
}

void RtlBridgeSystemC::run() {
    while (true) {
        try {
            const bool reset = rst_n.read();
            const auto state = static_cast<LinkState>(link_state.read());
            const bool active = state == LinkState::Active || state == LinkState::Degraded;

            model_->clk = 0;
            model_->rst_n = reset;
            model_->link_active = active;
            model_->fdi_tx_ready = active && link_tx.num_free() > 0;
            if (!reset) {
                rx_valid_ = false;
                responses_.clear();
                live_.clear();
                model_->fdi_rx_valid = 0;
                model_->mc_req_ready = 0;
                model_->mc_rsp_valid = 0;
            } else {
                drive_rx();
            }
            model_->eval();

            if (reset) {
                advance_memory();
                drive_response();
                // A request accepted in this RTL cycle must not complete before
                // the rising edge records its tag in the transaction engine.
                accept_request();
                model_->eval();
            }

            const bool rx_fire = model_->fdi_rx_valid && model_->fdi_rx_ready;
            const bool tx_fire = model_->fdi_tx_valid && model_->fdi_tx_ready;
            const bool response_fire = model_->mc_rsp_valid && model_->mc_rsp_ready;
            std::array<std::uint8_t, kPlpBytes> tx_bytes{};
            if (tx_fire)
                for (unsigned i = 0; i < kPlpBytes; ++i)
                    tx_bytes[i] = get_byte(model_->fdi_tx_plp, i);

            model_->clk = 1;
            model_->eval();
            if (model_->protocol_error) {
                std::ostringstream message;
                message << "RTL Bridge protocol_error code=0x" << std::hex
                        << unsigned(model_->protocol_error_code);
                throw std::runtime_error(message.str());
            }
            if (rx_fire) rx_valid_ = false;
            if (response_fire) {
                responses_.pop_front();
                ++completed;
            }
            if (tx_fire) {
                const unsigned message_type = tx_bytes[10] >> 4;
                const unsigned credit = unsigned(tx_bytes[4]) |
                                        (unsigned(tx_bytes[5]) << 8);
                granted_wreq += decode_credit(credit);
                granted_rreq += decode_credit(credit >> 3);
                granted_wdata += decode_credit(credit >> 6);
                if ((tx_bytes[0] & 0xf0) == 0)
                    ++tx_credit_only;
                else if (message_type == 4)
                    ++tx_read_responses;
                else if (message_type == 5)
                    ++tx_write_responses;
                FdiFlit outgoing;
                outgoing.payload.assign(tx_bytes.begin(), tx_bytes.end());
                outgoing.valid_bytes = kPlpBytes;
                outgoing.transaction_id = tx_trace_id_++;
                outgoing.vc = 0;
                outgoing.kind = BusinessKind::Response;
                outgoing.transaction_start = outgoing.fdi_time = sc_time_stamp();
                if (!link_tx.nb_write(outgoing))
                    throw std::runtime_error("RTL FDI TX reservation failed");
            }
        } catch (const std::exception& e) {
            SC_REPORT_FATAL("RtlBridgeSystemC", e.what());
            return;
        }
        wait();
    }
}
