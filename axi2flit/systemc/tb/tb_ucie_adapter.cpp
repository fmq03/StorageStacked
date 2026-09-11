/**
 * 链路适配组件自检。覆盖帧映射、逐位错误检测、深度1的双向FIFO与训练门控。
 */
#include "ucie_aou_adapter.h"
#include "ucie_link.h"
#include <random>

static unsigned pass_count = 0, fail_count = 0;
static void check(bool ok, const char* why) {
    if (ok) ++pass_count;
    else { ++fail_count; std::cerr << "[FAIL] " << why << '\n'; }
}
template<class F> static void rejects(F call, const char* why) {
    bool caught = false;
    try { call(); } catch (const std::exception&) { caught = true; }
    check(caught, why);
}

static AouFlit sample(unsigned i) {
    AouFlit f;
    f.msg_start = (0x0123456789ABULL * (i + 1)) & ((1ULL << 48) - 1);
    f.msg_credit = uint16_t(i * 7919);
    for (unsigned j = 0; j < 240; ++j) f.payload[j] = uint8_t(i * 17 + j * 23);
    return f;
}

static void format_unit(const Config& cfg) {
    const auto wire = serialize_aou(sample(19));
    const std::vector<uint8_t> payload(wire.begin(), wire.end());
    auto built = build_flit(cfg, 0x1AB, payload, false);
    const auto checked = check_flit(cfg, built);
    check(built.size() == 256 && checked.crc_ok && checked.seq8 == 0xAB &&
          !checked.replay_flag, "AouFormat6 长度/序号/CRC");
    ucie_detail::Frame frame;
    frame.bytes = built; frame.valid_bytes = 250;
    check(ucie_detail::unpack_fdi(cfg, frame).payload == payload, "UCIe 接收实际走 gather");
    for (unsigned bit = 0; bit < 2048; ++bit) {
        auto bad = built; bad[bit / 8] ^= 1U << (bit % 8);
        check(!check_flit(cfg, bad).crc_ok, "FH/PH/payload/CRC 全 bit 错误检测");
    }
    built = build_flit(cfg, 0x1AB, payload, true);
    check(check_flit(cfg, built).crc_ok && check_flit(cfg, built).replay_flag,
          "重放头变化后重新计算 CRC");
    check(!check_flit(cfg, std::vector<uint8_t>(255)).crc_ok, "错误物理长度拒绝");
    rejects([&] { build_flit(cfg, 0, std::vector<uint8_t>(249), false); }, "249B PLP 拒绝");
    rejects([&] { build_flit(cfg, 0, std::vector<uint8_t>(251), false); }, "251B PLP 拒绝");
    FdiFlit invalid; invalid.payload = payload; invalid.valid_bytes = 249;
    rejects([&] { ucie_detail::require_valid_fdi(cfg, invalid); }, "AoU valid_bytes=249 拒绝");
    invalid.valid_bytes = 250; invalid.payload.resize(249);
    rejects([&] { ucie_detail::require_valid_fdi(cfg, invalid); }, "AoU FDI 数组短包拒绝");
    // 使用独立的 CCITT 校验串检查链路校验算法。
    const uint8_t canonical[] = {'1','2','3','4','5','6','7','8','9'};
    check(crc16_ccitt(canonical, 9) == 0x29B1, "CRC 行为算法独立黄金值");
    for (auto format : {FlitFormat::Standard256, FlitFormat::Compact68}) {
        Config old = cfg; old.flit_format = format;
        const auto bytes = build_flit(old, 7, std::vector<uint8_t>(old.payload_bytes(), 0xA5), false);
        check(check_flit(old, bytes).crc_ok, "原有格式未被 AoU 分支覆盖");
    }
    Config wrong = cfg; wrong.modulation = cfg.modulation == Modulation::NRZ ? Modulation::PAM4 : Modulation::NRZ;
    rejects([&] { require_aou_ucie_config(wrong); }, "调制配置不一致拒绝");
    wrong = cfg; ++wrong.num_lanes;
    rejects([&] { require_aou_ucie_config(wrong); }, "lane 数不一致拒绝");
    wrong = cfg; wrong.lane_rate_gtps += 1;
    rejects([&] { require_aou_ucie_config(wrong); }, "速率不一致拒绝");
    wrong = cfg; wrong.flit_format = FlitFormat::Standard256;
    rejects([&] { require_aou_ucie_config(wrong); }, "236B 配置拒绝");
    check(std::abs(cfg.num_lanes * cfg.lane_rate_gtps * cfg.bits_per_ui() / 8.0 -
                   LINK_BYTES_PER_NS) < 1e-9, "桥和 UCIe 裸字节速率一致");
}

int sc_main(int, char**) {
    const Config cfg = make_aou_ucie_config();
    format_unit(cfg);
    sc_clock clk("clk", 2, SC_NS, 0.5, 1, SC_NS);
    sc_signal<bool> rst, tx_ready, rx_ready;
    sc_signal<unsigned> state;
    sc_signal<FlitTransfer> tx, rx;
    sc_fifo<FdiFlit> fifo_tx("fifo_tx", 1), fifo_rx("fifo_rx", 1);
    UcieAouAdapter adapter("adapter", cfg);
    adapter.clk(clk); adapter.rst_n(rst); adapter.link_state(state);
    adapter.tx(tx); adapter.tx_ready(tx_ready); adapter.rx(rx); adapter.rx_ready(rx_ready);
    adapter.fifo_tx(fifo_tx); adapter.fifo_rx(fifo_rx);
    rst.write(false); state.write(static_cast<unsigned>(LinkState::Reset));
    sc_start(6, SC_NS);
    rst.write(true); state.write(static_cast<unsigned>(LinkState::Training));
    tx.write(FlitTransfer(sample(0))); rx_ready.write(false);
    sc_start(10, SC_NS);
    check(!tx_ready.read() && fifo_tx.num_available() == 0 && !rx.read().valid,
          "训练期间初始 Flit 不提前进入 FIFO");
    state.write(static_cast<unsigned>(LinkState::Active));
    std::mt19937 rng(0xF1F0);
    constexpr unsigned total = 256;
    unsigned accepted = 0, delivered = 0, injected = 0, received = 0, stalls = 0;
    std::vector<sc_time> tx_handshake_times;
    bool stalled = false;
    FlitTransfer held;
    for (unsigned cycle = 0; cycle < 10000 && (delivered < total || received < total); ++cycle) {
        tx.write(accepted < total ? FlitTransfer(sample(accepted)) : FlitTransfer{});
        rx_ready.write((rng() % 5) == 0);
        state.write(static_cast<unsigned>(cycle >= 200 && cycle < 240 ? LinkState::Degraded : LinkState::Active));
        sc_start(SC_ZERO_TIME);
        if (stalled) check(rx.read() == held, "RX FIFO 背压期间 holding register 稳定");
        if (tx.read().valid && tx_ready.read()) {
            tx_handshake_times.push_back(sc_time_stamp() + sc_time(1, SC_NS));
            ++accepted;
        }
        if (rx.read().valid && rx_ready.read()) {
            check(serialize_aou(rx.read().flit) == serialize_aou(sample(1000 + received)),
                  "入站 250B 顺序与逐字节一致");
            check(rx.read().flit.used_granules == -1, "适配器不传递 used_granules");
            ++received;
        }
        stalled = rx.read().valid && !rx_ready.read();
        held = rx.read(); if (stalled) ++stalls;
        FdiFlit fdi;
        if ((rng() % 5) == 0 && fifo_tx.nb_read(fdi)) {
            const auto expected = serialize_aou(sample(delivered));
            check(fdi.payload == std::vector<uint8_t>(expected.begin(), expected.end()) &&
                  fdi.valid_bytes == 250 && fdi.vc == 0 && fdi.transaction_id == delivered,
                  "出站 FIFO 250B/元数据/顺序");
            check(delivered < tx_handshake_times.size() && fdi.fdi_time == tx_handshake_times[delivered],
                  "TX 在握手沿入 FIFO，无额外流水周期");
            ++delivered;
        }
        if (injected < total && (rng() & 1)) {
            const auto bytes = serialize_aou(sample(1000 + injected));
            fdi.payload.assign(bytes.begin(), bytes.end()); fdi.valid_bytes = 250;
            // 接收不依赖调试元数据，故意填无关值以检验无旁路语义。
            fdi.transaction_id = 999999; fdi.vc = 3;
            if (fifo_rx.nb_write(fdi)) ++injected;
        }
        sc_start(2, SC_NS);
    }
    check(accepted == total && delivered == total && injected == total && received == total,
          "双向各 256 Flit 数量守恒且无死锁");
    check(stalls > 100, "随机用例确实覆盖足够背压");
    tx.write(FlitTransfer{}); rx_ready.write(true); sc_start(20, SC_NS);
    check(!rx.read().valid && fifo_tx.num_available() == 0, "完成后无重复或尾部残留");
    std::cout << "ADAPTER: " << pass_count << " PASS / " << fail_count << " FAIL\n";
    return fail_count ? 1 : 0;
}
