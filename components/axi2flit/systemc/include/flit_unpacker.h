/**
 * 帧接收、消息重组与响应分流。接收暂存同时保护待处理消息和credit事件。
 */
#pragma once

#include "aou_types.h"
#include "credit_manager.h"
#include "aou_stream_decoder.h"
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
    // RP=4 时一个合法 Flit 的多条 CrdtGrant 可产生超过64条 credit 事件。
    // 不可阻塞 write 后继续保持旧 ready；事件与消息共同占用 holding register。
    std::vector<CreditUpdate> pending_credits_;
    std::size_t pending_credit_index_ = 0;

    // ---- 跨 Flit 续传状态 ----
    AouStreamDecoder stream_decoder_; // 仅保存线上消息续传状态，不依赖辅助粒度字段

    unsigned long flits_received_    = 0;
    unsigned long granules_received_ = 0;

    void unpacking_thread();
    void parse_holding_flit();
    void dispatch_message(const AouMessage& msg);
    void route_messages();
    void emit_credit_matrix(const CreditMatrix& grants);
};
