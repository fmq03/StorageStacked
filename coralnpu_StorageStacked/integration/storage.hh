// 防止本头文件被重复包含。
#pragma once
// 引入 CoralNPU 原生请求到 AXI 的主端适配器。
#include "axi_master.hh"
// 引入共享 AoU 存储后端。
#include "aou_backend.hh"
// 引入 AXI 协议监控与主端统计类型。
#include "axi_monitor.hh"

// The only connection between the CoralNPU AXI master and the shared storage.
// 将顶层连接模块放入 storage_axi 命名空间。
namespace storage_axi {
// 定义连接 CoralNPU AXI 主端与共享存储的 SystemC 顶层模块。
class Demo : public sc_core::sc_module {
 // 以下为入口程序可调用的公开接口。
 public:
    // 声明此类包含 SystemC 进程，支持注册复位线程。
    SC_HAS_PROCESS(Demo);
    // 使用模块名和运行配置构造顶层连接。
    Demo(sc_core::sc_module_name, const ConfigParams&);
    // 析构时关闭仍打开的 VCD 波形文件。
    ~Demo();
    // 将主端记录的设备执行结束标志提供给入口程序。
    bool done() const { return master.done; }
    // 将原生模型的执行周期数提供给入口程序。
    uint64_t cycles() const { return master.cycles(); }
    // 将设备 mailbox 状态提供给入口程序。
    uint32_t mailbox() const { return master.mailbox(); }
    // 返回设备停止执行时记录的 SystemC 时间刻度。
    uint64_t doneTick() const { return master.doneTick; }
    // 导出存储、协议监控及波形的最终结果。
    void finish();
 // 以下成员为模块内部的连接与记录状态。
 private:
    // 保存主端、存储后端和监控器共用的 AXI 信号集合。
    Signals wires;
    // 生成 AXI 时钟，周期由配置指定。
    sc_core::sc_clock clock;
    // 保存低有效复位信号，供主端、存储和监控器共用。
    sc_core::sc_signal<bool> resetn{"resetn"};
    // 实例化 CoralNPU 原生模型及其 AXI 适配器。
    Master master;
    // 实例化共享 AoU 存储链路后端。
    AouBackend storage;
    // 实例化观察 AXI 握手与事务状态的监控器。
    AxiMonitor monitor;
    // 保存 VCD 波形句柄，关闭后会清空指针。
    sc_core::sc_trace_file* wave = nullptr;
    // 保存日志、波形和最终统计的输出目录。
    std::string directory;
    // 标记是否已执行收尾，防止重复导出或关闭文件。
    bool finished = false;
    // 复位线程先保持复位，再等待存储后端就绪后释放。
    void reset();
// 结束 Demo 类声明。
};
// 结束 storage_axi 命名空间。
}
