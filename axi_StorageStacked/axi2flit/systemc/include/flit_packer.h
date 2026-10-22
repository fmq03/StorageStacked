/**
 * 带发送credit控制的帧打包器。支持每拍多消息、跨帧续传和输出背压保持。
 * 输出寄存器与在建帧分别保存状态，已公布的数据保持至握手完成。
 */
#pragma once

#include "aou_types.h"
#include "credit_manager.h"
#include <array>
#include <optional>

// 攒包超时（拍）。0 = 本拍没有可打的消息就立刻发出，时延最优。
// 设为正数则会等待若干拍以换取更高的 Flit 占用率，可用于做时延/带宽折中实验。
static constexpr int DEFAULT_FLUSH_TIMEOUT_CYCLES = 0;

// 没有业务 Flit 可捎带时，主动发送专用 CrdtGrant 前的等待拍数
static constexpr int CREDIT_RETURN_TIMEOUT_CYCLES = 2;

// 一拍最多起多少条新消息。这是对打包器组合逻辑宽度的建模假设：
// 8 条 × 最短消息 3 granule = 24 granule/拍，8 条 × WriteDataFull 7 granule
// = 56 granule/拍 > 48，即单拍可填满一个 Flit，打包器不会成为瓶颈。
static constexpr unsigned PACK_MSGS_PER_CYCLE = 8;

SC_MODULE(FlitPacker) {
public:
    sc_in<bool> clk;
    sc_in<bool> rst_n;

    // 每个 vector 元素对应一个 RP，防止某个 RP credit 耗尽后堵住其他 RP 队头。
    sc_vector<sc_fifo_in<AouMessage>> rreq_in;
    sc_vector<sc_fifo_in<AouMessage>> wreq_in;
    sc_vector<sc_fifo_in<AouMessage>> wdata_in;

    // 对端授予的发送 credit，以及本端 R/B 消费后需要归还给对端的 credit。
    sc_fifo_in<CreditUpdate> credit_update_in;
    sc_fifo_in<CreditReturn> credit_return_in;

    sc_out<FlitTransfer> flit_out;
    sc_in<bool>          flit_ready;

    SC_HAS_PROCESS(FlitPacker);
    FlitPacker(sc_module_name name,
               unsigned rp_count = DEFAULT_RESOURCE_PLANES,
               unsigned rdata_capacity_per_rp = RX_RDATA_CREDITS_PER_RP,
               unsigned wresp_capacity_per_rp = RX_WRESP_CREDITS_PER_RP,
               int flush_timeout_cycles = DEFAULT_FLUSH_TIMEOUT_CYCLES);

    // 统计：累计发出的 Flit 数与已使用粒度数（testbench 计算链路占用率用）
    unsigned long flits_sent()      const { return flits_sent_; }
    unsigned long granules_sent()   const { return granules_sent_; }

private:
    struct Candidate {
        AouMessage* message = nullptr;
        unsigned rp = 0;
    };

    unsigned rp_count_;
    int flush_timeout_cycles_;
    CreditManager credits_;

    AouFlit cur_flit_;                 // 在建 Flit（第 2 级缓冲）
    FlitTransfer output_transfer_;     // 输出寄存器（第 1 级缓冲）
    bool output_active_ = false;

    // ---- 跨 Flit 续传状态 ----
    // spill_active_ 为真时，spill_msg_ 的前 spill_done_ 个粒度已经写入前一个
    // Flit 的尾部，剩余部分必须写在下一个 Flit 的 G0（且不置 MsgStart 位）。
    bool       spill_active_ = false;
    AouMessage spill_msg_{};
    int        spill_done_   = 0;

    int timeout_cnt_ = 0;
    int credit_return_idle_cnt_ = 0;
    unsigned next_rp_ = 0;

    unsigned long flits_sent_    = 0;
    unsigned long granules_sent_ = 0;

    // sc_fifo 没有 peek 接口。每个 RP/类型设置一个 staging slot，先取出队头，
    // 再根据消息实际 granule 数检查 credit；credit 不足时消息留在 slot 中等待。
    std::array<std::optional<AouMessage>, MAX_RESOURCE_PLANES> staged_rreq_{};
    std::array<std::optional<AouMessage>, MAX_RESOURCE_PLANES> staged_wreq_{};
    std::array<std::optional<AouMessage>, MAX_RESOURCE_PLANES> staged_wdata_{};

    void packing_thread();
    void drain_credit_events();
    void fill_staging_slots();
    bool pack_cycle();                 // 返回 true 表示"本拍已无更多可打的消息"
    Candidate select_candidate();
    Candidate select_from(
        std::array<std::optional<AouMessage>, MAX_RESOURCE_PLANES>& slots);
    void consume_candidate(const Candidate& candidate);
    void send_dedicated_credit_grant();
    void flush_flit(bool allow_header_credit = true);
    void print_flit_summary(const AouFlit& flit) const;
};
