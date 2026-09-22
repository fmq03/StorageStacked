// SoC 侧：把 gem5 通过 Unix socket 送来的 AXI4 突发，驱动到 Axi2Flit 的
// 五通道接口上，并把 B/R 响应送回 socket。
//
// 端口与 Axi2Flit 互补（Axi2Flit 的 in 就是本模块的 out），因此可以直接
// 对绑，替换掉 tb_full_link 里那个内联的 AXI BFM。
//
// ## 线程模型（本文件最关键的部分）
//
// gem5 的在线事务协议是**阻塞 lock-step** 的：它在 directAxiSegment 里同步
// 等响应，期间整个 gem5 事件队列冻结。而响应必须靠 SystemC 内核推进时间才能
// 产生（经过 Axi2Flit → UCIe → Bridge → mem_sim 再回来）。
//
// 所以在 SC_THREAD 里阻塞 recv() 会直接死锁：内核停摆 → 响应永不产生 →
// gem5 永不返回 → socket 永不收到下一笔。
//
// 因此 socket I/O 跑在一个**独立的原生 std::thread** 上，与 SC_THREAD 之间
// 只用互斥量 + 条件变量的有界队列交换数据。两条硬规则：
//   * socket 线程绝不触碰任何 SystemC 对象（含 sc_stop —— 从外部线程调用非法）
//   * socket 线程在 recv/send 期间不持锁
#pragma once

#include "axi_if.h"
#include "gem5_axi_socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <systemc.h>
#include <thread>
#include <vector>

SC_MODULE(Gem5AxiAgent) {
    sc_in<bool> clk;

    // 与 Axi2Flit 互补的 AXI 五通道
    sc_out<bool> aw_valid;
    sc_in<bool> aw_ready;
    sc_out<AxChannel> aw_ch;
    sc_out<bool> w_valid;
    sc_in<bool> w_ready;
    sc_out<WChannel> w_ch;
    sc_out<bool> ar_valid;
    sc_in<bool> ar_ready;
    sc_out<AxChannel> ar_ch;
    sc_in<bool> b_valid;
    sc_out<bool> b_ready;
    sc_in<BChannel> b_ch;
    sc_in<bool> r_valid;
    sc_out<bool> r_ready;
    sc_in<RChannel> r_ch;

    // 统计。注意 w_count / r_count 数的是**拍**，aw/ar/b 与 r_txn_count 数的是
    // **事务**——两套口径不能直接相减比较。
    std::uint64_t aw_count = 0, w_count = 0, ar_count = 0, b_count = 0,
                  r_count = 0, r_txn_count = 0;
    std::uint64_t socket_errors = 0;

    SC_HAS_PROCESS(Gem5AxiAgent);
    Gem5AxiAgent(sc_module_name name, const std::string& socket_path,
                 std::size_t queue_depth = 4);
    ~Gem5AxiAgent() override;

    // gem5 已正常退出（socket EOF）且没有在途事务。
    bool eof_seen() const;
    // 当前没有在途的 gem5 事务。
    bool idle() const;
    const std::string& last_error() const;

private:
    struct Shared;

    std::string socket_path_;
    std::unique_ptr<Shared> shared_;
    std::thread socket_thread_;

    void socket_loop();
    void axi_loop();
};
