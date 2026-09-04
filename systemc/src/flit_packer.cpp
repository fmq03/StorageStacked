/**
 * @file flit_packer.cpp
 * @brief Flit 打包模块实现
 *
 * 打包主线程每周期执行一次，流程：
 *   1. 先将 flit_out 置为 invalid（上周期输出的 flit 本周期作废）
 *   2. 判断当前 flit 是否放不下下一条消息，若放不下则先发出
 *   3. 按优先级从 FIFO 非阻塞读一条消息，填入当前 flit
 *   4. 更新超时计数；若超时则强制发出当前 flit
 *   5. wait()（等下一时钟上升沿）
 */

#include "flit_packer.h"
#include <iomanip>
#include <iostream>

void FlitPacker::packing_thread() {
    cur_flit.clear();
    timeout_cnt = 0;
    flit_out.write(FlitTransfer{});
    wait();

    while (true) {
        // 每周期开始先清除上一周期的输出（保证 flit_out 只在发送周期为 valid）
        flit_out.write(FlitTransfer{});

        // ---- 判断当前 flit 是否放不下下一条消息，若放不下则先发出 ----
        bool rreq_pending  = rreq_in.num_available()  > 0;
        bool wreq_pending  = wreq_in.num_available()  > 0;
        bool wdata_pending = wdata_in.num_available() > 0;

        // 取优先级最高的待处理消息所需 granule 数
        int need = 0;
        if      (rreq_pending)  need = REQ_GRANULES;
        else if (wreq_pending)  need = REQ_GRANULES;
        else if (wdata_pending) need = WDATA_GRANULES;

        if (cur_flit.valid) {
            bool full   = (cur_flit.remaining_granules() == 0);
            bool cannot = (need > 0 && cur_flit.remaining_granules() < need);
            if (full || cannot) {
                flush_flit();
            }
        }

        // ---- 按优先级取消息填入当前 flit ----
        bool got_msg = false;
        if (!got_msg && rreq_pending)  got_msg = try_pack_from(rreq_in,  MsgType::ReadReq);
        if (!got_msg && wreq_pending)  got_msg = try_pack_from(wreq_in,  MsgType::WriteReq);
        if (!got_msg && wdata_pending) got_msg = try_pack_from(wdata_in, MsgType::WriteData);

        // ---- 更新超时计数 ----
        if (got_msg) {
            timeout_cnt = 0;
        } else if (cur_flit.valid) {
            timeout_cnt++;
        }

        // ---- 超时或满载则发出 ----
        if (cur_flit.valid) {
            bool timed_out = (timeout_cnt >= FLUSH_TIMEOUT_CYCLES);
            bool full      = (cur_flit.remaining_granules() == 0);
            if (timed_out || full) {
                flush_flit();
            }
        }

        wait();
    }
}

// 从指定 FIFO 端口非阻塞读出一条消息并填入当前 flit
bool FlitPacker::try_pack_from(sc_fifo_in<AouMessage>& in, MsgType /*hint*/) {
    AouMessage msg;
    if (!in.nb_read(msg)) return false;
    cur_flit.pack_message(msg);
    std::cout << "[FlitPacker] @" << sc_time_stamp()
              << " Packed " << msgtype_to_str(msg.type)
              << " id=" << msg.axi_id
              << " addr=0x" << std::hex << msg.axi_addr << std::dec
              << " (used=" << cur_flit.used_granules << "/48 granules)" << std::endl;
    return true;
}

// 将当前 flit 写到 flit_out，然后清空内部状态
void FlitPacker::flush_flit() {
    print_flit_summary(cur_flit);
    flit_out.write(FlitTransfer(cur_flit));   // 本周期输出有效 flit
    cur_flit.clear();
    timeout_cnt = 0;
    // 不调用 wait()：flush 在同一时钟周期内完成；
    // 下一周期循环开始时会重新清除 flit_out 为 invalid。
}

// 打印 flit 摘要（调试日志）
void FlitPacker::print_flit_summary(const AouFlit& flit) const {
    std::cout << "========================================" << std::endl;
    std::cout << "[FlitPacker] @" << sc_time_stamp()
              << " >>> FLUSH FLIT <<<" << std::endl;
    std::cout << "  used_granules = " << flit.used_granules << " / 48" << std::endl;
    std::cout << "  utilization   = "
              << std::fixed << std::setprecision(1)
              << (100.0 * flit.used_granules / GRANULE_COUNT) << "%" << std::endl;
    std::cout << "  msg_start     = 0x" << std::hex << flit.msg_start << std::dec << std::endl;
    std::cout << "========================================" << std::endl;
}
