/**
 * @file flit_packer.h
 * @brief 带 AoU credit 流控、跨 Flit 续传和 FDI ready/valid 保持语义的 Flit 打包模块
 *
 * 每个 Resource Plane 都有独立的 RREQ/WREQ/WDATA 输入 FIFO 端口。Packer
 * 只有在相应 [RP][消息类型] credit 足够时才会开始一条消息，并在开始时一次性
 * 扣除整条消息所需的 granule credit。Misc/CrdtGrant 按规范不消耗 credit。
 *
 * 输出端采用标准 ready/valid 语义：valid 拉高后，如果 flit_ready 为低，完整
 * Flit 必须保持不变；只有在某个时钟沿 valid && ready 后才允许撤销或发送下一包。
 *
 * 【本版本的三处关键改动】
 * 1. 跨 Flit 续传（P0-2）
 *    一条消息装不进当前 Flit 尾部时，不再"整条推迟到下一包"，而是先把能装下的
 *    粒度填满当前 Flit，剩余部分接在下一个 Flit 的 G0。这是 512b/1024b 数据宽度
 *    可用的硬前提：WriteData1024 = 30 granule，若不许跨包，48 粒度的 Flit 只能装
 *    一条，占用率立刻掉到 30/48 = 62.5%。
 *
 * 2. 一拍可打多条消息（P0-4）
 *    原实现每拍只打一条，256b WriteData 只有 8 granule，填满一个 Flit 要 6 拍，
 *    打包本身就成了瓶颈。现在一拍最多起 PACK_MSGS_PER_CYCLE 条新消息。
 *
 * 3. 输出寄存器 + 在建 Flit 构成两级缓冲（P0-4/P0-5）
 *    输出被链路背压时仍继续填 cur_flit_，链路一旦取走上一包，下一包可以同拍发出，
 *    中间没有空泡。等价于参考 RTL 的 2 entry TX ring buffer。
 *    配合 flush_timeout_cycles_ = 0（无候选立即发包），把原来固定 4 拍的
 *    攒包等待（@500MHz = 8ns）从时延路径上彻底去掉。
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
