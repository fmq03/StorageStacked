/**
 * 接收暂存、消息解析、credit事件发送与响应队列分流的实现。
 */
#include "flit_unpacker.h"
#include <algorithm>
#include <iostream>

FlitUnpacker::FlitUnpacker(sc_module_name name, unsigned rp_count)
    : sc_module(name),
      rdata_out("rdata_out", rp_count),
      wresp_out("wresp_out", rp_count),
      rp_count_(rp_count) {
    sc_assert(rp_count_ >= 1 && rp_count_ <= MAX_RESOURCE_PLANES);
    SC_THREAD(unpacking_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
}

void FlitUnpacker::unpacking_thread() {
    holding_valid_ = false;
    holding_flit_ = FlitTransfer{};
    pending_messages_.clear();
    pending_index_ = 0;
    pending_credits_.clear();
    pending_credit_index_ = 0;
    stream_decoder_.reset();
    flits_received_ = 0;
    granules_received_ = 0;
    flit_in_ready.write(true);
    wait();

    while (true) {
        // 1) 先把上一拍解析出来、还没写进下游 FIFO 的消息尽量分发掉
        route_messages();

        // 2) holding register 空了才接收新 Flit。
        //    flit_in_ready.read() 读到的是上一拍写入的值，与"寄存输出的 ready"
        //    语义一致：valid && ready 同拍为高即完成握手。
        if (!holding_valid_ && flit_in_ready.read()) {
            FlitTransfer incoming = flit_in.read();
            if (incoming.valid) {
                holding_flit_  = incoming;
                holding_valid_ = true;
                ++flits_received_;
                parse_holding_flit();
                route_messages();   // 同拍尽量分发，避免每包多花一拍
            }
        }

        // 3) 本 Flit 的消息全部落进下游 FIFO 后释放 holding register
        if (holding_valid_ && pending_index_ >= pending_messages_.size() &&
            pending_credit_index_ >= pending_credits_.size()) {
            holding_valid_ = false;
            holding_flit_  = FlitTransfer{};
            pending_messages_.clear();
            pending_index_ = 0;
            pending_credits_.clear();
            pending_credit_index_ = 0;
        }

        flit_in_ready.write(!holding_valid_);
        wait();
    }
}

/**
 * @brief 解析 holding register 中的 Flit
 *
 * 消息边界的判定规则：
 *   - MsgStart[i] = 1  → granule i 是一条新消息的起点
 *   - 消息总长度由首字节算出（message_granules_from_header）
 *   - 若剩余粒度不足以放下整条消息，说明它被截断到下一个 Flit 的 G0
 */
void FlitUnpacker::parse_holding_flit() {
    pending_messages_.clear();
    pending_index_ = 0;
    const AouFlit& flit = holding_flit_.flit;

    // 先完整检查消息结构再提交 credit/响应，防止坏包产生部分副作用。
    AouStreamDecoder::Result decoded;
    try {
        if (flit.fdid != 0) throw std::invalid_argument("本桥只接收 FDId=0");
        decoded = stream_decoder_.consume(flit);
    } catch (const std::exception& e) {
        SC_REPORT_FATAL("FlitUnpacker/Protocol", e.what());
        return;
    }
    granules_received_ += decoded.used_granules;

    // Protocol Header 中捎带的是"对端接收缓冲已释放"的 credit，直接送往
    // 本端 Tx CreditManager；字段本身不占 Protocol Payload。
    decode_header_credits(flit.msg_credit, rp_count_, [this](const CreditUpdate& update) {
        pending_credits_.push_back(update);
    });

    // 解析器先拼接 G0 的续传，再按 MsgStart 扫描新消息；空粒度和 credit-only
    // Flit 不产生消息。消息长度来自首字节，而非相邻 MsgStart 间距。
    // 具体边界检查和跨 Flit 状态现集中在 aou_stream_decoder.h，供接入端复用。
    for (const auto& msg : decoded.messages) dispatch_message(msg);
}

// 按消息类型分类：Misc 直接转成 credit 事件，R/B 进入待分发队列，其余丢弃并告警
void FlitUnpacker::dispatch_message(const AouMessage& msg) {
    if (msg.type == MsgType::Misc) {
        CreditMatrix grants{};
        if (decode_crdt_grant_message(msg, rp_count_, grants))
            emit_credit_matrix(grants);
        return;
    }
    if (msg.rp >= rp_count_) {
        SC_REPORT_WARNING("FlitUnpacker", "收到未启用 RP 的消息，已丢弃");
        return;
    }
    if (msg.type == MsgType::ReadData || msg.type == MsgType::WriteResp) {
        pending_messages_.push_back(msg);
    } else {
        // 当前模块是 initiator 侧桥，只实现从链路接收 R/B；按照 桥接消息 非对称
        // 接口规则，本端对 WREQ/RREQ/WDATA 公布的接收 credit 均为 0。
        SC_REPORT_WARNING("FlitUnpacker", "收到本端未实现的消息类型，已丢弃");
    }
}

// 把已解析消息写入对应 RP 的接收 FIFO，一拍最多 UNPACK_MSGS_PER_CYCLE 条；
// 下游 FIFO 满时提前退出，剩余消息留到下一拍继续。
void FlitUnpacker::route_messages() {
    // 使用非阻塞事件发送。FIFO 满就保留余量，主循环会保持 holding/撤销 ready，
    // 下拍重试；不会在时钟线程内部阻塞并漏掉已经对外承诺的 Flit 握手。
    while (pending_credit_index_ < pending_credits_.size()) {
        if (!credit_update_out.nb_write(pending_credits_[pending_credit_index_])) return;
        ++pending_credit_index_;
    }
    unsigned routed = 0;
    while (pending_index_ < pending_messages_.size() && routed < UNPACK_MSGS_PER_CYCLE) {
        const AouMessage& msg = pending_messages_[pending_index_];
        bool written = false;
        if (msg.type == MsgType::ReadData)
            written = rdata_out[msg.rp].nb_write(msg);
        else if (msg.type == MsgType::WriteResp)
            written = wresp_out[msg.rp].nb_write(msg);
        else
            written = true;   // 理论上不会出现，避免死循环

        if (!written) break;
        ++pending_index_;
        ++routed;
    }
}

void FlitUnpacker::emit_credit_matrix(const CreditMatrix& grants) {
    for (unsigned rp = 0; rp < rp_count_; ++rp) {
        for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k) {
            unsigned amount = grants[rp][k];
            if (amount != 0) {
                pending_credits_.push_back(CreditUpdate{
                    static_cast<uint8_t>(rp), static_cast<CreditKind>(k), amount});
            }
        }
    }
}
