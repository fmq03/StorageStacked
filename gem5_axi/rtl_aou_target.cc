#include "rtl_aou_target.hh"

#define VL_TIME_CONTEXT
#define VM_SC 0
#define VM_TRACE 0
#include "Vstorage_bridge_top.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace storage_axi {
namespace {

void clear_words(WData* words, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        words[i] = 0;
}

void set_byte(WData* words, unsigned byte, uint8_t value)
{
    const unsigned word = byte / 4;
    const unsigned shift = 8 * (byte % 4);
    words[word] = (words[word] & ~(0xffU << shift)) |
                  (uint32_t(value) << shift);
}

uint8_t get_byte(const WData* words, unsigned byte)
{
    return uint8_t(words[byte / 4] >> (8 * (byte % 4)));
}

bool power_of_two(unsigned value)
{
    return value && !(value & (value - 1));
}

unsigned size_code(unsigned bytes)
{
    unsigned size = 0;
    while ((1U << size) < bytes)
        ++size;
    return size;
}

} // namespace

RtlAouTarget::RtlAouTarget(sc_core::sc_module_name name, unsigned rp_count)
    : sc_module(name), model(std::make_unique<Vstorage_bridge_top>()),
      rp_count(rp_count)
{
    if (rp_count < 1 || rp_count > 4)
        throw std::invalid_argument("RTL Bridge supports one to four RPs");
    model->clk = 0;
    model->rst_n = 0;
    model->link_active = 0;
    model->fdi_rx_valid = 0;
    model->fdi_tx_ready = 0;
    model->mc_req_ready = 0;
    model->mc_rsp_valid = 0;
    clear_words(model->fdi_rx_plp, 63);
    clear_words(model->mc_rsp_data, 8);
    model->eval();
    SC_THREAD(run);
    sensitive << clk.pos();
}

RtlAouTarget::~RtlAouTarget()
{
    model->final();
}

bool
RtlAouTarget::idle() const
{
    return !rx_valid && !rsp_valid && live_count == 0 && !model->busy &&
           link_rx.num_available() == 0 && mem_rsp.num_available() == 0;
}

unsigned
RtlAouTarget::available_id() const
{
    for (unsigned n = 0; n < IdCount - 1; ++n) {
        const unsigned id = 1 + ((next_id - 1 + n) % (IdCount - 1));
        if (!live[id].valid)
            return id;
    }
    return 0;
}

void
RtlAouTarget::drive_rx()
{
    if (!rx_valid) {
        FdiFlit incoming;
        if (link_rx.nb_read(incoming)) {
            if (incoming.payload.size() != PlpBytes ||
                incoming.valid_bytes != PlpBytes)
                throw std::runtime_error("RTL Bridge requires a 250-byte FDI PLP");
            held_rx = std::move(incoming);
            rx_valid = true;
        }
    }
    model->fdi_rx_valid = rx_valid;
    clear_words(model->fdi_rx_plp, 63);
    if (rx_valid) {
        for (unsigned i = 0; i < PlpBytes; ++i)
            set_byte(model->fdi_rx_plp, i, held_rx.payload[i]);
    }
}

void
RtlAouTarget::drive_response()
{
    if (!rsp_valid)
        rsp_valid = mem_rsp.nb_read(held_rsp);
    model->mc_rsp_valid = rsp_valid;
    clear_words(model->mc_rsp_data, 8);
    if (!rsp_valid)
        return;
    if (held_rsp.id == 0 || held_rsp.id >= IdCount || !live[held_rsp.id].valid)
        throw std::runtime_error("memory returned an unknown RTL Bridge ID");
    const auto &meta = live[held_rsp.id];
    if (held_rsp.write != meta.write ||
        held_rsp.read_beats.size() != (meta.write ? 0U : 1U))
        throw std::runtime_error("memory response shape does not match RTL request");
    model->mc_rsp_tag = meta.rtl_tag;
    model->mc_rsp_status = meta.write ? held_rsp.resp : held_rsp.read_beats[0].resp;
    if (!meta.write) {
        const auto &data = held_rsp.read_beats[0].data;
        for (unsigned i = 0; i < meta.bytes; ++i)
            set_byte(model->mc_rsp_data, i, data[meta.lane + i]);
    }
}

void
RtlAouTarget::submit_request(unsigned id)
{
    const uint64_t address = model->mc_req_addr;
    const unsigned bytes = model->mc_req_bytes;
    const unsigned lane = address % DataBytes;
    if (!id || !bytes || bytes > DataBytes || lane + bytes > DataBytes)
        throw std::runtime_error("invalid transaction emitted by RTL Bridge");

    SimpleMemRequest req;
    req.write = model->mc_req_write;
    req.rp = uint8_t(model->mc_req_qos % rp_count);
    req.address.id = id;
    // Preserve native AXI-shaped chunks in logs.  Only a compact chunk that
    // AXI AxSIZE cannot represent (for example 12/20/24 bytes) needs an
    // aligned 32-byte container; lane/bytes retain its actual subrange.
    const bool needs_container = !power_of_two(bytes);
    req.address.addr = needs_container ? address - lane : address;
    req.address.len = 0;
    req.address.size = needs_container ? 5 : size_code(bytes);
    req.address.burst = 1;
    req.address.qos = model->mc_req_qos;
    if (req.write) {
        SimpleMemWriteBeat beat;
        for (unsigned i = 0; i < bytes; ++i) {
            beat.data[lane + i] = get_byte(model->mc_req_data, i);
            beat.strobe[lane + i] = (model->mc_req_byte_mask >> i) & 1U;
        }
        req.write_beats.push_back(beat);
    }
    if (!mem_req.nb_write(req))
        throw std::runtime_error("reserved memory FIFO entry disappeared");

    live[id] = Live{true, req.write, uint32_t(model->mc_req_tag),
                    uint8_t(lane), uint8_t(bytes)};
    next_id = id == IdCount - 1 ? 1 : id + 1;
    ++live_count;
    ++submitted;
    if (req.write) {
        ++writes;
        ++write_beats;
    } else {
        ++reads;
        ++read_beats;
    }
}

void
RtlAouTarget::retire_response()
{
    const unsigned id = held_rsp.id;
    live[id] = Live{};
    --live_count;
    ++completed;
    rsp_valid = false;
}

void
RtlAouTarget::transmit(const std::array<uint8_t, PlpBytes> &bytes)
{
    FdiFlit outgoing;
    outgoing.payload.assign(bytes.begin(), bytes.end());
    outgoing.valid_bytes = PlpBytes;
    outgoing.transaction_id = tx_trace_id++;
    outgoing.vc = 0;
    outgoing.kind = BusinessKind::Response;
    outgoing.transaction_start = outgoing.fdi_time = sc_core::sc_time_stamp();
    if (!link_tx.nb_write(outgoing))
        throw std::runtime_error("reserved RTL FDI TX entry disappeared");
}

void
RtlAouTarget::run()
{
    while (true) {
        try {
            const bool reset = rst_n.read();
            const auto state = static_cast<LinkState>(link_state.read());
            const bool active = state == LinkState::Active ||
                                state == LinkState::Degraded;

            model->clk = 0;
            model->rst_n = reset;
            model->link_active = active;
            model->fdi_tx_ready = active && link_tx.num_free() > 0;
            if (!reset) {
                rx_valid = false;
                rsp_valid = false;
                live.fill(Live{});
                live_count = 0;
                next_id = 1;
                model->fdi_rx_valid = 0;
                model->mc_rsp_valid = 0;
            } else {
                drive_rx();
                drive_response();
            }

            const unsigned id = reset ? available_id() : 0;
            model->mc_req_ready = reset && id && mem_req.num_free() > 0;
            if (reset && model->mc_req_valid && !model->mc_req_ready)
                ++memory_stalls;
            model->eval();

            const bool rx_fire = model->fdi_rx_valid && model->fdi_rx_ready;
            const bool tx_fire = model->fdi_tx_valid && model->fdi_tx_ready;
            const bool req_fire = model->mc_req_valid && model->mc_req_ready;
            const bool rsp_fire = model->mc_rsp_valid && model->mc_rsp_ready;
            std::array<uint8_t, PlpBytes> tx_bytes{};
            if (tx_fire) {
                for (unsigned i = 0; i < PlpBytes; ++i)
                    tx_bytes[i] = get_byte(model->fdi_tx_plp, i);
            }
            if (req_fire)
                submit_request(id);

            model->clk = 1;
            model->eval();
            if (model->protocol_error) {
                std::ostringstream message;
                message << "RTL Bridge protocol error 0x" << std::hex
                        << unsigned(model->protocol_error_code);
                throw std::runtime_error(message.str());
            }
            if (rx_fire)
                rx_valid = false;
            if (rsp_fire)
                retire_response();
            if (tx_fire)
                transmit(tx_bytes);
        } catch (const std::exception &e) {
            SC_REPORT_FATAL("RtlAouTarget", e.what());
            return;
        }
        wait();
    }
}

} // namespace storage_axi
