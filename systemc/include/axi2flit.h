/**
 * @file axi2flit.h
 * @brief AXI2FLIT 顶层模块头文件
 *
 * Axi2Flit 是整个桥接的顶层模块，对外暴露 AXI4 接口（作为从端接收 AXI 主机请求），
 * 对内实例化消息构造逻辑和 FlitPacker，最终向 FDI 输出 UCIe Flit。
 *
 * 内部结构：
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                       Axi2Flit                              │
 *   │                                                             │
 *   │  AXI AW ──► [AW 通道处理线程] ──► wreq_msg 内部信号           │
 *   │  AXI W  ──► [W  通道处理线程] ──► wdata_msg 内部信号          │  ──► flit_out
 *   │  AXI AR ──► [AR 通道处理线程] ──► rreq_msg 内部信号           │
 *   │                                          └──► FlitPacker    │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * 各通道处理线程负责：
 *   - 执行 valid/ready 握手，接收 AXI beat
 *   - 调用 MsgBuilder 构造 AoU 消息
 *   - 向 FlitPacker 的输入队列发送消息（通过 sc_signal 传递）
 */

#pragma once

#include "axi_if.h"
#include "aou_types.h"
#include "msg_builder.h"
#include "flit_packer.h"

// ============================================================
//  Axi2Flit 顶层 SystemC 模块
// ============================================================
SC_MODULE(Axi2Flit) {
public:
    // ------ 时钟与复位 ------
    sc_in<bool> clk;
    sc_in<bool> rst_n;

    // ------ AXI4 从端接口（Slave Interface）------
    // 写地址通道（AW）
    sc_in<bool>      aw_valid;
    sc_out<bool>     aw_ready;
    sc_in<AxChannel> aw_ch;

    // 写数据通道（W）
    sc_in<bool>     w_valid;
    sc_out<bool>    w_ready;
    sc_in<WChannel> w_ch;

    // 读地址通道（AR）
    sc_in<bool>      ar_valid;
    sc_out<bool>     ar_ready;
    sc_in<AxChannel> ar_ch;

    // ------ FDI 输出接口 ------
    sc_out<FlitTransfer> flit_out;   // 向 UCIe FDI 输出的 flit
    sc_in<bool>          flit_ready; // FDI 背压信号

    // ============================================================
    //  构造函数
    // ============================================================
    SC_CTOR(Axi2Flit)
        : packer("flit_packer")
    {
        // 连接内部 FlitPacker 的时钟和复位
        packer.clk(clk);
        packer.rst_n(rst_n);

        // 连接 FlitPacker 的三路消息输入（通过内部 sc_fifo 通道桥接）
        // sc_fifo 天然处理线程间生产者-消费者同步
        packer.rreq_in(sig_rreq_fifo);
        packer.wreq_in(sig_wreq_fifo);
        packer.wdata_in(sig_wdata_fifo);

        // 连接 Flit 输出
        packer.flit_out(flit_out);
        packer.flit_ready(flit_ready);

        // 注册各通道处理线程
        SC_THREAD(aw_channel_thread);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);

        SC_THREAD(w_channel_thread);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);

        SC_THREAD(ar_channel_thread);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

private:
    // ------ 内部子模块 ------
    FlitPacker packer;

    // ------ 内部通道：AXI 通道处理线程 → FlitPacker ------
    // FIFO 深度设为 4，允许最多 4 条消息排队，为 FlitPacker 提供缓冲
    sc_fifo<AouMessage>  sig_rreq_fifo{"rreq_fifo", 4};    // 读请求 FIFO
    sc_fifo<AouMessage>  sig_wreq_fifo{"wreq_fifo", 4};    // 写请求 FIFO
    sc_fifo<AouMessage>  sig_wdata_fifo{"wdata_fifo", 8};  // 写数据 FIFO（更深）

    // ============================================================
    //  各通道处理线程
    // ============================================================
    void aw_channel_thread();  // 处理 AW 通道，生成 WriteReq 消息
    void w_channel_thread();   // 处理 W  通道，生成 WriteData/WriteDataFull 消息
    void ar_channel_thread();  // 处理 AR 通道，生成 ReadReq 消息
};

// AouMessage 的操作符定义已移至 aou_types.h，此处无需重复。
