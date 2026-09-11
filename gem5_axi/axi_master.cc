#include "axi_master.hh"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace storage_axi {
using namespace sc_core;
Master::Master(sc_module_name n, unsigned s, bool st, const std::string& dir)
    : sc_module(n), slots(s), stalls(st), trace(dir + "/transactions.csv") {
    if (slots == 0 || slots > 65534 || !trace) throw std::runtime_error("invalid master configuration");
    trace << "id,command,address,bytes,begin_tick,accepted_tick,axi_done_tick,end_resp_tick,segments,requestor,stream,substream,status\n";
    socket.register_nb_transport_fw(this, &Master::transport);
    socket.register_b_transport(this, &Master::blocking);
    socket.register_transport_dbg(this, &Master::debug);
    socket.register_get_direct_mem_ptr(this, &Master::dmi);
    SC_METHOD(tick);
    sensitive << clk.pos();
    dont_initialize();
}

std::vector<Master::Burst> Master::split(uint64_t addr, unsigned len) {
    std::vector<Burst> out;
    unsigned offset = 0;
    while (offset < len) {
        unsigned remaining = len - offset;
        unsigned beat = DataBytes, size = DataSize;
        while (beat > remaining || addr % beat) { beat /= 2; --size; }
        unsigned beats = std::min({remaining / beat, 256u,
                                  unsigned((4096 - (addr & 4095)) / beat)});
        out.push_back({addr, offset, beat, beats, size});
        unsigned bytes = beat * beats;
        addr += bytes; offset += bytes;
    }
    return out;
}

tlm::tlm_sync_enum Master::transport(tlm::tlm_generic_payload& gp,
    tlm::tlm_phase& phase, sc_time& delay) {
    if (phase == tlm::BEGIN_REQ) {
        sc_assert(pending == nullptr);
        sc_assert(gp.has_mm());
        gp.acquire();
        pending = &gp;
        pendingBegin = sc_time_stamp().value() + delay.value();
        auto* attrs = gp.get_extension<RequestAttributes>();
        // gem5 clears payloadDelay after packet2payload; preserve it in the
        // conversion hook and conservatively wait for the full payload.
        pendingAt = sc_time::from_value(pendingBegin + (attrs ? attrs->payloadDelay : 0));
        return tlm::TLM_ACCEPTED;
    }
    sc_assert(phase == tlm::END_RESP && responding == &gp);
    sc_assert(delay == SC_ZERO_TIME); // contract with bundled Gem5ToTlmBridge
    auto it = std::find_if(active.begin(), active.end(),
                          [&gp](const auto& p) { return p.second->gp == &gp; });
    sc_assert(it != active.end());
    Txn& t = *it->second;
    auto* a = gp.get_extension<RequestAttributes>();
    trace << t.id << ',' << (gp.is_write() ? 'W' : 'R') << ',' << gp.get_address()
          << ',' << gp.get_data_length() << ',' << t.begin << ',' << t.accept << ','
          << t.axiDone << ',' << sc_time_stamp().value() << ',' << t.bursts.size()
          << ',' << (a ? a->requestor : 0) << ',' << (a ? a->stream : 0)
          << ',' << (a ? a->substream : 0) << ',' << int(gp.get_response_status()) << '\n';
    trace.flush();
    responding = nullptr;
    active.erase(it);
    ++completed;
    gp.release();
    return tlm::TLM_COMPLETED;
}

unsigned Master::debug(tlm::tlm_generic_payload& gp) {
    // This first version permits debug/atomic access only while drained.
    sc_assert(idle() && functional);
    return functional(gp);
}
void Master::blocking(tlm::tlm_generic_payload& gp, sc_time&) { debug(gp); }

void Master::admit() {
    if (!pending || active.size() >= slots || pendingAt >= sc_time_stamp()) return;
    auto* gp = pending;
    pending = nullptr;
    sc_assert(maxId > 0 && maxId <= 65535 && slots <= maxId);
    if (!nextId || nextId > maxId) nextId = 1;
    while (active.count(nextId)) nextId = nextId == maxId ? 1 : nextId + 1;
    auto t = std::make_unique<Txn>();
    t->gp = gp; t->id = nextId++;
    t->begin = pendingBegin; t->accept = sc_time_stamp().value();
    bool valid = (gp->is_read() || gp->is_write()) && gp->get_data_ptr() &&
                 gp->get_data_length() && gp->get_data_length() <= 65536 &&
                 gp->get_address() <= std::numeric_limits<uint64_t>::max() - gp->get_data_length();
    if (gp->get_streaming_width() < gp->get_data_length()) valid = false;
    if (gp->get_byte_enable_ptr() && !gp->get_byte_enable_length()) valid = false;
    gp->set_response_status(valid ? tlm::TLM_OK_RESPONSE : tlm::TLM_BURST_ERROR_RESPONSE);
    if (valid) t->bursts = split(gp->get_address(), gp->get_data_length());
    uint16_t id = t->id;
    active.emplace(id, std::move(t));
    ++accepted;
    maxActive = std::max<uint64_t>(maxActive, active.size());
    tlm::tlm_phase p = tlm::END_REQ;
    sc_time d = SC_ZERO_TIME;
    socket->nb_transport_bw(*gp, p, d);
    if (valid) enqueueSegment(*active.at(id));
    else { active.at(id)->axiDone = sc_time_stamp().value(); responses.push_back(id); }
}

void Master::enqueueSegment(Txn& t) {
    t.wbeat = t.rbeat = 0;
    // Alternate AW-leading and W-leading transfers. W order remains FIFO.
    t.awAfter = cycle + (stalls && (t.id & 1) ? 3 : 0);
    t.wAfter = cycle + (stalls && !(t.id & 1) ? 3 : 0);
    t.arAfter = cycle;
    if (t.gp->is_write()) { awq.push_back(t.id); wq.push_back(t.id); }
    else arq.push_back(t.id);
}
void Master::finishSegment(Txn& t) {
    if (++t.segment < t.bursts.size()) enqueueSegment(t);
    else { t.axiDone = sc_time_stamp().value(); responses.push_back(t.id); }
}

void Master::tick() {
    ++cycle;
    if (!resetn.read()) {
        sc_assert(active.empty());
        axi.awvalid = false; axi.wvalid = false; axi.arvalid = false;
        axi.rready = false; axi.bready = false;
        return;
    }
    if (axi.awvalid.read() && axi.awready.read()) { sc_assert(!awq.empty()); awq.pop_front(); }
    if (axi.arvalid.read() && axi.arready.read()) { sc_assert(!arq.empty()); arq.pop_front(); }
    if (axi.wvalid.read() && axi.wready.read()) {
        auto& t = *active.at(wq.front());
        if (++t.wbeat == t.bursts[t.segment].beats) wq.pop_front();
    }
    if (axi.rvalid.read() && axi.rready.read()) {
        auto& t = *active.at(axi.rid.read().to_uint());
        sc_assert(t.gp->is_read());
        auto& b = t.bursts[t.segment];
        sc_assert(axi.rlast.read() == (t.rbeat + 1 == b.beats));
        if (axi.rresp.read() != 0) t.gp->set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        const Data data = axi.rdata.read();
        unsigned lane = (b.address + t.rbeat * b.bytes) % DataBytes;
        for (unsigned j = 0; j < b.bytes; ++j) {
            unsigned off = b.offset + t.rbeat * b.bytes + j;
            auto* mask = t.gp->get_byte_enable_ptr();
            if (!mask || mask[off % t.gp->get_byte_enable_length()])
                t.gp->get_data_ptr()[off] = data.range(8*(lane+j)+7, 8*(lane+j)).to_uint();
        }
        if (++t.rbeat == b.beats) finishSegment(t);
    }
    if (axi.bvalid.read() && axi.bready.read()) {
        auto& t = *active.at(axi.bid.read().to_uint());
        sc_assert(t.gp->is_write());
        if (axi.bresp.read() != 0) t.gp->set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        finishSegment(t);
    }
    admit();
    drive();
    respond();
}

void Master::drive() {
    axi.awvalid = false;
    if (!awq.empty()) {
        auto& t = *active.at(awq.front()); auto& b = t.bursts[t.segment];
        axi.awaddr = b.address; axi.awid = t.id; axi.awlen = b.beats - 1;
        axi.awsize = b.size; axi.awburst = 1;
        axi.awvalid = cycle >= t.awAfter;
    }
    axi.arvalid = false;
    if (!arq.empty()) {
        auto& t = *active.at(arq.front()); auto& b = t.bursts[t.segment];
        axi.araddr = b.address; axi.arid = t.id; axi.arlen = b.beats - 1;
        axi.arsize = b.size; axi.arburst = 1;
        axi.arvalid = cycle >= t.arAfter;
    }
    axi.wvalid = false;
    if (!wq.empty()) {
        auto& t = *active.at(wq.front()); auto& b = t.bursts[t.segment];
        Data data = 0; uint32_t strb = 0;
        unsigned lane = (b.address + t.wbeat * b.bytes) % DataBytes;
        for (unsigned j = 0; j < b.bytes; ++j) {
            unsigned off = b.offset + t.wbeat * b.bytes + j;
            data.range(8*(lane+j)+7, 8*(lane+j)) = t.gp->get_data_ptr()[off];
            auto* mask = t.gp->get_byte_enable_ptr();
            if (!mask || mask[off % t.gp->get_byte_enable_length()]) strb |= uint32_t(1) << (lane + j);
        }
        axi.wdata = data; axi.wstrb = strb; axi.wlast = t.wbeat + 1 == b.beats;
        axi.wvalid = cycle >= t.wAfter;
    }
    axi.rready = !stalls || cycle % 7 >= 3;
    axi.bready = !stalls || cycle % 5 >= 2;
}
void Master::respond() {
    if (responding || responses.empty()) return;
    responding = active.at(responses.front())->gp;
    responses.pop_front();
    tlm::tlm_phase p = tlm::BEGIN_RESP;
    sc_time d = SC_ZERO_TIME;
    auto result = socket->nb_transport_bw(*responding, p, d);
    sc_assert(result == tlm::TLM_ACCEPTED); // bundled gem5 bridge contract
}
}
