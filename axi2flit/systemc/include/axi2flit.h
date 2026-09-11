/**
 * 请求发起侧双向协议桥。AXI请求经消息队列打包发送，返回消息解码为B/R通道。
 * 资源平面按QOS取模映射，写数据按写地址接收顺序继承路由。
 */
#pragma once

#include "axi_if.h"
#include "aou_types.h"
#include "msg_builder.h"
#include "msg_decoder.h"
#include "flit_packer.h"
#include "flit_unpacker.h"
#include "credit_manager.h"
#include "rp_order_guard.h"
#include "axi_contract.h"

// FIFO / credit 深度常量定义在 aou_types.h（按credit环路时间预算计算，见该文件注释）。

// AXI4 W 不携带 ID/RP，需要按 AW 接收顺序保存写 burst 的路由信息。
struct WriteRoute {
    uint8_t rp = 0;
    unsigned beats_remaining = 0;
};
inline std::ostream& operator<<(std::ostream& os, const WriteRoute& route) {
    return os << "[WriteRoute rp=" << (int)route.rp
              << " beats=" << route.beats_remaining << "]";
}

SC_MODULE(Axi2Flit) {
public:
    sc_in<bool> clk;
    sc_in<bool> rst_n;

    // AXI4 Slave 侧请求通道
    sc_in<bool>      aw_valid;
    sc_out<bool>     aw_ready;
    sc_in<AxChannel> aw_ch;
    sc_in<bool>      w_valid;
    sc_out<bool>     w_ready;
    sc_in<WChannel>  w_ch;
    sc_in<bool>      ar_valid;
    sc_out<bool>     ar_ready;
    sc_in<AxChannel> ar_ch;

    // AXI4 Slave 侧响应通道。valid/数据由本桥驱动，ready 由 AXI Master 驱动。
    sc_out<bool>     b_valid;
    sc_in<bool>      b_ready;
    sc_out<BChannel> b_ch;
    sc_out<bool>     r_valid;
    sc_in<bool>      r_ready;
    sc_out<RChannel> r_ch;

    // 双向 FDI 抽象接口
    sc_out<FlitTransfer> flit_out;
    sc_in<bool>          flit_ready;
    sc_in<FlitTransfer>  flit_in;
    sc_out<bool>         flit_in_ready;

    SC_HAS_PROCESS(Axi2Flit);
    explicit Axi2Flit(sc_module_name name,
                      unsigned rp_count = DEFAULT_RESOURCE_PLANES);

    unsigned rp_count() const { return rp_count_; }

    /**
     * 同 ID 跨 RP 的顺序违例计数（资源平面顺序约束，见 rp_order_guard.h）。
     * testbench 应在仿真结束时检查它为 0；不为 0 说明激励或 SoC 侧的
     * QoS/ID 规划违反了集成约束，桥接单元无法保证 AXI 响应顺序。
     */
    unsigned long order_violations() const {
        return rd_order_guard_.violations() + wr_order_guard_.violations();
    }

private:
    unsigned rp_count_;
    FlitPacker packer;
    FlitUnpacker unpacker;

    // 发送与接收队列均按 RP 分开，保证某一 RP credit 枯竭时不会形成跨 RP 队头阻塞。
    sc_vector<sc_fifo<AouMessage>> sig_rreq_fifo;
    sc_vector<sc_fifo<AouMessage>> sig_wreq_fifo;
    sc_vector<sc_fifo<AouMessage>> sig_wdata_fifo;
    sc_vector<sc_fifo<AouMessage>> sig_rdata_fifo;
    sc_vector<sc_fifo<AouMessage>> sig_wresp_fifo;

    sc_fifo<CreditUpdate> sig_credit_update_fifo;
    sc_fifo<CreditReturn> sig_credit_return_fifo;
    sc_fifo<WriteRoute> sig_write_route_fifo;

    // sc_fifo 没有 reset 端口，复位期间必须显式丢弃全部旧事务与 credit。
    void reset_queues_and_order();

    void aw_channel_thread();
    void w_channel_thread();
    void ar_channel_thread();
    void b_channel_thread();
    void r_channel_thread();
    uint8_t map_qos_to_rp(uint8_t qos) const;

    // 同 ID 跨 RP 顺序约束检查（读写各一张表，理由见 rp_order_guard.h）
    RpOrderGuard rd_order_guard_{"读"};
    RpOrderGuard wr_order_guard_{"写"};
};
