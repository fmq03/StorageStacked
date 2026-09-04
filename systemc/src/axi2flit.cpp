/**
 * @file axi2flit.cpp
 * @brief AXI2FLIT 顶层模块实现
 *
 * 每个通道线程使用 wait-first 风格（每次循环体的第一行是 wait()），
 * 确保读取 AXI valid 信号时，发送方已完成信号更新（避免 delta-cycle 竞态）。
 *
 * 握手协议：
 *   1. wait() 返回（时钟上升沿）
 *   2. 读 *_valid；若为 false 则 continue（不拉 ready，再 wait）
 *   3. 将 AXI 字段转换为 AoU 消息并写入 sc_fifo（若 fifo 满则反压）
 *   4. 写 *_ready=true（同拍完成握手，主机在下一个 wait() 看到 ready）
 *   5. wait()；写 *_ready=false；继续循环
 */

#include "axi2flit.h"
#include <iostream>

// ============================================================
//  AW 通道处理线程：接收写地址 → 生成 WriteReq 消息 → 写入 FIFO
// ============================================================
void Axi2Flit::aw_channel_thread() {
    aw_ready.write(false);
    wait();  // 等待复位完成

    while (true) {
        wait();  // wait-first：先等时钟沿，再读信号

        if (!aw_valid.read()) {
            aw_ready.write(false);
            continue;
        }

        // 读取 AW 字段，构造 WriteReq 消息
        AxChannel aw = aw_ch.read();
        AouMessage msg = MsgBuilder::build_write_req(aw);

        // 尝试写入 FIFO；若满则下一拍重试（反压 AXI 主机）
        if (!sig_wreq_fifo.nb_write(msg)) {
            aw_ready.write(false);
            continue;
        }

        // 握手成功：拉高 aw_ready，下一拍再撤销
        std::cout << "[AW Thread] @" << sc_time_stamp()
                  << " Got AW: id=" << (int)aw.id
                  << " addr=0x" << std::hex << aw.addr << std::dec
                  << " len=" << (int)aw.len << std::endl;
        aw_ready.write(true);
        wait();
        aw_ready.write(false);
    }
}

// ============================================================
//  W 通道处理线程：接收写数据 → 生成 WriteData/WriteDataFull → 写入 FIFO
// ============================================================
void Axi2Flit::w_channel_thread() {
    w_ready.write(false);
    wait();

    while (true) {
        wait();

        if (!w_valid.read()) {
            w_ready.write(false);
            continue;
        }

        WChannel w = w_ch.read();
        AouMessage msg;
        if (MsgBuilder::is_full_strobe(w)) {
            msg = MsgBuilder::build_write_data_full(w);
        } else {
            msg = MsgBuilder::build_write_data(w);
        }

        if (!sig_wdata_fifo.nb_write(msg)) {
            w_ready.write(false);
            continue;
        }

        std::cout << "[W  Thread] @" << sc_time_stamp()
                  << " Got W: type=" << msgtype_to_str(msg.type)
                  << " last=" << w.last << std::endl;
        w_ready.write(true);
        wait();
        w_ready.write(false);
    }
}

// ============================================================
//  AR 通道处理线程：接收读地址 → 生成 ReadReq 消息 → 写入 FIFO
// ============================================================
void Axi2Flit::ar_channel_thread() {
    ar_ready.write(false);
    wait();

    while (true) {
        wait();

        if (!ar_valid.read()) {
            ar_ready.write(false);
            continue;
        }

        AxChannel ar = ar_ch.read();
        AouMessage msg = MsgBuilder::build_read_req(ar);

        if (!sig_rreq_fifo.nb_write(msg)) {
            ar_ready.write(false);
            continue;
        }

        std::cout << "[AR Thread] @" << sc_time_stamp()
                  << " Got AR: id=" << (int)ar.id
                  << " addr=0x" << std::hex << ar.addr << std::dec
                  << " len=" << (int)ar.len << std::endl;
        ar_ready.write(true);
        wait();
        ar_ready.write(false);
    }
}
