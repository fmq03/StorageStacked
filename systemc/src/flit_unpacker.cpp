/**
 * @file flit_unpacker.cpp
 * @brief FlitUnpacker 实现：Flit 接收、跨 Flit 消息重组、credit 上报与消息分流
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
    carry_active_ = false;
    carry_have_ = 0;
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
                granules_received_ +=
                    static_cast<unsigned long>(incoming.flit.used_granules);
                parse_holding_flit();
                route_messages();   // 同拍尽量分发，避免每包多花一拍
            }
        }

        // 3) 本 Flit 的消息全部落进下游 FIFO 后释放 holding register
        if (holding_valid_ && pending_index_ >= pending_messages_.size()) {
            holding_valid_ = false;
            holding_flit_  = FlitTransfer{};
            pending_messages_.clear();
            pending_index_ = 0;
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

    // Protocol Header 中捎带的是"对端接收缓冲已释放"的 credit，直接送往
    // 本端 Tx CreditManager；字段本身不占 Protocol Payload。
    decode_header_credits(flit.msg_credit, rp_count_, [this](const CreditUpdate& update) {
        credit_update_out.write(update);
    });

    if (flit.used_granules <= 0) return;

    int g = 0;

    // ---- (a) 处理续传片段：本 Flit 以上一个 Flit 未发完的消息开头 ----
    if (carry_active_) {
        if ((flit.msg_start & 1ULL) != 0) {
            // 发送侧本应把续传片段放在 G0 且不置 MsgStart[0]，出现这种情况
            // 说明链路上丢/乱了包，丢弃残片并按新消息重新对齐。
            SC_REPORT_WARNING("FlitUnpacker",
                              "期待跨 Flit 续传片段，但 MsgStart[0]=1，丢弃残片");
            carry_active_ = false;
            carry_have_   = 0;
        } else {
            int need = carry_msg_.granules - carry_have_;
            int take = std::min(need, flit.used_granules);
            std::copy_n(&flit.payload[0], take * GRANULE_BYTES,
                        carry_msg_.data + carry_have_ * GRANULE_BYTES);
            carry_have_ += take;
            g = take;
            if (carry_have_ >= carry_msg_.granules) {
                dispatch_message(carry_msg_);
                carry_active_ = false;
                carry_have_   = 0;
            }
            // 否则这条消息还要继续跨到下一个 Flit（1024b 消息可能连跨多包）
        }
    } else if ((flit.msg_start & 1ULL) == 0) {
        SC_REPORT_WARNING("FlitUnpacker",
                          "Flit 以非消息起始的 granule 开头，但本端没有待续传消息");
        return;
    }

    // ---- (b) 本 Flit 内新起的消息 ----
    while (g < flit.used_granules) {
        if (((flit.msg_start >> g) & 1ULL) == 0) {
            // 正常情况下不会走到这里（消息长度是自描述的），作为防御处理
            ++g;
            continue;
        }
        uint8_t b0 = flit.payload[g * GRANULE_BYTES];
        int total = message_granules_from_header(b0);
        if (total <= 0) {
            SC_REPORT_WARNING("FlitUnpacker", "无法识别的消息类型，放弃解析本 Flit 剩余部分");
            break;
        }
        int avail = flit.used_granules - g;
        int take  = std::min(total, avail);

        AouMessage msg;
        msg.type     = msg_type_of(b0);
        msg.rp       = msg_rp_of(b0);
        msg.granules = total;
        msg.byte_len = total * GRANULE_BYTES;
        std::copy_n(&flit.payload[g * GRANULE_BYTES], take * GRANULE_BYTES, msg.data);

        if (take < total) {
            // 消息在 Flit 尾部被截断，剩余部分应出现在下一个 Flit 的 G0
            carry_msg_    = msg;
            carry_have_   = take;
            carry_active_ = true;
            break;      // 截断只可能发生在 Flit 尾部，后面不会再有消息
        }
        dispatch_message(msg);
        g += total;
    }
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
        // 当前模块是 initiator 侧桥，只实现从链路接收 R/B；按照 AoU 非对称
        // 接口规则，本端对 WREQ/RREQ/WDATA 公布的接收 credit 均为 0。
        SC_REPORT_WARNING("FlitUnpacker", "收到本端未实现的消息类型，已丢弃");
    }
}

// 把已解析消息写入对应 RP 的接收 FIFO，一拍最多 UNPACK_MSGS_PER_CYCLE 条；
// 下游 FIFO 满时提前退出，剩余消息留到下一拍继续。
void FlitUnpacker::route_messages() {
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
                credit_update_out.write(CreditUpdate{
                    static_cast<uint8_t>(rp), static_cast<CreditKind>(k), amount});
            }
        }
    }
}
