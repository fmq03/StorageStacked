/**
 * 发送调度、credit消耗、跨帧续传与输出保持的实现。
 */
#include "flit_packer.h"
#include <iomanip>
#include <iostream>

FlitPacker::FlitPacker(sc_module_name name,
                       unsigned rp_count,
                       unsigned rdata_capacity_per_rp,
                       unsigned wresp_capacity_per_rp,
                       int flush_timeout_cycles)
    : sc_module(name),
      rreq_in("rreq_in", rp_count),
      wreq_in("wreq_in", rp_count),
      wdata_in("wdata_in", rp_count),
      rp_count_(rp_count),
      flush_timeout_cycles_(flush_timeout_cycles),
      credits_(rp_count, rdata_capacity_per_rp, wresp_capacity_per_rp) {
    SC_THREAD(packing_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
}

// ---------------------------------------------------------------------------
// 主循环。每个时钟沿依次做四件事：
//   1. 收 credit 事件（对端授予的 TX credit / 本端待归还的 RX credit）
//   2. 处理输出寄存器的 ready/valid 握手
//   3. 往在建 Flit 里填消息（即使输出寄存器还被占着也照填，形成两级流水）
//   4. 决定是否把在建 Flit 推到输出寄存器
// ---------------------------------------------------------------------------
void FlitPacker::packing_thread() {
    cur_flit_.clear();
    credits_.reset();
    // 复位后立刻把本端 R/B 接收容量转成"待归还 credit"，由下面的常规
    // CrdtGrant 路径分批公布；单个三位credit字段最多表示 128 粒度，
    // 写响应的两位credit字段最多表示 8 粒度。
    credits_.publish_initial_capacity();
    timeout_cnt_ = 0;
    // 让复位后的第一个空闲拍就发出 CrdtGrant，尽快让对端可以开始发数据。
    credit_return_idle_cnt_ = CREDIT_RETURN_TIMEOUT_CYCLES - 1;
    next_rp_ = 0;
    output_active_ = false;
    output_transfer_ = FlitTransfer{};
    spill_active_ = false;
    spill_done_ = 0;
    flits_sent_ = 0;
    granules_sent_ = 0;
    for (unsigned rp = 0; rp < MAX_RESOURCE_PLANES; ++rp) {
        staged_rreq_[rp].reset();
        staged_wreq_[rp].reset();
        staged_wdata_[rp].reset();
    }
    flit_out.write(FlitTransfer{});
    wait();

    while (true) {
        drain_credit_events();

        // ---- 2) 输出握手 ----
        // 上一拍已有有效 Flit：ready=0 时绝不能清空或改写输出；
        // ready=1 表示本时钟沿完成握手，随后才允许撤销 valid。
        if (output_active_) {
            if (flit_ready.read()) {
                output_active_ = false;
                output_transfer_ = FlitTransfer{};
                flit_out.write(FlitTransfer{});
            } else {
                flit_out.write(output_transfer_);
            }
        }

        bool exhausted = pack_cycle();

        // ---- 4) 发包判决 ----
        if (!output_active_) {
            if (cur_flit_.valid) {
                bool full = cur_flit_.remaining_granules() == 0;
                if (full) {
                    // 装满即发，这是链路成为瓶颈时的常态路径
                    flush_flit();
                } else if (exhausted) {
                    // 本拍确实没有更多消息可打。flush_timeout_cycles_ = 0 时
                    // 立即发出（时延最优）；设为正数则再等几拍碰运气攒包。
                    if (timeout_cnt_ >= flush_timeout_cycles_) flush_flit();
                    else ++timeout_cnt_;
                } else {
                    // 因条数上限而停下，说明还有后续消息，下一拍继续填
                    timeout_cnt_ = 0;
                }
            } else if (credits_.has_pending_returns()) {
                // 没有业务 Flit 可供捎带时，必须主动发 CrdtGrant，否则对端可能在
                // credit 用尽后永久等待，形成协议级死锁。
                if (++credit_return_idle_cnt_ >= CREDIT_RETURN_TIMEOUT_CYCLES) {
                    send_dedicated_credit_grant();
                    credit_return_idle_cnt_ = 0;
                }
            }
        }

        wait();
    }
}

void FlitPacker::drain_credit_events() {
    CreditUpdate update;
    while (credit_update_in.nb_read(update)) {
        credits_.add_tx_credit(update);
        if (g_aou_verbose) {
            std::cout << "[CreditManager] @" << sc_time_stamp()
                      << " RX grant rp=" << (int)update.rp
                      << " type=" << credit_kind_to_str(update.kind)
                      << " +" << update.granules << std::endl;
        }
    }
    while (credit_return_in.nb_read(update)) {
        credits_.return_rx_credit(update);
        if (g_aou_verbose) {
            std::cout << "[CreditManager] @" << sc_time_stamp()
                      << " Return pending rp=" << (int)update.rp
                      << " type=" << credit_kind_to_str(update.kind)
                      << " +" << update.granules << std::endl;
        }
    }
}

void FlitPacker::fill_staging_slots() {
    for (unsigned rp = 0; rp < rp_count_; ++rp) {
        AouMessage msg;
        if (!staged_rreq_[rp]  && rreq_in[rp].nb_read(msg))  staged_rreq_[rp]  = msg;
        if (!staged_wreq_[rp]  && wreq_in[rp].nb_read(msg))  staged_wreq_[rp]  = msg;
        if (!staged_wdata_[rp] && wdata_in[rp].nb_read(msg)) staged_wdata_[rp] = msg;
    }
}

/**
 * @brief 本拍的打包动作
 * @return true 表示"本拍已经没有任何可打的消息"（可以考虑立即发包）
 *         false 表示因为 Flit 装满 / 达到单拍条数上限而停下（不应算作空闲）
 */
bool FlitPacker::pack_cycle() {
    // ---- 3a) 跨 Flit 续传 ----
    // 上一个 Flit 尾部截断的消息，必须接在新 Flit 的 G0，且不置 MsgStart 位。
    if (spill_active_) {
        if (cur_flit_.used_granules != 0) return false;   // 上一包还没发走，先等
        int took = cur_flit_.pack_fragment(spill_msg_, spill_done_);
        spill_done_ += took;
        if (spill_done_ >= spill_msg_.granules) {
            if (g_aou_verbose) {
                std::cout << "[FlitPacker] @" << sc_time_stamp()
                          << " Resumed " << msgtype_to_str(spill_msg_.type)
                          << " tail " << took << " granule(s) at G0" << std::endl;
            }
            spill_active_ = false;
            spill_done_   = 0;
        } else {
            return false;   // 续传占满整帧时等待输出，再处理后续片段。
        }
    }

    // ---- 3b) 常规打包 ----
    unsigned started = 0;
    while (started < PACK_MSGS_PER_CYCLE && cur_flit_.remaining_granules() > 0) {
        fill_staging_slots();
        Candidate candidate = select_candidate();
        if (!candidate.message) return true;      // 确实没东西可打了
        consume_candidate(candidate);
        ++started;
        if (spill_active_) return false;          // 本 Flit 已被填满
    }
    return false;
}

FlitPacker::Candidate FlitPacker::select_from(
    std::array<std::optional<AouMessage>, MAX_RESOURCE_PLANES>& slots) {
    for (unsigned checked = 0; checked < rp_count_; ++checked) {
        unsigned rp = (next_rp_ + checked) % rp_count_;
        if (!slots[rp]) continue;
        AouMessage& msg = *slots[rp];
        if (credits_.can_consume(static_cast<uint8_t>(rp), msg.type, msg.granules))
            return Candidate{&msg, rp};
    }
    return {};
}

FlitPacker::Candidate FlitPacker::select_candidate() {
    // 优先级为读请求、写请求、写数据；每一类内部按资源平面轮转。
    Candidate candidate = select_from(staged_rreq_);
    if (!candidate.message) candidate = select_from(staged_wreq_);
    if (!candidate.message) candidate = select_from(staged_wdata_);
    return candidate;
}

void FlitPacker::consume_candidate(const Candidate& candidate) {
    AouMessage msg = *candidate.message;

    // credit 按整条消息一次性扣除，即使消息会被拆到两个 Flit 里发送。
    // credit描述对端接收缓冲能容纳的粒度数，
    // 与它被切成几个 Flit 传输无关。
    credits_.consume(static_cast<uint8_t>(candidate.rp), msg.type, msg.granules);

    int took = cur_flit_.pack_fragment(msg, 0);
    sc_assert(took > 0);
    if (took < msg.granules) {
        // 消息在 Flit 尾部被截断，记录续传状态
        spill_msg_    = msg;
        spill_done_   = took;
        spill_active_ = true;
    }

    if (msg.type == MsgType::ReadReq)       staged_rreq_[candidate.rp].reset();
    else if (msg.type == MsgType::WriteReq) staged_wreq_[candidate.rp].reset();
    else                                    staged_wdata_[candidate.rp].reset();

    next_rp_ = (candidate.rp + 1) % rp_count_;
    if (g_aou_verbose) {
        std::cout << "[FlitPacker] @" << sc_time_stamp()
                  << " Packed " << msgtype_to_str(msg.type)
                  << " rp=" << candidate.rp
                  << " id=" << msg.axi_id
                  << " (" << took << "/" << msg.granules << " granule"
                  << (took < msg.granules ? ", spans to next flit" : "")
                  << ", used=" << cur_flit_.used_granules << "/48)"
                  << std::endl;
    }
}

void FlitPacker::send_dedicated_credit_grant() {
    // take_misc_grants 会把 pending 中"本次能编码出来的部分"取走，剩余量留到
    // 后续 CrdtGrant / MsgCredit 继续发放。复位后的初始容量也走这条路径，
    // 因此大容量会分几拍逐步公布完，而不是被截断到 128。
    CreditMatrix grants = credits_.take_misc_grants();
    AouMessage grant = build_crdt_grant_message(grants, rp_count_);
    bool packed = cur_flit_.pack_message(grant);
    sc_assert(packed);
    flush_flit(false);  // 同一 Flit 不再重复通过 MsgCredit 发放相同 credit。
    if (g_aou_verbose) {
        std::cout << "[CreditManager] @" << sc_time_stamp()
                  << " Sent dedicated CrdtGrant" << std::endl;
    }
}

void FlitPacker::flush_flit(bool allow_header_credit) {
    sc_assert(cur_flit_.valid && !output_active_);
    if (allow_header_credit) {
        cur_flit_.msg_credit = credits_.take_header_grant();
        if (cur_flit_.msg_credit != 0) credit_return_idle_cnt_ = 0;
    }
    if (g_aou_verbose) print_flit_summary(cur_flit_);

    ++flits_sent_;
    granules_sent_ += static_cast<unsigned long>(cur_flit_.used_granules);

    output_transfer_ = FlitTransfer(cur_flit_);
    output_active_ = true;
    flit_out.write(output_transfer_);
    cur_flit_.clear();
    timeout_cnt_ = 0;
}

void FlitPacker::print_flit_summary(const AouFlit& flit) const {
    std::cout << "========================================" << std::endl;
    std::cout << "[FlitPacker] @" << sc_time_stamp() << " >>> FLUSH FLIT <<<" << std::endl;
    std::cout << "  used_granules = " << flit.used_granules << " / 48" << std::endl;
    std::cout << "  utilization   = " << std::fixed << std::setprecision(1)
              << (100.0 * flit.used_granules / GRANULE_COUNT) << "%" << std::endl;
    std::cout << "  msg_start     = 0x" << std::hex << flit.msg_start << std::dec << std::endl;
    std::cout << "  msg_credit    = 0x" << std::hex << flit.msg_credit << std::dec << std::endl;
    std::cout << "========================================" << std::endl;
}
