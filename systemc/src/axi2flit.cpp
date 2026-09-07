/**
 * @file axi2flit.cpp
 * @brief Axi2Flit 顶层构造、端口连接以及 AXI 五通道处理线程
 */

#include "axi2flit.h"
#include <iostream>

Axi2Flit::Axi2Flit(sc_module_name name, unsigned rp_count)
    : sc_module(name),
      rp_count_(rp_count),
      packer("flit_packer", rp_count,
             RX_RDATA_CREDITS_PER_RP, RX_WRESP_CREDITS_PER_RP),
      unpacker("flit_unpacker", rp_count),
      sig_rreq_fifo("rreq_fifo"),
      sig_wreq_fifo("wreq_fifo"),
      sig_wdata_fifo("wdata_fifo"),
      sig_rdata_fifo("rdata_fifo"),
      sig_wresp_fifo("wresp_fifo"),
      sig_credit_update_fifo("credit_update_fifo", 64),
      sig_credit_return_fifo("credit_return_fifo", 64),
      sig_write_route_fifo("write_route_fifo", TX_REQ_FIFO_DEPTH_PER_RP * rp_count) {
    sc_assert(rp_count_ >= 1 && rp_count_ <= MAX_RESOURCE_PLANES);

    // sc_vector 中的 sc_fifo 需要自定义 creator，才能为每个 RP 指定队列深度。
    sig_rreq_fifo.init(rp_count_, [](const char* nm, std::size_t) {
        return new sc_fifo<AouMessage>(nm, TX_REQ_FIFO_DEPTH_PER_RP);
    });
    sig_wreq_fifo.init(rp_count_, [](const char* nm, std::size_t) {
        return new sc_fifo<AouMessage>(nm, TX_REQ_FIFO_DEPTH_PER_RP);
    });
    sig_wdata_fifo.init(rp_count_, [](const char* nm, std::size_t) {
        return new sc_fifo<AouMessage>(nm, TX_WDATA_FIFO_DEPTH_PER_RP);
    });
    sig_rdata_fifo.init(rp_count_, [](const char* nm, std::size_t) {
        return new sc_fifo<AouMessage>(nm, RX_RDATA_FIFO_DEPTH_PER_RP);
    });
    sig_wresp_fifo.init(rp_count_, [](const char* nm, std::size_t) {
        return new sc_fifo<AouMessage>(nm, RX_WRESP_FIFO_DEPTH_PER_RP);
    });

    packer.clk(clk);
    packer.rst_n(rst_n);
    packer.flit_out(flit_out);
    packer.flit_ready(flit_ready);
    packer.credit_update_in(sig_credit_update_fifo);
    packer.credit_return_in(sig_credit_return_fifo);

    unpacker.clk(clk);
    unpacker.rst_n(rst_n);
    unpacker.flit_in(flit_in);
    unpacker.flit_in_ready(flit_in_ready);
    unpacker.credit_update_out(sig_credit_update_fifo);

    for (unsigned rp = 0; rp < rp_count_; ++rp) {
        packer.rreq_in[rp](sig_rreq_fifo[rp]);
        packer.wreq_in[rp](sig_wreq_fifo[rp]);
        packer.wdata_in[rp](sig_wdata_fifo[rp]);
        unpacker.rdata_out[rp](sig_rdata_fifo[rp]);
        unpacker.wresp_out[rp](sig_wresp_fifo[rp]);
    }

    SC_THREAD(aw_channel_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
    SC_THREAD(w_channel_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
    SC_THREAD(ar_channel_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
    SC_THREAD(b_channel_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
    SC_THREAD(r_channel_thread);
    sensitive << clk.pos();
    async_reset_signal_is(rst_n, false);
}
uint8_t Axi2Flit::map_qos_to_rp(uint8_t qos) const {
    return static_cast<uint8_t>(qos % rp_count_);
}

/**
 * 【P0-3：AXI 通道去气泡】
 *
 * 旧实现的写法是：
 *      ready.write(true);  wait();  ready.write(false);
 * 也就是每完成一次握手就强制把 ready 拉低一拍。结果是 AXI 侧最快只能
 * 2 拍传 1 拍数据，吞吐直接腰斩（@500MHz、256b 数据 → 8GB/s）。
 *
 * 新实现改成标准的"寄存器化 ready"：
 *      本拍看到 (valid && ready_reg) 就接收数据；
 *      然后只根据下游 FIFO 是否还有空间来决定下一拍的 ready。
 * ready 与是否刚刚发生握手无关，因此源端 valid 常拉高时可以做到
 * 1 beat/cycle 连续传输。
 *
 * ready_reg 在拉高之前必须保证"下一拍真的能收下"，所以判据是
 * num_free() > 0（本拍最多只会有一个写入者，因此 1 个空位即足够）。
 * AW/AR 的目标 RP 由 QoS 决定，在数据到来前无法预知，因此采取保守判据：
 * 所有已启用 RP 的对应 FIFO 都有空位才拉高 ready。
 */
void Axi2Flit::aw_channel_thread() {
    aw_ready.write(false);
    bool ready_reg = false;
    wait();
    while (true) {
        // ---- 1) 采样握手：上一拍拉高的 ready 与本拍的 valid 同时有效 ----
        if (ready_reg && aw_valid.read()) {
            AxChannel aw = aw_ch.read();
            uint8_t rp = map_qos_to_rp(aw.qos);
            // 约束 C-1 检查：同一 AWID 的未完成事务必须落在同一个 RP，
            // 否则 B 响应可能跨 RP 乱序返回（详见 rp_order_guard.h）。
            wr_order_guard_.bind(aw.id, rp);
            // AW 消息和 W 路由信息必须原子入队，两个队列的空间已在
            // 上一拍计算 ready 时统一检查过，这里断言必定成功。
            AouMessage msg = MsgBuilder::build_write_req(aw, rp);
            bool msg_ok   = sig_wreq_fifo[rp].nb_write(msg);
            bool route_ok = sig_write_route_fifo.nb_write(
                WriteRoute{rp, static_cast<unsigned>(aw.len) + 1});
            sc_assert(msg_ok && route_ok);

            if (g_aou_verbose) {
                std::cout << "[AW Thread] @" << sc_time_stamp()
                          << " Got AW id=" << (int)aw.id << " rp=" << (int)rp
                          << " len=" << (int)aw.len
                          << " addr=0x" << std::hex << aw.addr << std::dec << std::endl;
            }
        }

        // ---- 2) 计算下一拍的 ready ----
        bool space = sig_write_route_fifo.num_free() > 0;
        for (unsigned rp = 0; rp < rp_count_ && space; ++rp)
            space = sig_wreq_fifo[rp].num_free() > 0;
        ready_reg = space;
        aw_ready.write(ready_reg);
        wait();
    }
}

void Axi2Flit::w_channel_thread() {
    w_ready.write(false);
    bool ready_reg = false;
    bool have_route = false;
    WriteRoute route{};
    wait();
    while (true) {
        // ---- 1) 采样握手 ----
        // ready_reg 只有在 have_route 成立时才会拉高，因此这里 route 一定有效。
        if (ready_reg && w_valid.read()) {
            WChannel w = w_ch.read();
            // 全 strobe 有效时用 WriteDataFull（256b: 7 granule），
            // 比带 WSTRB 的 WriteData（8 granule）省 1 个 granule。
            AouMessage msg = MsgBuilder::is_full_strobe(w)
                ? MsgBuilder::build_write_data_full(w, route.rp)
                : MsgBuilder::build_write_data(w, route.rp);
            bool written = sig_wdata_fifo[route.rp].nb_write(msg);
            sc_assert(written);

            if (route.beats_remaining > 0) --route.beats_remaining;
            bool route_done = route.beats_remaining == 0;
            if (route_done != w.last)
                SC_REPORT_WARNING("Axi2Flit", "AWLEN 与 WLAST 不一致，按先到的结束条件收束");
            if (route_done || w.last) have_route = false;

            if (g_aou_verbose) {
                std::cout << "[W  Thread] @" << sc_time_stamp()
                          << " Got W rp=" << (int)msg.rp
                          << " type=" << msgtype_to_str(msg.type)
                          << " last=" << w.last << std::endl;
            }
        }

        // ---- 2) 取下一条 burst 的路由 ----
        // AXI4 的 W 通道没有 WID，必须靠 AW 的到达顺序确定这一串 W 属于哪个 RP。
        if (!have_route) have_route = sig_write_route_fifo.nb_read(route);

        // ---- 3) 计算下一拍的 ready ----
        ready_reg = have_route && sig_wdata_fifo[route.rp].num_free() > 0;
        w_ready.write(ready_reg);
        wait();
    }
}

void Axi2Flit::ar_channel_thread() {
    ar_ready.write(false);
    bool ready_reg = false;
    wait();
    while (true) {
        // ---- 1) 采样握手 ----
        if (ready_reg && ar_valid.read()) {
            AxChannel ar = ar_ch.read();
            uint8_t rp = map_qos_to_rp(ar.qos);
            // 约束 C-1 检查，理由同 aw_channel_thread
            rd_order_guard_.bind(ar.id, rp);
            AouMessage msg = MsgBuilder::build_read_req(ar, rp);
            bool written = sig_rreq_fifo[rp].nb_write(msg);
            sc_assert(written);

            if (g_aou_verbose) {
                std::cout << "[AR Thread] @" << sc_time_stamp()
                          << " Got AR id=" << (int)ar.id << " rp=" << (int)rp
                          << " len=" << (int)ar.len
                          << " addr=0x" << std::hex << ar.addr << std::dec << std::endl;
            }
        }

        // ---- 2) 计算下一拍的 ready ----
        bool space = true;
        for (unsigned rp = 0; rp < rp_count_ && space; ++rp)
            space = sig_rreq_fifo[rp].num_free() > 0;
        ready_reg = space;
        ar_ready.write(ready_reg);
        wait();
    }
}

/**
 * B/R 通道同样去掉了原来的"每拍先 wait 再判断"结构：
 * 握手完成的当拍就去取下一条消息并驱动 valid，因此在响应流连续时
 * 可以做到 1 beat/cycle，不再每条之间插一个空泡。
 *
 * credit 归还发生在响应真正被 AXI 主设备取走的那一拍——这才是接收缓冲
 * 真正被释放的时刻，早归还会导致对端超发、接收 FIFO 溢出。
 */
void Axi2Flit::b_channel_thread() {
    b_valid.write(false);
    b_ch.write(BChannel{});
    bool active = false;
    AouMessage active_msg;
    uint16_t active_id = 0;      // 当前在 b_ch 上的 BID，握手后用于销账（约束 C-1）
    unsigned next_rp = 0;
    wait();
    while (true) {
        // ---- 1) 握手完成，归还 WRESP credit ----
        if (active && b_ready.read()) {
            bool ok = sig_credit_return_fifo.nb_write(CreditReturn{
                active_msg.rp, CreditKind::WriteResp,
                static_cast<unsigned>(active_msg.granules)});
            sc_assert(ok);   // FlitPacker 每拍都会抽干该队列，正常不会满
            // 一笔写事务到 B 握手才算真正结束，此时该 ID 的占用可以释放。
            wr_order_guard_.retire(active_id);
            active = false;
        }

        // ---- 2) 同拍取下一条 WriteResp（RP 间轮转，避免饿死）----
        if (!active) {
            for (unsigned checked = 0; checked < rp_count_; ++checked) {
                unsigned rp = (next_rp + checked) % rp_count_;
                if (!sig_wresp_fifo[rp].nb_read(active_msg)) continue;
                BChannel b;
                if (!MsgDecoder::decode_write_resp(active_msg, b)) {
                    SC_REPORT_ERROR("Axi2Flit", "WriteResp 解码失败");
                    break;
                }
                b_ch.write(b);
                active_id = b.id;
                active = true;
                next_rp = (rp + 1) % rp_count_;
                break;
            }
        }

        b_valid.write(active);
        wait();
    }
}

void Axi2Flit::r_channel_thread() {
    r_valid.write(false);
    r_ch.write(RChannel{});
    bool active = false;
    AouMessage active_msg;
    uint16_t active_id   = 0;    // 当前在 r_ch 上的 RID
    bool     active_last = false;// 是否是该 burst 的最后一拍
    unsigned next_rp = 0;
    wait();
    while (true) {
        // ---- 1) 握手完成，归还 RDATA credit ----
        if (active && r_ready.read()) {
            bool ok = sig_credit_return_fifo.nb_write(CreditReturn{
                active_msg.rp, CreditKind::ReadData,
                static_cast<unsigned>(active_msg.granules)});
            sc_assert(ok);
            // 读事务要等 RLAST 才算结束 —— 中间的 beat 不能销账，
            // 否则一条 16 beat 的 burst 会被当成 16 笔事务重复释放。
            if (active_last) rd_order_guard_.retire(active_id);
            active = false;
        }

        // ---- 2) 同拍取下一条 ReadData ----
        if (!active) {
            for (unsigned checked = 0; checked < rp_count_; ++checked) {
                unsigned rp = (next_rp + checked) % rp_count_;
                if (!sig_rdata_fifo[rp].nb_read(active_msg)) continue;
                RChannel r;
                if (!MsgDecoder::decode_read_data(active_msg, r)) {
                    SC_REPORT_ERROR("Axi2Flit", "ReadData 解码失败");
                    break;
                }
                r_ch.write(r);
                active_id   = r.id;
                active_last = r.last;
                active = true;
                next_rp = (rp + 1) % rp_count_;
                break;
            }
        }

        r_valid.write(active);
        wait();
    }
}
