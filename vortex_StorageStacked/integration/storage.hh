// 防止本头文件重复展开。
#pragma once
// 引入 TLM 到 AXI 主端及请求扩展属性声明。
#include "axi_master.hh"
// 引入 AXI 信号监控器和主端统计类型。
#include "axi_monitor.hh"
// 引入 gem5 端口与 SystemC TLM socket 之间的包装器。
#include "systemc/tlm_port_wrapper.hh"
// 引入由 StorageBridge.py 生成的 C++ 参数结构。
#include "params/StorageBridge.hh"
// 将集成桥声明放入 storage_axi 命名空间。
namespace storage_axi {
// 前置声明 AoU 后端，头文件只需保存其智能指针。
class AouBackend;
// 定义嵌入 gem5 的 SystemC 存储桥模块。
class Bridge : public sc_core::sc_module {
 // 以下接口供 gem5 构造、连接和收尾时调用。
 public:
    // 声明该类包含 SystemC 线程和方法进程。
    SC_HAS_PROCESS(Bridge);
    // 根据 gem5 生成的参数显式构造桥对象。
    explicit Bridge(const gem5::StorageBridgeParams&);
    // 按端口名向 gem5 返回 TLM 包装端口；默认索引为 -1。
    gem5::Port& gem5_getPort(const std::string&, int idx = -1) override;
    // 导出最终监控、存储与波形数据，供 Python 结束时调用。
    void finish();
    // 声明析构函数，在实现文件中可见完整后端类型时定义。
    ~Bridge();
 // 以下成员保存桥内部的模块、信号与状态。
 private:
    // 接收显式 SystemC 名称和 gem5 参数的委托构造目标。
    Bridge(sc_core::sc_module_name, const gem5::StorageBridgeParams&);
    // 保存主端、后端和监控器共用的 AXI 五通道信号。
    Signals wires;
    // 保存 AXI 时钟，周期由 gem5 参数传入。
    sc_core::sc_clock clock;
    // 保存低有效复位信号。
    sc_core::sc_signal<bool> resetn{"resetn"};
    // 实例化 TLM 目标到 AXI 主端的适配器。
    Master master;
    // 独占持有共享 AoU 存储后端。
    std::unique_ptr<AouBackend> aou;
    // 用 64 位 TLM 目标包装器向 gem5 暴露主端 socket。
    sc_gem5::TlmTargetWrapper<64> wrapper;
    // 观察 AXI 时序与握手，记录协议统计。
    AxiMonitor monitor;
    // 保存 VCD 波形句柄，初始为空。
    sc_core::sc_trace_file* vcd = nullptr;
    // 保存所有输出记录的目录。
    std::string directory;
    // 标记 finish 是否已执行，初始尚未收尾。
    bool finished = false;
    // 复位线程等待启动延迟及后端就绪后释放复位。
    void reset();
    // 在 AXI 时钟沿检查 SystemC 与 gem5 时间是否一致。
    void checkClock();
// 结束 Bridge 类声明。
};
// 结束 storage_axi 命名空间。
}
