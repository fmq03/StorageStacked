/**
 * @file tb_axi2flit.cpp
 * @brief AXI2FLIT 仿真顶层（Testbench）
 *
 * 包含三个测试场景：
 *   TC1：单笔读请求（延迟场景，验证超时触发）
 *   TC2：单笔写事务（WriteReq + WriteDataFull × 4 beats）
 *   TC3：读写混合高并发（多笔并行，验证带宽利用率）
 *
 * 运行后在当前目录生成 waveform.vcd，用 GTKWave 打开即可查看波形：
 *   gtkwave waveform.vcd
 *
 * 编译与运行：
 *   make && ./sim/axi2flit_sim
 */

#include <systemc.h>
#include <iostream>
#include <iomanip>
#include "axi2flit.h"

// ============================================================
//  仿真时钟参数
// ============================================================
static constexpr double CLK_PERIOD_NS = 2.0;   // 2ns 时钟（对应 28nm ~500MHz）
static constexpr int    SIM_CYCLES    = 200;    // 最大仿真时钟周期数

// ============================================================
//  带宽统计辅助：记录输出 flit 数量和 granule 总量
// ============================================================
static int total_flits    = 0;
static int total_granules = 0;

// ============================================================
//  FDI 监视器模块：观察 flit_out 并打印统计
// ============================================================
SC_MODULE(FdiMonitor) {
    sc_in<bool>          clk;
    sc_in<FlitTransfer>  flit_in;
    sc_out<bool>         flit_ready; // 始终拉高（理想接收端，无背压）

    SC_CTOR(FdiMonitor) {
        SC_THREAD(monitor_thread);
        sensitive << clk.pos();
    }

    void monitor_thread() {
        flit_ready.write(true);  // 始终就绪
        while (true) {
            wait();
            FlitTransfer ft = flit_in.read();
            if (ft.valid) {
                total_flits++;
                total_granules += ft.flit.used_granules;
                double util = 100.0 * ft.flit.used_granules / GRANULE_COUNT;
                std::cout << "[FdiMonitor] @" << sc_time_stamp()
                          << " Received Flit #" << total_flits
                          << "  granules=" << ft.flit.used_granules << "/48"
                          << "  util=" << std::fixed << std::setprecision(1) << util << "%"
                          << "  msgstart=0x" << std::hex << ft.flit.msg_start << std::dec
                          << std::endl;
            }
        }
    }
};

// ============================================================
//  AXI 激励驱动器模块
// ============================================================
SC_MODULE(AxiDriver) {
    sc_in<bool>       clk;
    sc_in<bool>       rst_n;

    // AW 通道输出
    sc_out<bool>      aw_valid;
    sc_in<bool>       aw_ready;
    sc_out<AxChannel> aw_ch;

    // W 通道输出
    sc_out<bool>      w_valid;
    sc_in<bool>       w_ready;
    sc_out<WChannel>  w_ch;

    // AR 通道输出
    sc_out<bool>      ar_valid;
    sc_in<bool>       ar_ready;
    sc_out<AxChannel> ar_ch;

    SC_CTOR(AxiDriver) {
        SC_THREAD(drive_thread);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

    // ------ 辅助：等待 N 个时钟周期 ------
    void wait_cycles(int n) {
        for (int i = 0; i < n; ++i) wait();
    }

    // ------ 辅助：发送一笔 AW beat（阻塞直到握手完成）------
    // DUT 使用 wait-first 风格：在时钟沿读信号。
    // 驱动器：设置 valid，然后每拍检查 ready；看到 ready=true 时认为握手完成。
    void send_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
        AxChannel aw;
        aw.id   = id;
        aw.addr = addr;
        aw.len  = len;
        aw.size = 5;
        aw_ch.write(aw);
        aw_valid.write(true);
        // 等待 DUT 拉高 ready（DUT 在 wait-first 的 ready=true 那拍完成握手）
        do { wait(); } while (!aw_ready.read());
        aw_valid.write(false);
        std::cout << "[AxiDriver] @" << sc_time_stamp()
                  << " AW  id=" << (int)id << " addr=0x" << std::hex << addr
                  << std::dec << " len=" << (int)len << " ACCEPTED" << std::endl;
    }

    // ------ 辅助：发送一笔 W beat（阻塞直到握手完成）------
    void send_w(uint8_t beat_data_val, bool last, uint16_t user = 0) {
        WChannel w;
        // 填充数据（简单填充为固定值，用于验证字段传递）
        std::fill(std::begin(w.data), std::end(w.data), beat_data_val);
        // strobe 全 1（全字节有效，触发 WriteDataFull 路径）
        std::fill(std::begin(w.strb), std::end(w.strb), 0xFF);
        w.last = last;
        w.user = user;
        w_ch.write(w);
        w_valid.write(true);
        do { wait(); } while (!w_ready.read());
        w_valid.write(false);
        std::cout << "[AxiDriver] @" << sc_time_stamp()
                  << " W   data[0]=0x" << std::hex << (int)beat_data_val
                  << " last=" << std::dec << last << " ACCEPTED" << std::endl;
    }

    // ------ 辅助：发送一笔 AR beat ------
    void send_ar(uint8_t id, uint64_t addr, uint8_t len = 0) {
        AxChannel ar;
        ar.id   = id;
        ar.addr = addr;
        ar.len  = len;
        ar.size = 5;
        ar_ch.write(ar);
        ar_valid.write(true);
        do { wait(); } while (!ar_ready.read());
        ar_valid.write(false);
        std::cout << "[AxiDriver] @" << sc_time_stamp()
                  << " AR  id=" << (int)id << " addr=0x" << std::hex << addr
                  << std::dec << " len=" << (int)len << " ACCEPTED" << std::endl;
    }

    // ============================================================
    //  主激励线程
    // ============================================================
    void drive_thread() {
        // 初始化所有输出为无效
        aw_valid.write(false);
        w_valid.write(false);
        ar_valid.write(false);
        aw_ch.write(AxChannel{});
        w_ch.write(WChannel{});
        ar_ch.write(AxChannel{});
        wait();  // 等待复位完成

        // ==================================================
        //  TC1：单笔读请求（延迟场景）
        //  目的：验证在队列只有 1 条消息时，超时触发能在
        //        FLUSH_TIMEOUT_CYCLES 个周期内将 flit 发出
        // ==================================================
        std::cout << "\n===== TC1: 单笔 ReadReq 延迟场景 =====" << std::endl;
        send_ar(/*id=*/0x01, /*addr=*/0x1000'0000, /*len=*/0);
        wait_cycles(8);  // 观察超时触发，约 4 个周期后 flit 应当发出

        // ==================================================
        //  TC2：单笔写事务（WriteReq + 4 beats WriteDataFull）
        //  目的：验证 AW 和 W 通道的消息均能正确打包进 flit
        // ==================================================
        std::cout << "\n===== TC2: 单笔写事务（1+4 消息）=====" << std::endl;
        send_aw(/*id=*/0x02, /*addr=*/0x2000'0000, /*len=*/3);  // 4-beat burst
        // 发送 4 个 W beat
        for (int i = 0; i < 4; ++i) {
            bool last = (i == 3);
            send_w(/*data=*/0xAA + i, /*last=*/last);
        }
        wait_cycles(10);

        // ==================================================
        //  TC3：读写混合并发（高带宽场景）
        //  目的：验证混合消息的打包效率，统计最终 granule 利用率
        //  发送 4 笔读请求 + 2 笔写事务，期望多条消息聚合进同一 flit
        // ==================================================
        std::cout << "\n===== TC3: 读写混合并发（高带宽）=====" << std::endl;

        // 快速连续发送 4 笔 AR
        for (int i = 0; i < 4; ++i) {
            send_ar(0x10 + i, 0x3000'0000 + i * 0x100, /*len=*/3);
        }
        // 同时发送 2 笔写事务（AW + W）
        for (int i = 0; i < 2; ++i) {
            send_aw(0x20 + i, 0x4000'0000 + i * 0x200, /*len=*/1);
            for (int b = 0; b < 2; ++b) {
                send_w(0xBB + i, /*last=*/(b == 1));
            }
        }
        wait_cycles(20);

        // ==================================================
        //  打印最终带宽利用率统计
        // ==================================================
        std::cout << "\n===== 仿真统计 =====" << std::endl;
        std::cout << "  总输出 flit 数：  " << total_flits << std::endl;
        std::cout << "  总 granule 使用：  " << total_granules << std::endl;
        if (total_flits > 0) {
            double avg_util = 100.0 * total_granules / (total_flits * GRANULE_COUNT);
            std::cout << "  平均 flit 利用率： "
                      << std::fixed << std::setprecision(1) << avg_util << "%" << std::endl;
        }

        sc_stop();  // 结束仿真
    }
};

// ============================================================
//  sc_main：仿真顶层
// ============================================================
int sc_main(int argc, char* argv[]) {
    std::cout << "AXI2FLIT SystemC 仿真启动" << std::endl;
    std::cout << "时钟周期：" << CLK_PERIOD_NS << " ns" << std::endl;

    // ------ 时钟和复位信号 ------
    sc_clock clk("clk", CLK_PERIOD_NS, SC_NS);
    sc_signal<bool> rst_n("rst_n");

    // ------ AXI 接口信号 ------
    sc_signal<bool>      aw_valid("aw_valid"), aw_ready("aw_ready");
    sc_signal<bool>      w_valid("w_valid"),   w_ready("w_ready");
    sc_signal<bool>      ar_valid("ar_valid"), ar_ready("ar_ready");
    sc_signal<AxChannel> aw_ch("aw_ch"), ar_ch("ar_ch");
    sc_signal<WChannel>  w_ch("w_ch");

    // ------ FDI 输出信号 ------
    sc_signal<FlitTransfer> flit_out("flit_out");
    sc_signal<bool>         flit_ready("flit_ready");

    // ------ 实例化被测模块（DUT）------
    Axi2Flit dut("axi2flit");
    dut.clk(clk);
    dut.rst_n(rst_n);
    dut.aw_valid(aw_valid);  dut.aw_ready(aw_ready);  dut.aw_ch(aw_ch);
    dut.w_valid(w_valid);    dut.w_ready(w_ready);    dut.w_ch(w_ch);
    dut.ar_valid(ar_valid);  dut.ar_ready(ar_ready);  dut.ar_ch(ar_ch);
    dut.flit_out(flit_out);  dut.flit_ready(flit_ready);

    // ------ 实例化 FDI 监视器 ------
    FdiMonitor monitor("fdi_monitor");
    monitor.clk(clk);
    monitor.flit_in(flit_out);
    monitor.flit_ready(flit_ready);

    // ------ 实例化 AXI 激励驱动器 ------
    AxiDriver driver("axi_driver");
    driver.clk(clk);       driver.rst_n(rst_n);
    driver.aw_valid(aw_valid); driver.aw_ready(aw_ready); driver.aw_ch(aw_ch);
    driver.w_valid(w_valid);   driver.w_ready(w_ready);   driver.w_ch(w_ch);
    driver.ar_valid(ar_valid); driver.ar_ready(ar_ready); driver.ar_ch(ar_ch);

    // ------ 打开 VCD 波形文件 ------
    sc_trace_file* tf = sc_create_vcd_trace_file("sim/waveform");
    tf->set_time_unit(1, SC_NS);

    // 追踪时钟和复位
    sc_trace(tf, clk,      "clk");
    sc_trace(tf, rst_n,    "rst_n");

    // 追踪 AXI 接口信号
    sc_trace(tf, aw_valid, "aw_valid");
    sc_trace(tf, aw_ready, "aw_ready");
    sc_trace(tf, aw_ch,    "aw");

    sc_trace(tf, w_valid,  "w_valid");
    sc_trace(tf, w_ready,  "w_ready");
    sc_trace(tf, w_ch,     "w");

    sc_trace(tf, ar_valid, "ar_valid");
    sc_trace(tf, ar_ready, "ar_ready");
    sc_trace(tf, ar_ch,    "ar");

    // 追踪 FDI 输出
    sc_trace(tf, flit_out,   "flit_out");
    sc_trace(tf, flit_ready, "flit_ready");

    // ------ 复位序列：低电平复位 2 个周期后释放 ------
    rst_n.write(false);
    sc_start(CLK_PERIOD_NS * 2, SC_NS);
    rst_n.write(true);

    // ------ 启动仿真（最多运行 SIM_CYCLES 个周期）------
    sc_start(CLK_PERIOD_NS * SIM_CYCLES, SC_NS);

    // ------ 关闭 VCD 文件 ------
    sc_close_vcd_trace_file(tf);

    std::cout << "\n仿真结束，波形已保存至 sim/waveform.vcd" << std::endl;
    std::cout << "使用 GTKWave 查看：  gtkwave sim/waveform.vcd" << std::endl;

    return 0;
}
