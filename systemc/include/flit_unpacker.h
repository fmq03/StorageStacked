/**
 * @file flit_unpacker.h
 * @brief AoU Flit 接收、Protocol Header 解析、跨 Flit 消息重组与 R/B 消息分流
 *
 * Unpacker 自带一个 Flit holding register：只有 holding register 为空时才拉高
 * flit_in_ready。这样即使下游 RDATA/WRESP FIFO 临时满，也能保持已经握手接收的
 * Flit，逐条等待写入，而不会丢包。对端若遵守本端公布的 credit，正常情况下
 * 接收 FIFO 不应溢出；holding register 仍用于隔离时序和增强模型健壮性。
 *
 * 【跨 Flit 续传的接收侧实现（P0-2）】
 * MsgStart[47:0] 的语义是"granule i 是一条新消息的第一个粒度"。因此：
 *   - 若 MsgStart[0] = 0 且本 Flit 有有效粒度，说明它以上一个 Flit 未发完的
 *     消息续传开头，前若干个粒度要拼接到 carry_msg_ 上；
 *   - 每条新消息的总长度，由它自己的首字节（MSGTYPE + DLENGTH）算出，
 *     而不是靠"相邻两个 MsgStart 位之间的距离"——后者在消息被截断时会算错。
 */

#pragma once

#include "aou_types.h"
#include "credit_manager.h"
#include <vector>

// 一拍最多把多少条已解析消息写进下游 FIFO（与 PACK_MSGS_PER_CYCLE 对称）
static constexpr unsigned UNPACK_MSGS_PER_CYCLE = 8;

SC_MODULE(FlitUnpacker) {
public:
    sc_in<bool> clk;
    sc_in<bool> rst_n;

    sc_in<FlitTransfer> flit_in;
    sc_out<bool>        flit_in_ready;

    sc_vector<sc_fifo_out<AouMessage>> rdata_out;
    sc_vector<sc_fifo_out<AouMessage>> wresp_out;
    sc_fifo_out<CreditUpdate> credit_update_out;

    SC_HAS_PROCESS(FlitUnpacker);
    FlitUnpacker(sc_module_name name,
                 unsigned rp_count = DEFAULT_RESOURCE_PLANES);

    // 统计：累计收到的 Flit 数与其中已使用的粒度数
    unsigned long flits_received()    const { return flits_received_; }
    unsigned long granules_received() const { return granules_received_; }

private:
    unsigned rp_count_;
    bool holding_valid_ = false;
    FlitTransfer holding_flit_;
    std::vector<AouMessage> pending_messages_;
    std::size_t pending_index_ = 0;

    // ---- 跨 Flit 续传状态 ----
    bool       carry_active_ = false;   // 有一条消息尚未收全
    AouMessage carry_msg_{};            // 已经收到的部分（granules 为整条长度）
    int        carry_have_   = 0;       // 已收到的粒度数

    unsigned long flits_received_    = 0;
    unsigned long granules_received_ = 0;

    void unpacking_thread();
    void parse_holding_flit();
    void dispatch_message(const AouMessage& msg);
    void route_messages();
    void emit_credit_matrix(const CreditMatrix& grants);
};
