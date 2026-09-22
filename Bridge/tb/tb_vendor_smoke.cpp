// P0a 门禁：确认 vendor/ 下这套（已打 AOU 补丁的）依赖能在本机工具链上
// 编译、链接并真正跑起来。
//
// 这里刻意只做"装配得起来 + 关键契约成立"的检查，不跑业务逻辑——
// 真正的功能验证在 tb_mem_backend 与 tb_full_chain。
#include "aou_target.h"
#include "ucie_aou_adapter.h"
#include "ucie_link.h"

#include <cstdio>
#include <stdexcept>
#include <systemc.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// 桥、Adapter、Target 三方共用同一份 Config，且必须来自同一个
// make_aou_ucie_config()，否则 require_aou_ucie_config 会抛异常。
void config_contract() {
    std::printf("Config 契约\n");
    Config cfg = make_aou_ucie_config();

    check(cfg.flit_format == FlitFormat::AouFormat6, "flit 格式为 AoU Format 6");
    check(cfg.payload_bytes() == 250, "AoU 载荷为 250 字节 PLP");
    check(cfg.flit_bytes() == 256, "AoU 物理帧为 256 字节");
    check(cfg.num_lanes == LINK_LANES, "lane 数与编译宏一致");
    check(cfg.bits_per_ui() == LINK_BITS_PER_SYMBOL, "每 UI 比特数与编译宏一致");

    // make_aou_ucie_config 的关键副作用：把调制方式从 Config 默认的 PAM4
    // 改成与编译宏一致。默认 PAM4 会让速率翻倍，这是最容易踩的坑。
    check(cfg.modulation == (LINK_BITS_PER_SYMBOL == 1 ? Modulation::NRZ
                                                       : Modulation::PAM4),
          "调制方式与编译宏一致（非 Config 默认的 PAM4）");

    bool threw = false;
    Config bad = cfg;
    bad.num_lanes = LINK_LANES + 1;
    try {
        require_aou_ucie_config(bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "不一致的 Config 会被 require_aou_ucie_config 拒绝");
}

// 例化整条 SystemC 装配，确认端口能绑到一起、链路能训练到 Active。
//
// 注意成员声明顺序：cfg_ 与 stats_ 必须在所有模块之前，否则模块构造时
// 读到的是尚未初始化的对象（C++ 按声明顺序初始化）。
SC_MODULE(BridgeHarness) {
    const Config cfg_;
    Stats stats_;

    sc_clock clk{"clk", 2, SC_NS};
    sc_signal<bool> rst_n{"rst_n"};
    sc_signal<unsigned> link_state{"link_state"};
    sc_signal<FlitTransfer> tx{"tx"}, rx{"rx"};
    sc_signal<bool> tx_ready{"tx_ready"}, rx_ready{"rx_ready"};

    // 四个 FDI FIFO + 存储侧请求/响应 FIFO，与 tb_full_link 的拓扑一致。
    sc_fifo<FdiFlit> soc_tx{8}, soc_rx{8}, mem_tx{8}, mem_rx{8};
    sc_fifo<SimpleMemRequest> mem_req{4};
    sc_fifo<SimpleMemResponse> mem_rsp{4};

    UcieAouAdapter adapter{"adapter", cfg_};
    UcieLink link{"link", cfg_, &stats_, sc_time(cfg_.ui_fs(), SC_FS)};
    AouTarget target{"target", cfg_, 1};

    SC_HAS_PROCESS(BridgeHarness);
    explicit BridgeHarness(sc_module_name n, const Config& cfg)
        : sc_module(n), cfg_(cfg) {
        adapter.clk(clk);  adapter.rst_n(rst_n);  adapter.link_state(link_state);
        adapter.tx(tx);    adapter.tx_ready(tx_ready);
        adapter.rx(rx);    adapter.rx_ready(rx_ready);
        adapter.fifo_tx(soc_tx);
        adapter.fifo_rx(soc_rx);

        link.soc_tx_in(soc_tx);
        link.soc_rx_out(soc_rx);
        link.mem_rx_out(mem_rx);
        link.mem_tx_in(mem_tx);
        link.link_state(link_state);

        target.clk(clk);  target.rst_n(rst_n);
        target.link_rx(mem_rx);
        target.link_tx(mem_tx);
        target.mem_req(mem_req);
        target.mem_rsp(mem_rsp);

        SC_THREAD(reset_sequence);
    }

    // 复位保持到链路训练完成再释放——与 tb_full_link 的启动顺序一致。
    void reset_sequence() {
        rst_n.write(false);
        for (unsigned i = 0; i < 100000; ++i) {
            if (link_state.read() == unsigned(LinkState::Active)) break;
            wait(clk.posedge_event());
        }
        rst_n.write(true);
    }

    bool link_active() const { return link_state.read() == unsigned(LinkState::Active); }
};

}  // namespace

// SystemC 库自带 main()，它调用 sc_main()；自定义 main 会与之冲突。
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_FS);

    std::printf("=== Bridge P0a：vendored 依赖自检 ===\n");
    std::printf("AXI_DATA_WIDTH_CFG=%d  LINK_LANES=%d  LINK_RATE_GTPS=%.1f  "
                "LINK_BITS_PER_SYMBOL=%d  AOU_LINK_TAT_NS=%.1f\n\n",
                AXI_DATA_WIDTH, LINK_LANES, LINK_RATE_GTPS, LINK_BITS_PER_SYMBOL,
                AOU_LINK_TAT_NS);

    config_contract();

    std::printf("\nSystemC 装配与链路训练\n");
    Config cfg = make_aou_ucie_config();
    BridgeHarness harness{"harness", cfg};

    // 训练需要 train_ui 个 UI。放足时间，让 status_thread 有时间推进到 Active。
    sc_start(500, SC_NS);

    check(sc_time_stamp().to_double() > 0.0, "SystemC 内核可推进时间");
    check(harness.link_active(), "UcieLink 训练到 Active");
    check(harness.link.link_state.read() == unsigned(LinkState::Active),
          "link_state 端口可读且为 Active");

    std::printf("\n%s（%d 项失败）\n",
                failures ? "P0a FAILED" : "P0a PASSED", failures);
    return failures ? 1 : 0;
}
