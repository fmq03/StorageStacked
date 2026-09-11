/**
 * 桥边界自检。覆盖资源平面配置、协调复位、背压及非法AXI入口请求。
 */
#include "axi2flit.h"
#include "aou_wire.h"
#include "aou_stream_decoder.h"
#include <random>
#include <stdexcept>

struct Harness {
    sc_clock clk{"clk", 2, SC_NS, 0.5, 1, SC_NS};
    sc_signal<bool> rst, av, ar, wv, wr, rv, rr, bv, br, arv, arr, txr, rxr;
    sc_signal<AxChannel> aw, axr;
    sc_signal<WChannel> w;
    sc_signal<RChannel> r;
    sc_signal<BChannel> b;
    sc_signal<FlitTransfer> tx, rx;
    Axi2Flit dut;
    AouStreamDecoder scanner;
    std::vector<AouMessage> observed;
    unsigned r_count = 0, b_count = 0;
    unsigned rp_count;

    explicit Harness(unsigned rp) : dut("dut", rp), rp_count(rp) {
        dut.clk(clk); dut.rst_n(rst);
        dut.aw_valid(av); dut.aw_ready(ar); dut.aw_ch(aw);
        dut.w_valid(wv); dut.w_ready(wr); dut.w_ch(w);
        dut.ar_valid(arv); dut.ar_ready(arr); dut.ar_ch(axr);
        dut.r_valid(rv); dut.r_ready(rr); dut.r_ch(r);
        dut.b_valid(bv); dut.b_ready(br); dut.b_ch(b);
        dut.flit_out(tx); dut.flit_ready(txr); dut.flit_in(rx); dut.flit_in_ready(rxr);
    }
    static void require(bool ok, const char* what) {
        if (!ok) throw std::runtime_error(what);
    }
    void tick() {
        if (sc_pending_activity_at_current_time()) sc_start(SC_ZERO_TIME);
        if (rst.read()) {
            if (tx.read().valid && txr.read()) {
                auto decoded = scanner.consume(deserialize_aou(serialize_aou(tx.read().flit)));
                observed.insert(observed.end(), decoded.messages.begin(), decoded.messages.end());
            }
            if (rv.read() && rr.read()) ++r_count;
            if (bv.read() && br.read()) ++b_count;
        }
        sc_start(2, SC_NS);
    }
    void cycles(unsigned n) { while (n--) tick(); }
    template<class Pred> void until(Pred pred, const char* what, unsigned limit = 500) {
        for (unsigned i = 0; i < limit; ++i) {
            if (sc_pending_activity_at_current_time()) sc_start(SC_ZERO_TIME);
            if (pred()) return;
            tick();
        }
        throw std::runtime_error(what);
    }
    void reset() {
        rst.write(false); av.write(false); arv.write(false); wv.write(false);
        rx.write(FlitTransfer{}); txr.write(false); rr.write(false); br.write(false);
        cycles(3);
        scanner.reset(); observed.clear(); r_count = b_count = 0;
        rst.write(true); cycles(3);
    }
    void send_ax(AxChannel a, bool read) {
        (read ? axr : aw).write(a); (read ? arv : av).write(true);
        until([&] { return (read ? arr : ar).read(); }, "AXI address handshake timeout");
        tick(); (read ? arv : av).write(false); tick();
    }
    void send_w(bool last) {
        WChannel beat; beat.last = last;
        std::fill_n(beat.strb, AXI_DATA_BYTES, 1);
        w.write(beat); wv.write(true);
        until([&] { return wr.read(); }, "AXI W handshake timeout");
        tick(); wv.write(false); tick();
    }
    void inbound(const AouFlit& f) {
        rx.write(FlitTransfer(deserialize_aou(serialize_aou(f))));
        until([&] { return rxr.read(); }, "Flit RX timeout");
        tick(); rx.write(FlitTransfer{}); tick();
    }
    void grants() {
        CreditMatrix matrix{};
        for (unsigned rp = 0; rp < rp_count; ++rp)
            for (auto kind : {CreditKind::WriteReq, CreditKind::ReadReq, CreditKind::WriteData})
                matrix[rp][credit_kind_index(kind)] = 128;
        AouFlit f; f.pack_message(build_crdt_grant_message(matrix, rp_count)); inbound(f);
    }
    unsigned count(MsgType type) const {
        return std::count_if(observed.begin(), observed.end(),
                             [&](const auto& m) { return m.type == type; });
    }
};

static void reset_and_backpressure(Harness& h) {
    h.reset();
    // 尚未授予请求 credit，旧 AW/AR/W 堵在 staging/FIFO；响应堵在 AXI 输出，
    // 并在尾部留下一个不完整 ReadData。复位后这几类状态均不得泄漏。
    AxChannel a; a.id = 7;
    h.send_ax(a, false); h.send_ax(a, true); h.send_w(true);
    RChannel r; r.id = 7; r.last = true;
    BChannel b; b.id = 7;
    AouFlit replies;
    replies.pack_message(MsgBuilder::build_read_data(r, 0));
    replies.pack_message(MsgBuilder::build_write_resp(b, 0));
    h.inbound(replies);
    h.until([&] { return h.rv.read() && h.bv.read(); }, "pre-reset R/B not held");
    AouFlit fragment; fragment.used_granules = 47;
    fragment.pack_fragment(MsgBuilder::build_read_data(r, 0), 0); h.inbound(fragment);
    h.reset();
    h.txr.write(true); h.rr.write(true); h.br.write(true); h.cycles(120);
    Harness::require(h.r_count == 0 && h.b_count == 0, "stale response after reset");
    Harness::require(h.count(MsgType::ReadReq) == 0 && h.count(MsgType::WriteReq) == 0 &&
                     h.count(MsgType::WriteDataFull) == 0, "stale request after reset");
    CreditMatrix initial{};
    for (const auto& m : h.observed) {
        CreditMatrix part{};
        if (decode_crdt_grant_message(m, h.rp_count, part))
            for (unsigned rp = 0; rp < h.rp_count; ++rp)
                for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k) initial[rp][k] += part[rp][k];
    }
    for (unsigned rp = 0; rp < h.rp_count; ++rp)
        Harness::require(initial[rp][credit_kind_index(CreditKind::ReadData)] == RX_RDATA_CREDITS_PER_RP &&
                         initial[rp][credit_kind_index(CreditKind::WriteResp)] == RX_WRESP_CREDITS_PER_RP,
                         "initial credit not republished exactly once");

    // 复位后用相同 ID 访问另一资源平面，确认顺序跟踪状态已清空。
    a.qos = h.rp_count - 1; a.addr = 0x2000;
    h.send_ax(a, false); h.send_ax(a, true); h.send_w(true);
    Harness::require(h.dut.order_violations() == 0, "RP order state survived reset");
    h.cycles(20);
    Harness::require(h.count(MsgType::ReadReq) == 0, "old TX credit survived reset");
    h.grants();
    h.until([&] { return h.count(MsgType::ReadReq) == 1 && h.count(MsgType::WriteReq) == 1 &&
                        h.count(MsgType::WriteDataFull) == 1; }, "post-reset traffic stalled");
    h.cycles(30);
    Harness::require(h.count(MsgType::ReadReq) == 1 && h.count(MsgType::WriteReq) == 1 &&
                     h.count(MsgType::WriteDataFull) == 1, "old staged/FIFO traffic leaked");
    AouFlit fresh;
    fresh.pack_message(MsgBuilder::build_read_data(r, a.qos));
    fresh.pack_message(MsgBuilder::build_write_resp(b, a.qos)); h.inbound(fresh);
    h.until([&] { return h.r_count == 1 && h.b_count == 1; }, "post-reset R/B stalled");

    // 固定种子随机 TX/RX 背压；逐项检查出站请求和入站响应，确认仅握手一次。
    std::mt19937 rng(0xBACC);
    for (unsigned i = 0; i < 96; ++i) {
        const unsigned rp = i % h.rp_count;
        const unsigned before = h.count(MsgType::ReadReq);
        h.grants();
        a.id = 32 + i; a.qos = rp; a.addr = 0x10000 + i * 4096ULL;
        h.txr.write(false); h.send_ax(a, true);
        h.until([&] { return h.tx.read().valid; }, "TX output not produced");
        const auto held = h.tx.read();
        for (unsigned n = 1 + rng() % 8; n; --n) {
            h.tick(); Harness::require(h.tx.read() == held, "TX changed while stalled");
        }
        h.txr.write(true);
        h.until([&] { return h.count(MsgType::ReadReq) == before + 1; }, "TX request lost");
        auto it = std::find_if(h.observed.rbegin(), h.observed.rend(),
                              [](const auto& m) { return m.type == MsgType::ReadReq; });
        AxChannel got;
        Harness::require(it != h.observed.rend() && MsgDecoder::decode_req(*it, got) &&
                         got.id == a.id && got.addr == a.addr && it->rp == rp, "TX fields/RP mismatch");
        r.id = a.id; r.last = true;
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) r.data[j] = uint8_t(i + j * 7);
        h.rr.write(false);
        AouFlit response; response.pack_message(MsgBuilder::build_read_data(r, rp)); h.inbound(response);
        h.until([&] { return h.rv.read(); }, "RX response not produced");
        const RChannel saved = h.r.read();
        for (unsigned n = 1 + rng() % 8; n; --n) {
            h.tick(); Harness::require(h.rv.read() && h.r.read() == saved, "R changed while stalled");
        }
        Harness::require(saved.id == r.id && std::equal(r.data, r.data + AXI_DATA_BYTES, saved.data),
                         "R payload mismatch");
        const unsigned count = h.r_count;
        h.rr.write(true); h.until([&] { return h.r_count == count + 1; }, "R handshake lost");
    }
    h.cycles(30);
    Harness::require(h.r_count == 97 && h.b_count == 1 && h.count(MsgType::ReadReq) == 97 &&
                     h.dut.order_violations() == 0, "random stream conservation failed");

    // 每半包两条 CrdtGrant 是合法上限。RP=4 时产生 4*4*5=80 个事件，
    // 超过内部 credit FIFO 的64深度；后续 marker 必须既不丢失也不重复。
    h.reset(); h.txr.write(true); h.br.write(true); h.rr.write(true); h.cycles(120);
    CreditMatrix matrix{};
    for (unsigned rp = 0; rp < h.rp_count; ++rp)
        for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k) matrix[rp][k] = 8;
    const auto grant = build_crdt_grant_message(matrix, h.rp_count);
    AouFlit many;
    many.pack_message(grant); many.pack_message(grant);
    many.used_granules = 24;
    many.pack_message(grant); many.pack_message(grant);
    AouFlit marker; marker.pack_message(MsgBuilder::build_write_resp(b, 0));
    // 紧邻发送两包，不插入空闲拍，才能检出阻塞 write 导致旧 ready 未撤销的问题。
    h.rx.write(FlitTransfer(deserialize_aou(serialize_aou(many))));
    h.until([&] { return h.rxr.read(); }, "credit pressure RX timeout"); h.tick();
    h.rx.write(FlitTransfer(deserialize_aou(serialize_aou(marker))));
    h.until([&] { return h.rxr.read(); }, "credit pressure marker timeout"); h.tick();
    h.rx.write(FlitTransfer{}); h.tick();
    h.until([&] { return h.b_count == 1; }, "credit event FIFO pressure lost following flit");
    h.cycles(20);
    Harness::require(h.b_count == 1, "credit event FIFO pressure duplicated following flit");
    // 最后一个 RP 的 RREQ credit 位于事件队列尾部；32gr 恰好允许10条请求，
    // 第11条还差1gr。少发/重复credit都会改变这个可观测边界。
    a.qos = h.rp_count - 1;
    for (unsigned i = 0; i < 11; ++i) { a.id = 200 + i; h.send_ax(a, true); }
    h.cycles(100);
    Harness::require(h.count(MsgType::ReadReq) == 10, "credit events lost or duplicated");
    AouFlit last_credit;
    last_credit.msg_credit = uint16_t((h.rp_count - 1) << 14) | (1U << 3);
    h.inbound(last_credit);
    h.until([&] { return h.count(MsgType::ReadReq) == 11; }, "credit remainder not conserved");
}

int sc_main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "normal";
    const unsigned rp = argc > 2 ? unsigned(std::stoul(argv[2])) : 1;
    sc_report_handler::set_actions(SC_FATAL, SC_THROW);
    g_aou_verbose = false;
    Harness h(rp);
    try {
        if (mode == "normal") reset_and_backpressure(h);
        else {
            h.reset();
            AxChannel a;
            if (mode.find("fixed") != std::string::npos) a.burst = 0;
            else if (mode.find("wrap") != std::string::npos) a.burst = 2;
            else if (mode.find("size") != std::string::npos) a.size = AXI_SIZE_CODE + 1;
            else if (mode.find("align") != std::string::npos) a.addr = 1;
            else if (mode.find("4k") != std::string::npos) { a.addr = 4096 - AXI_DATA_BYTES; a.len = 1; }
            else if (mode == "w_early") a.len = 1;
            else if (mode != "w_missing") throw std::runtime_error("unknown negative test");
            h.send_ax(a, mode.rfind("ar_", 0) == 0);
            if (mode == "w_early" || mode == "w_missing") h.send_w(mode == "w_early");
            throw std::runtime_error("illegal AXI was not rejected at handshake");
        }
    } catch (const sc_report& report) {
        if (mode != "normal" && std::string(report.get_msg_type()) == "Axi2Flit/AXI") {
            std::cout << "BOUNDARY " << mode << ": expected AXI rejection PASS\n"; return 0;
        }
        std::cerr << report.what() << '\n'; return 1;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << mode << ": " << error.what() << '\n'; return 1;
    }
    std::cout << "BOUNDARY " << AXI_DATA_WIDTH << "b RP=" << rp << ": PASS\n";
    return 0;
}
