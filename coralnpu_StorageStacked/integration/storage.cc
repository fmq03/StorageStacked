// 引入顶层连接模块的声明。
#include "storage.hh"
// 将顶层实现放入 storage_axi 命名空间。
namespace storage_axi {
// 使用 SystemC 核心命名空间，简化时钟、线程和波形接口调用。
using namespace sc_core;
// 定义接收模块名与运行参数的顶层构造函数。
Demo::Demo(sc_module_name name, const ConfigParams& p)
    // 初始化 SystemC 基类，并按配置时间刻度创建 AXI 时钟。
    : sc_module(name), clock("aclk", sc_time::from_value(p.period)),
      // 构造设备主端、共享存储和观察公共信号的监控器。
      master("master", p), storage("aou", p), monitor("monitor", wires, p.trace_dir),
      // 保存输出目录，进入构造函数体。
      directory(p.trace_dir) {
    // 将设备适配器绑定到公共 AXI 时钟、复位和信号集合。
    master.clk(clock); master.resetn(resetn); master.axi.bind(wires);
    // 将共享存储后端绑定到相同的 AXI 时钟、复位和信号集合。
    storage.clk(clock); storage.resetn(resetn); storage.axi.bind(wires);
    // 将监控器绑定到公共时钟和复位。
    monitor.clk(clock); monitor.resetn(resetn);
    // 在输出目录创建 axi_wave.vcd 波形文件。
    wave = sc_create_vcd_trace_file((directory + "/axi_wave").c_str());
    // 将 VCD 的时间单位设为 1 fs。
    wave->set_time_unit(1, SC_FS);
    // 注册 AXI 时钟 ACLK 和低有效复位 ARESETn 的波形记录。
    sc_trace(wave, clock, "ACLK"); sc_trace(wave, resetn, "ARESETn");
    // 注册公共 AXI 信号和存储后端内部信号的波形记录。
    wires.trace(wave); storage.trace(wave);
    // 注册可调用 wait 的 SystemC 复位线程。
    SC_THREAD(reset);
// 结束顶层模块构造函数。
}
// 析构时若波形文件仍打开，则关闭它。
Demo::~Demo() { if (wave) sc_close_vcd_trace_file(wave); }
// 定义启动时执行的复位线程。
void Demo::reset() {
    // 拉低复位信号，使主端、后端和监控器保持复位。
    resetn = false;
    // 等待 3.5 个 AXI 周期，将后续释放时刻放在非上升沿位置。
    wait(clock.period() * 3 + clock.period() / 2);
    // 若后端尚未就绪，按一个 AXI 周期的间隔继续等待。
    while (!storage.ready()) wait(clock.period());
    // 后端就绪后拉高低有效复位信号，允许系统执行访问。
    resetn = true;
// 结束复位线程。
}
// 定义成功结束时导出各模块记录的收尾函数。
void Demo::finish() {
    // 已经收尾时直接返回，避免重复导出与关闭波形。
    if (finished) return;
    // 要求共享存储后端向输出目录写出最终结果。
    storage.finish(directory);
    // 用主端接收数、完成数和在途峰值初始化统计记录。
    AxiMasterStats stats{master.accepted, master.completed, master.maxActive,
                         // 继续填入容量拒绝数与是否排空的状态，完成统计结构。
                         master.capacityDenials, master.idle()};
    // 把 AXI 周期刻度和主端统计交给协议监控器导出。
    monitor.finish(clock.period().value(), &stats);
    // 关闭波形文件并清空句柄，避免析构时重复关闭。
    sc_close_vcd_trace_file(wave); wave = nullptr;
    // 标记收尾已完成。
    finished = true;
// 结束收尾函数。
}
// 结束 storage_axi 命名空间。
}
