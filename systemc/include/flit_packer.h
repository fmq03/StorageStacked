/**
 * @file flit_packer.h
 * @brief Flit 打包模块头文件
 *
 * FlitPacker 从三路 sc_fifo 输入读取 AoU 消息，按优先级填入 256B UCIe Flit，
 * 并在满足以下任一条件时将 flit 发出到 FDI 接口：
 *
 *   1. 水位触发：当前 flit 剩余空间不足以容纳下一条最高优先级消息
 *   2. 超时触发：flit 有内容但队列持续空闲 >= FLUSH_TIMEOUT_CYCLES 个时钟周期
 *   3. 满载触发：flit 的 granule 用尽（remaining_granules == 0）
 *
 * 使用 sc_fifo 作为消息输入接口，避免了 valid/ready 信号握手的竞态问题。
 * 生产者（AXI 通道线程）调用 nb_write 写入；本模块用 nb_read 轮询。
 *
 * 消息优先级（高→低）：ReadReq > WriteReq > WriteData/WriteDataFull
 */

#pragma once

#include "aou_types.h"

// 超时强制发送阈值（时钟周期数）
// 4 cycle × 2ns/cycle = 8ns，保留足够裕量到 20ns 延迟目标
static constexpr int FLUSH_TIMEOUT_CYCLES = 4;

// WriteData / WriteDataFull 单条消息占用的粒度数（256-bit = 8 granule）
static constexpr int WDATA_GRANULES = 8;

// ============================================================
//  FlitPacker：SystemC 模块
// ============================================================
SC_MODULE(FlitPacker) {
public:
    // ------ 时钟与复位 ------
    sc_in<bool>  clk;
    sc_in<bool>  rst_n;

    // ------ 消息输入：三路 FIFO 端口（由 Axi2Flit 顶层绑定内部 sc_fifo）------
    // 使用 sc_fifo_in 替代 valid/ready/msg 三元信号，天然处理生产-消费同步。
    sc_fifo_in<AouMessage>  rreq_in;    // 读请求消息 FIFO 端口
    sc_fifo_in<AouMessage>  wreq_in;    // 写请求消息 FIFO 端口
    sc_fifo_in<AouMessage>  wdata_in;   // 写数据消息 FIFO 端口

    // ------ 输出到 FDI 接口的 Flit ------
    sc_out<FlitTransfer>  flit_out;    // 输出 flit（valid 字段标识是否有效）
    sc_in<bool>           flit_ready;  // FDI 背压信号（简化模型中始终为 true）

    SC_CTOR(FlitPacker) : timeout_cnt(0) {
        SC_THREAD(packing_thread);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

private:
    AouFlit  cur_flit;      // 当前正在打包的 flit
    int      timeout_cnt;   // 无新消息时的计时（超时触发发送）

    void packing_thread();
    bool try_pack_from(sc_fifo_in<AouMessage>& in, MsgType type);
    void flush_flit();
    void print_flit_summary(const AouFlit& flit) const;
};
