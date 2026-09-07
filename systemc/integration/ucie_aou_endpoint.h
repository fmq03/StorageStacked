/**
 * @file ucie_aou_endpoint.h
 * @brief UCIe 一侧的双向 ready/valid ↔ FDI FIFO 接入模块。
 *
 * AXI2Flit 端口保持不变：出站 flit_out/ready 接 tx/tx_ready，入站接 rx/rx_ready。
 * fifo_tx/fifo_rx 分别连接 UcieLink 的本端输入/输出 FIFO；一个 slot 恰好 250B。
 * TX 在 AXI 时钟沿握手后立即 nb_write，不另加流水寄存器；RX 有一个 holding
 * register，FIFO 取出后下一 AXI 采样沿才能被接收，至少一个 AXI 周期。
 * FIFO 自身的 delta 可见性、跨时钟等待和训练时间另计，不能承诺整体零延迟。
 *
 * 首次进入 Active 后允许工作；Degraded 仍由链路管理纠错/重放。启动训练阶段
 * 不接受新 Flit。一次已经公布的 ready 必须兑现，即使同拍状态发生变化。
 * 当前链路无 reset 端口，只允许协调启动/全系统重建；不能单独热复位本模块。
 */
#pragma once

#include "aou_wire.h"
#include "ucie_common.h"
#include "ucie_fdi.h"
#include <cmath>

// 配置必须从桥的编译参数生成；原 UCIe Config 默认 PAM4 会使速率翻倍。
inline Config make_aou_ucie_config() {
    Config cfg;
    cfg.flit_format = FlitFormat::AouFormat6;
    cfg.num_lanes = LINK_LANES;
    cfg.lane_rate_gtps = LINK_RATE_GTPS;
    cfg.modulation = LINK_BITS_PER_SYMBOL == 1 ? Modulation::NRZ : Modulation::PAM4;
    return cfg;
}

inline void require_aou_ucie_config(const Config& cfg) {
    require_valid_config(cfg);
    if (cfg.flit_format != FlitFormat::AouFormat6 || cfg.num_lanes != LINK_LANES ||
        cfg.bits_per_ui() != LINK_BITS_PER_SYMBOL ||
        !std::isfinite(cfg.lane_rate_gtps) ||
        std::abs(cfg.lane_rate_gtps - LINK_RATE_GTPS) > 1e-9)
        throw std::invalid_argument("UCIe format/rate 与 AXI2Flit 编译配置不一致");
}

SC_MODULE(UcieAouEndpoint) {
    sc_in<bool> clk, rst_n;
    sc_in<unsigned> link_state; // 与 UcieLink::link_state 类型一致，可直接接线/追踪
    sc_in<FlitTransfer> tx;
    sc_out<bool> tx_ready;
    sc_out<FlitTransfer> rx;
    sc_in<bool> rx_ready;
    sc_fifo_out<FdiFlit> fifo_tx;
    sc_fifo_in<FdiFlit> fifo_rx;

    SC_HAS_PROCESS(UcieAouEndpoint);
    UcieAouEndpoint(sc_module_name name, const Config& cfg,
                    BusinessKind direction = BusinessKind::Request)
        : sc_module(name), direction_(direction) {
        require_aou_ucie_config(cfg);
        SC_THREAD(transfer);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

private:
    BusinessKind direction_;
    void transfer() {
        bool ready = false;
        bool held = false;
        uint64_t trace_id = 0;
        tx_ready.write(false);
        rx.write(FlitTransfer{});
        wait();
        while (true) {
            try {
                if (ready && tx.read().valid) {
                    FdiFlit fdi;
                    const auto bytes = serialize_aou(tx.read().flit);
                    fdi.payload.assign(bytes.begin(), bytes.end());
                    fdi.valid_bytes = bytes.size();
                    fdi.transaction_id = trace_id++;
                    fdi.vc = 0;
                    fdi.kind = direction_;
                    fdi.transaction_start = fdi.fdi_time = sc_time_stamp();
                    if (!fifo_tx.nb_write(fdi))
                        throw std::runtime_error("FDI FIFO 违反单写者容量预约");
                }
                if (held && rx_ready.read()) { held = false; rx.write(FlitTransfer{}); }
                const auto state = static_cast<LinkState>(link_state.read());
                const bool active = state == LinkState::Active || state == LinkState::Degraded;
                if (active && !held) {
                    FdiFlit fdi;
                    if (fifo_rx.nb_read(fdi)) {
                        if (fdi.payload.size() != 250 || fdi.valid_bytes != 250)
                            throw std::invalid_argument("入站 AoU FDI 必须恰好 250B");
                        AouWireFlit bytes{};
                        std::copy(fdi.payload.begin(), fdi.payload.end(), bytes.begin());
                        rx.write(FlitTransfer(deserialize_aou(bytes)));
                        held = true;
                    }
                }
                ready = active && fifo_tx.num_free() > 0;
                tx_ready.write(ready);
            } catch (const std::exception& e) {
                SC_REPORT_FATAL("UcieAouEndpoint", e.what());
                return;
            }
            wait();
        }
    }
};
