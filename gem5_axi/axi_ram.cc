#include "axi_ram.hh"
#include <stdexcept>
namespace storage_axi {
Ram::Ram(sc_core::sc_module_name n, uint64_t b, unsigned s, unsigned l, bool st)
    : sc_module(n), base(b), latency(l), stalls(st), bytes(s, 0) {
    if (!s || !l) throw std::runtime_error("RAM size/latency must be positive");
    SC_METHOD(tick); sensitive << clk.pos(); dont_initialize();
}
bool Ram::contains(uint64_t a, unsigned n) const {
    return a >= base && a - base <= bytes.size() && n <= bytes.size() - (a - base);
}
unsigned Ram::access(tlm::tlm_generic_payload& gp) {
    if (!contains(gp.get_address(), gp.get_data_length())) {
        gp.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); return 0;
    }
    if (!(gp.is_read() || gp.is_write())) {
        gp.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE); return 0;
    }
    for (unsigned i = 0; i < gp.get_data_length(); ++i) {
        if (gp.get_byte_enable_ptr() && !gp.get_byte_enable_ptr()[i % gp.get_byte_enable_length()]) continue;
        auto& b = bytes[gp.get_address() - base + i];
        if (gp.is_read()) gp.get_data_ptr()[i] = b;
        else b = gp.get_data_ptr()[i];
    }
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
    return gp.get_data_length();
}
void Ram::tick() {
    ++cycle;
    if (!resetn.read()) {
        sc_assert(writes.empty() && reads.empty() && data.empty() && responses.empty());
        axi.awready = false; axi.wready = false; axi.arready = false;
        axi.bvalid = false; axi.rvalid = false; return;
    }
    if (axi.bvalid.read() && axi.bready.read()) responses.pop_front();
    if (axi.rvalid.read() && axi.rready.read()) {
        if (++reads.front().seen == reads.front().beats) reads.pop_front();
    }
    if (axi.awvalid.read() && axi.awready.read()) {
        sc_assert(axi.awburst.read() == 1 && axi.awsize.read() <= DataSize);
        writes.push_back({axi.awaddr.read().to_uint64(), axi.awid.read().to_uint(),
                          axi.awlen.read().to_uint() + 1, axi.awsize.read().to_uint()});
    }
    if (axi.arvalid.read() && axi.arready.read()) {
        sc_assert(axi.arburst.read() == 1 && axi.arsize.read() <= DataSize);
        reads.push_back({axi.araddr.read().to_uint64(), axi.arid.read().to_uint(),
                         axi.arlen.read().to_uint() + 1, axi.arsize.read().to_uint(), 0,
                         cycle + latency});
    }
    if (axi.wvalid.read() && axi.wready.read())
        data.push_back({axi.wdata.read(), axi.wstrb.read().to_uint(), axi.wlast.read()});
    if (!writes.empty() && !data.empty()) {
        auto& w = writes.front(); auto beat = data.front(); data.pop_front();
        unsigned count = 1u << w.size;
        uint64_t addr = w.address + w.seen * count;
        unsigned lane = addr % DataBytes;
        sc_assert(beat.last == (w.seen + 1 == w.beats));
        sc_assert((beat.strb & ~(((uint64_t(1) << count) - 1) << lane)) == 0);
        if (!contains(addr, count)) w.error = true;
        else for (unsigned j = 0; j < count; ++j)
            if (beat.strb & (uint32_t(1) << (lane + j)))
                bytes[addr - base + j] = beat.data.range(8*(lane+j)+7, 8*(lane+j)).to_uint();
        if (++w.seen == w.beats) {
            responses.push_back({w.id, w.error ? 3u : 0u, cycle + latency});
            writes.pop_front();
        }
    }
    // Hold the whole R payload under backpressure, including the data snapshot.
    if (!(axi.rvalid.read() && !axi.rready.read())) {
        axi.rvalid = false;
        if (!reads.empty() && reads.front().ready <= cycle) {
            const auto& r = reads.front(); unsigned count = 1u << r.size;
            uint64_t addr = r.address + r.seen * count;
            Data value = 0;
            bool ok = contains(addr, count);
            if (ok) for (unsigned j = 0; j < count; ++j)
                value.range(8*(addr%DataBytes+j)+7, 8*(addr%DataBytes+j)) = bytes[addr-base+j];
            axi.rid = r.id; axi.rdata = value; axi.rresp = ok ? 0 : 3;
            axi.rlast = r.seen + 1 == r.beats; axi.rvalid = true;
        }
    }
    axi.bvalid = false;
    if (!responses.empty() && responses.front().ready <= cycle) {
        axi.bid = responses.front().id; axi.bresp = responses.front().resp; axi.bvalid = true;
    }
    axi.awready = writes.size() + responses.size() < 16 && (!stalls || cycle % 5 >= 2);
    axi.wready = data.size() < 512 && (!stalls || cycle % 4 != 1);
    axi.arready = reads.size() < 16 && (!stalls || cycle % 6 >= 2);
}
}
