// P3b：gem5 → Bridge → mem_sim 的完整链路顶层。
//
//   gem5 (CommMonitor, AF_UNIX server)
//     ⇅ 在线事务协议
//   Gem5AxiAgent ─► Axi2Flit ─► UcieAouAdapter ─► UcieLink
//                                                        ⇩
//                  C++ AouTarget + MemSimBackend，或 RTL Bridge + transactor
//                                                        ⇩
//                                                     mem_sim
//
// 与 tb_full_link 的区别只在最上游：那里是内联的 AXI BFM，这里是真实的 gem5
// 通过 socket 驱动。链路中段与存储侧完全一致。
//
// 收尾顺序见 shutdown()：gem5 关闭 socket 后不能立刻 sc_stop，必须先把 UCIe
// 链路与 mem_sim 都排空，否则统计不全、dirty row 也不落盘。
#include "axi2flit.h"
#include "gem5_axi_agent.h"
#ifdef RTL_BRIDGE
#include "rtl_bridge_systemc.h"
#else
#include "aou_target.h"
#include "mem_sim_backend.h"
#endif
#include "ucie_aou_adapter.h"
#include "ucie_link.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <systemc.h>

namespace {

// 地址窗口必须覆盖 gem5 实际访问的物理地址范围。SE 模式下 gem5 从 0 开始
// 布局镜像与堆栈，所以基址取 0、大小取与 soc_axi.py 的 --memory-size 一致。
// mem_sim 的 HBM4 单 channel 容量恰好是 2 GiB。
constexpr std::uint64_t kDefaultBase = 0;
constexpr std::uint64_t kDefaultMemSize = 2ull << 30;  // 2 GiB

}  // namespace

SC_MODULE(Gem5FullChain) {
    // cfg / stats / socket_path_ 必须在所有模块之前声明：C++ 按声明顺序初始化，
    // 而下面的模块构造时就要用到它们。
    Config cfg;
    Stats stats;
    std::string socket_path_;
    const std::uint64_t kMemBase_;
    const std::uint64_t kMemSize_;

    sc_clock clk{"clk", 2, SC_NS};
    sc_signal<bool> rst{"rst_n"};
    sc_signal<unsigned> state{"link_state"};

    sc_signal<bool> av{"aw_valid"}, ar{"aw_ready"};
    sc_signal<bool> wv{"w_valid"}, wr{"w_ready"};
    sc_signal<bool> arv{"ar_valid"}, arr{"ar_ready"};
    sc_signal<bool> bv{"b_valid"}, br{"b_ready"};
    sc_signal<bool> rv{"r_valid"}, rr{"r_ready"};
    sc_signal<AxChannel> aw{"aw_ch"}, ra{"ar_ch"};
    sc_signal<WChannel> w{"w_ch"};
    sc_signal<BChannel> b{"b_ch"};
    sc_signal<RChannel> r{"r_ch"};

    sc_signal<FlitTransfer> tx{"tx"}, rx{"rx"};
    sc_signal<bool> txr{"tx_ready"}, rxr{"rx_ready"};

    sc_fifo<FdiFlit> soc_tx{"soc_tx", 8}, soc_rx{"soc_rx", 8};
    sc_fifo<FdiFlit> mem_tx{"mem_tx", 8}, mem_rx{"mem_rx", 8};
#ifndef RTL_BRIDGE
    sc_fifo<SimpleMemRequest> requests{"requests", 4};
    sc_fifo<SimpleMemResponse> responses{"responses", 4};
#endif

    Axi2Flit bridge{"bridge", 1};
    UcieAouAdapter adapter{"adapter", cfg};
    UcieLink link{"link", cfg, &stats, sc_time(cfg.ui_fs(), SC_FS)};
#ifdef RTL_BRIDGE
    RtlBridgeSystemC target{"target", kMemBase_, kMemSize_};
#else
    AouTarget target{"target", cfg, 1};
    MemSimBackend memory{"memory", kMemBase_, kMemSize_};
#endif
    Gem5AxiAgent agent{"agent", socket_path_};

    bool done = false;
    int failures = 0;

    SC_HAS_PROCESS(Gem5FullChain);
    Gem5FullChain(sc_module_name name, Config config, const std::string& socket_path,
                  std::uint64_t base, std::uint64_t size)
        : sc_module(name), cfg(config), socket_path_(socket_path),
          kMemBase_(base), kMemSize_(size) {
        // ---- AXI 五通道：agent 与 bridge 互补对接 ----
        agent.clk(clk);
        agent.aw_valid(av);  agent.aw_ready(ar);  agent.aw_ch(aw);
        agent.w_valid(wv);   agent.w_ready(wr);   agent.w_ch(w);
        agent.ar_valid(arv); agent.ar_ready(arr); agent.ar_ch(ra);
        agent.b_valid(bv);   agent.b_ready(br);   agent.b_ch(b);
        agent.r_valid(rv);   agent.r_ready(rr);   agent.r_ch(r);

        bridge.clk(clk);     bridge.rst_n(rst);
        bridge.aw_valid(av); bridge.aw_ready(ar); bridge.aw_ch(aw);
        bridge.w_valid(wv);  bridge.w_ready(wr);  bridge.w_ch(w);
        bridge.ar_valid(arv); bridge.ar_ready(arr); bridge.ar_ch(ra);
        bridge.b_valid(bv);  bridge.b_ready(br);  bridge.b_ch(b);
        bridge.r_valid(rv);  bridge.r_ready(rr);  bridge.r_ch(r);
        bridge.flit_out(tx); bridge.flit_ready(txr);
        bridge.flit_in(rx);  bridge.flit_in_ready(rxr);

        adapter.clk(clk);    adapter.rst_n(rst);  adapter.link_state(state);
        adapter.tx(tx);      adapter.tx_ready(txr);
        adapter.rx(rx);      adapter.rx_ready(rxr);
        adapter.fifo_tx(soc_tx);
        adapter.fifo_rx(soc_rx);

        link.soc_tx_in(soc_tx);
        link.soc_rx_out(soc_rx);
        link.mem_rx_out(mem_rx);
        link.mem_tx_in(mem_tx);
        link.link_state(state);

        target.clk(clk);     target.rst_n(rst);
#ifdef RTL_BRIDGE
        target.link_state(state);
#endif
        target.link_rx(mem_rx);
        target.link_tx(mem_tx);
#ifndef RTL_BRIDGE
        target.mem_req(requests);
        target.mem_rsp(responses);

        memory.clk(clk);     memory.rst_n(rst);
        memory.request(requests);
        memory.response(responses);
#endif

        SC_THREAD(startup);
        SC_THREAD(shutdown);
    }

    // 复位保持到链路训练完成再释放——与 tb_full_link 的启动顺序一致。
    void startup() {
        rst.write(false);
        for (unsigned i = 0; i < 200000; ++i) {
            if (state.read() == unsigned(LinkState::Active)) break;
            wait(clk.posedge_event());
        }
        if (state.read() != unsigned(LinkState::Active)) {
            std::printf("[FAIL] 链路训练超时\n");
            ++failures;
            done = true;
            sc_stop();
            return;
        }
        rst.write(true);
    }

    // gem5 退出（socket EOF）后按序收尾。每一步都必须成立，否则
    // sc_stop 会让统计不全、dirty row 不落盘。
    void shutdown() {
        // 1. 等 gem5 关闭连接
        while (!agent.eof_seen()) wait(clk.posedge_event());

        if (!agent.last_error().empty()) {
            std::printf("[FAIL] socket 错误：%s\n", agent.last_error().c_str());
            ++failures;
        }

        // 2. 等 socket 侧在途事务排空
        for (int i = 0; i < 2000000 && !agent.idle(); ++i) wait(clk.posedge_event());
        if (!agent.idle()) {
            std::printf("[FAIL] gem5 侧仍有在途事务未完成\n");
            ++failures;
        }

        // 3. 等链路排空（Target 空闲 + 四个 FDI FIFO 空 + ACK 追平）
        for (int i = 0; i < 2000000; ++i) {
            const bool drained = target.idle() && soc_tx.num_available() == 0 &&
                                 soc_rx.num_available() == 0 &&
                                 mem_tx.num_available() == 0 &&
                                 mem_rx.num_available() == 0 &&
                                 stats.forward.ack_count >= stats.forward.tx_new_flits;
            if (drained) break;
            wait(clk.posedge_event());
        }

        // 4. 等 mem_sim 排空
#ifndef RTL_BRIDGE
        for (int i = 0; i < 2000000 && !memory.memory_quiescent(); ++i) {
            wait(clk.posedge_event());
        }
        if (!memory.memory_quiescent()) {
            std::printf("[WARN] mem_sim 在收尾时仍未 quiescent\n");
        }

        // 5. 生成聚合统计并 flush
        memory.finish_memory();
#else
        if (!target.idle()) {
            std::printf("[WARN] RTL Bridge/mem_sim 在收尾时仍未 quiescent\n");
        }
        target.finish();
#endif

        // 6. 报告
        std::printf("\n==== 全链路结果 ====\n");
        std::printf("gem5→AXI : AW=%llu W=%llu AR=%llu\n",
                    (unsigned long long)agent.aw_count,
                    (unsigned long long)agent.w_count,
                    (unsigned long long)agent.ar_count);
        std::printf("AXI→gem5 : B=%llu R=%llu(拍) R事务=%llu\n",
                    (unsigned long long)agent.b_count,
                    (unsigned long long)agent.r_count,
                    (unsigned long long)agent.r_txn_count);
#ifdef RTL_BRIDGE
        std::printf("RTL      : submitted=%llu completed=%llu synthesized_errors=%llu "
                    "retries=%llu\n",
                    (unsigned long long)target.submitted,
                    (unsigned long long)target.completed,
                    (unsigned long long)target.synthesized_errors,
                    (unsigned long long)target.retries);
#else
        std::printf("Target   : reads=%llu writes=%llu max_outstanding=%u\n",
                    (unsigned long long)target.reads,
                    (unsigned long long)target.writes, target.max_outstanding);
#endif
        std::printf("UCIe     : fwd=%llu rev=%llu crc_fail=%llu replay=%llu\n",
                    (unsigned long long)stats.forward.tx_new_flits,
                    (unsigned long long)stats.reverse.tx_new_flits,
                    (unsigned long long)stats.forward.crc_fail_count,
                    (unsigned long long)stats.forward.tx_replay_flits);
#ifndef RTL_BRIDGE
        std::printf("Bridge   : completed=%llu errors=%llu submitted=%llu "
                    "retried=%llu 拒绝(越界=%llu lock=%llu strobe=%llu 契约=%llu)\n",
                    (unsigned long long)memory.completed,
                    (unsigned long long)memory.error_responses,
                    (unsigned long long)memory.submitted_txns,
                    (unsigned long long)memory.retried_submits,
                    (unsigned long long)memory.rejected_out_of_range,
                    (unsigned long long)memory.rejected_lock,
                    (unsigned long long)memory.rejected_strobe,
                    (unsigned long long)memory.rejected_contract);
#endif

        // 7. 一致性：每笔 gem5 写都该有一个 B，每笔读都该有数据返回
        if (agent.aw_count != agent.b_count) {
            std::printf("[FAIL] 写事务数不匹配：AW=%llu B=%llu\n",
                        (unsigned long long)agent.aw_count,
                        (unsigned long long)agent.b_count);
            ++failures;
        }
        if (agent.ar_count != agent.r_txn_count) {
            std::printf("[FAIL] 读事务数不匹配：AR=%llu RLAST=%llu\n",
                        (unsigned long long)agent.ar_count,
                        (unsigned long long)agent.r_txn_count);
            ++failures;
        }
        if (stats.forward.crc_fail_count > 0) {
            std::printf("[WARN] 链路出现 %llu 次 CRC 错误（已由重传恢复）\n",
                        (unsigned long long)stats.forward.crc_fail_count);
        }

        // 防"假通过"：链路机械上跑通了、gem5 也正常退出，但如果地址窗口与
        // gem5 实际访问的范围不重叠，所有访问都会被预校验拒掉，mem_sim 一个
        // 字节都没动。这种情形在 --axi-direct-shadow 下 gem5 照样拿到正确响应、
        // 程序照样跑完，从表面看完全正常。必须显式判失败。
#ifdef RTL_BRIDGE
        if ((agent.aw_count != 0 || agent.ar_count != 0) && target.submitted == 0) {
            std::printf("[FAIL] gem5 产生了访问，但 RTL Bridge 未向 mem_sim 提交事务；"
                        "请检查地址窗口。\n");
            ++failures;
        }
#else
        if (memory.completed > 0 && memory.submitted_txns == 0) {
            std::printf("[FAIL] 全部 %llu 笔访问都被预校验拒绝，mem_sim 未收到任何事务——\n"
                        "       地址窗口 [0x%llx, 0x%llx) 与 gem5 实际访问范围不重叠。\n"
                        "       请用 --base/--size 对齐（gem5 侧 --memory-size 默认为 2GiB）。\n",
                        (unsigned long long)memory.completed,
                        (unsigned long long)kMemBase_,
                        (unsigned long long)(kMemBase_ + kMemSize_));
            ++failures;
        }
#endif

        done = true;
        sc_stop();
    }
};

int sc_main(int argc, char** argv) {
    sc_set_time_resolution(1, SC_FS);

    std::string socket_path;
    std::uint64_t base = kDefaultBase;
    std::uint64_t size = kDefaultMemSize;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            base = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = std::strtoull(argv[++i], nullptr, 0);
        } else {
            std::fprintf(stderr,
                         "用法: %s --socket <path> [--base <addr>] [--size <bytes>]\n",
                         argv[0]);
            return 2;
        }
    }
    if (socket_path.empty()) {
        std::fprintf(stderr, "必须提供 --socket <gem5 的监听路径>\n");
        return 2;
    }

    std::printf("=== P3b：gem5 全链路 ===\n");
    std::printf("socket=%s  AXI_DATA_WIDTH=%d  地址窗口=[0x%llx, 0x%llx) (%.2f GiB)\n",
                socket_path.c_str(), AXI_DATA_WIDTH,
                (unsigned long long)base,
                (unsigned long long)(base + size),
                double(size) / 1073741824.0);

    // 桥的逐帧日志默认是开的（aou_types.h 里 g_aou_verbose = true）。gem5
    // 会送来大量帧，开着它光是写日志就能把仿真拖慢几个数量级。
    g_aou_verbose = false;

    Config cfg = make_aou_ucie_config();
    // 链路侧去掉随机损伤，先保证功能正确性可判读。
    cfg.awgn_sigma = cfg.jitter_sigma_ui = cfg.isi_h1 = cfg.isi_h2 = 0;
    cfg.lane_skew_max_ui = 0;
    cfg.extra_flit_error_rate = 0.0;
    require_valid_config(cfg);

    Gem5FullChain chain{"chain", cfg, socket_path, base, size};

    // 不设时限：gem5 真实运行时可能长时间没有访存，设时限会让 SystemC 侧
    // 在空转中提前耗尽预算而退出。收尾完全靠 shutdown() 里的 sc_stop()。
    sc_start();

    std::printf("\n%s\n", chain.failures ? "P3b FAILED" : "P3b PASSED");
    return chain.failures ? 1 : 0;
}
