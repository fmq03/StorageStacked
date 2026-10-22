// 引入 gem5/SystemC 存储桥类声明。
#include "storage.hh"
// 引入共享 AoU 存储后端及其配置定义。
#include "aou_backend.hh"
// 引入 gem5 Packet 到 TLM payload 的转换钩子接口。
#include "systemc/tlm_bridge/gem5_to_tlm.hh"
// 引入 gem5 集成的 SystemC 波形类型，用于结束前补录信号。
#include "systemc/utils/tracefile.hh"
// 引入 gem5 当前时间刻度查询接口。
#include "sim/core.hh"
// 引入运行时异常类型。
#include <stdexcept>
// 引入本次构建的 UCIe 链路编译参数。
#include "link_config.h"

// 将桥实现放入 storage_axi 命名空间。
namespace storage_axi {
// 使用 SystemC 核心命名空间，简化模块、时间和波形函数名称。
using namespace sc_core;
// 定义由 gem5 参数对象直接创建模块的公开构造函数。
Bridge::Bridge(const gem5::StorageBridgeParams& p)
    // 把 gem5 对象名转换为 SystemC 模块名，委托给内部构造函数。
    : Bridge(sc_module_name(p.name.c_str()), p) {}
// 定义实际负责模块连接与初始化的构造函数。
Bridge::Bridge(sc_module_name n, const gem5::StorageBridgeParams& p)
    // 用传入的模块名初始化 SystemC 基类。
    : sc_module(n),
      // 按 gem5 参数中的时间刻度生成 AXI 时钟。
      clock("aclk", sc_time::from_value(p.period)),
      // 用在途上限、反压开关和输出目录构造 TLM/AXI 主端。
      master("master", p.outstanding, p.stalls, p.trace_dir),
      // 将主端 socket 包装成 gem5 可连接的 .tlm 端口，使用无索引端口 ID。
      wrapper(master.socket, std::string(name()) + ".tlm", gem5::InvalidPortID),
      // 构造共用 AXI 信号的监控器，并保存输出目录。
      monitor("monitor", wires, p.trace_dir), directory(p.trace_dir) {
    // 将监控器连接到 AXI 时钟与低有效复位。
    monitor.clk(clock); monitor.resetn(resetn);
    // 将主端连接到时钟、复位和公共 AXI 五通道信号。
    master.clk(clock); master.resetn(resetn); master.axi.bind(wires);
    // AoU 可同时使用的 ID 最多为 1023，超过时拒绝此配置。
    if (p.outstanding > 1023) throw std::runtime_error("AoU supports at most 1023 live IDs");
    // 将主端的 AXI ID 分配上限约束为 1023。
    master.maxId = 1023;
    // 创建共享存储配置，先采用其默认值。
    StorageConfig storage;
    // 从 gem5 参数复制存储窗口基址和大小。
    storage.base = p.base; storage.size = p.size;
    // 复制 AoU 平面数和重放测试开关。
    storage.planes = p.planes; storage.replay = p.replay;
    // 复制在线内存模型的在途 burst 槽位数与通道数。
    storage.memsim_slots = p.memsim_slots; storage.memsim_channels = p.memsim_channels;
    // 复制内存时序缩放倍数与队列容量。
    storage.memsim_scale = p.memsim_scale; storage.memsim_queue = p.memsim_queue;
    // 复制以内存 tick 计的响应保持时间。
    storage.memsim_response_hold = p.memsim_response_hold;
    // 复制内存制式名称和日志输出目录。
    storage.memsim_standard = p.memsim_standard; storage.trace_dir = p.trace_dir;
    // 创建并独占持有共享 AoU 后端实例。
    aou = std::make_unique<AouBackend>("aou", storage);
    // 将后端连接到与主端相同的时钟、复位和 AXI 信号。
    aou->clk(clock); aou->resetn(resetn); aou->axi.bind(wires);
    // 安装当前桥的功能访问回调；此实现将该路径报告为不支持。
    master.functional = [](tlm::tlm_generic_payload& gp) {
        // 对调试/阻塞访问返回 TLM 命令错误状态。
        gp.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        // 返回零，表示没有经功能路径读写任何字节。
        return 0u;
    // 结束功能访问回调赋值。
    };
    // 用函数内静态标志记录全局 Packet 转换钩子是否已安装。
    static bool conversionInstalled = false;
    // 仅首个桥实例需要注册共享转换步骤。
    if (!conversionInstalled) {
        // 注册将 gem5 请求属性附加到 TLM payload 的转换回调。
        sc_gem5::addPacketToPayloadConversionStep([](gem5::PacketPtr pkt, tlm::tlm_generic_payload& gp) {
            // 查询当前 payload 是否已经带有本项目的请求扩展。
            auto* a = gp.get_extension<RequestAttributes>();
            // 若扩展不存在则创建，并交给 payload 的自动扩展机制管理。
            if (!a) { a = new RequestAttributes(); gp.set_auto_extension(a); }
            // 读取 gem5 请求中的字节使能向量。
            const auto& be = pkt->req->getByteEnable();
            // 按 Packet 长度创建全使能的字节数组，每个字节使用 0xff 表示有效。
            a->enables.assign(pkt->getSize(), 0xff);
            // 若上游提供了显式字节掩码，则使用它覆盖默认使能值。
            if (!be.empty()) {
                // 检查上游掩码长度与 Packet 数据长度一致。
                sc_assert(be.size() == pkt->getSize());
                // 将布尔字节使能逐项转换为 TLM 使用的 0xff 或 0。
                for (unsigned i = 0; i < be.size(); ++i) a->enables[i] = be[i] ? 0xff : 0;
            // 结束显式字节掩码转换分支。
            }
            // 将 payload 字节使能指针指向扩展对象持有的稳定数组。
            gp.set_byte_enable_ptr(a->enables.data());
            // 设置字节使能数组长度，供后续读写掩码索引使用。
            gp.set_byte_enable_length(a->enables.size());
            // 保存 gem5 请求发起者 ID，供事务日志记录。
            a->requestor = pkt->req->requestorId();
            // 记录请求是否携带流 ID。
            a->hasStream = pkt->req->hasStreamId();
            // 有流 ID 时保存其值，否则填零。
            a->stream = a->hasStream ? pkt->req->streamId() : 0;
            // 记录请求是否携带子流 ID。
            a->hasSubstream = pkt->req->hasSubstreamId();
            // 有子流 ID 时保存其值，否则填零。
            a->substream = a->hasSubstream ? pkt->req->substreamId() : 0;
            // 在 gem5 后续清除字段前保存 payloadDelay，供主端等待数据完整到达。
            a->payloadDelay = pkt->payloadDelay;
            // 开始识别不受当前桥支持的原子操作、LL/SC 和锁定读改写请求。
            if (pkt->isAtomicOp() || pkt->isLLSC() || pkt->isLockedRMW() ||
                // 同时识别交换操作与缓存维护请求。
                pkt->req->isSwap() || pkt->req->isCacheMaintenance())
                // 将这些特殊命令改为 IGNORE，交给主端合法性检查返回错误，避免当普通读写执行。
                gp.set_command(tlm::TLM_IGNORE_COMMAND);
        // 结束并注册 Packet 到 payload 的转换回调。
        });
        // 标记钩子已安装，后续桥实例不再重复注册。
        conversionInstalled = true;
    // 结束全局转换钩子的首次安装分支。
    }
    // 打开 UCIe 构建参数快照文件。
    std::ofstream link(directory + "/ucie_config.json");
    // 写入本次编译的 lane 数与 GT/s 链路速率。
    link << "{\"lanes\":" << LINK_LANES << ",\"rate_gtps\":" << LINK_RATE_GTPS
         // 继续写入每符号位数和 ns 周转时间，结束 JSON。
         << ",\"bits_per_symbol\":" << LINK_BITS_PER_SYMBOL << ",\"tat_ns\":" << LINK_TAT_NS << "}\n";
    // 创建输出目录中的 axi_wave.vcd 波形文件。
    vcd = sc_create_vcd_trace_file((directory + "/axi_wave").c_str());
    // 设置 VCD 时间单位为 1 fs。
    vcd->set_time_unit(1, SC_FS);
    // 注册公共 AXI 时钟和低有效复位的波形。
    sc_trace(vcd, clock, "ACLK"); sc_trace(vcd, resetn, "ARESETn");
    // 注册 AXI 五通道信号的波形。
    wires.trace(vcd);
    // 若后端存在，注册其内部状态波形。
    if (aou) aou->trace(vcd);
    // 注册启动复位线程。
    SC_THREAD(reset);
    // 注册 AXI 上升沿的时钟一致性检查方法，并跳过初始化调用。
    SC_METHOD(checkClock); sensitive << clock.posedge_event(); dont_initialize();
// 结束内部构造函数。
}
// 在完整后端类型可见的实现文件中默认定义析构函数，支持 unique_ptr 正常销毁。
Bridge::~Bridge() = default;
// 定义桥启动时执行的复位线程。
void Bridge::reset() {
    // 拉低低有效复位信号，暂不允许正常 AXI 传输。
    resetn = false;
    // 保持复位 3.5 个 AXI 周期，使释放时刻避开上升沿。
    wait(clock.period() * 3 + clock.period() / 2);
    // 后端存在但尚未就绪时，每隔一个 AXI 周期继续等待。
    while (aou && !aou->ready()) wait(clock.period());
    // 后端就绪后拉高复位信号，启动正常工作。
    resetn = true;
// 结束复位线程。
}
// 定义 gem5 按端口名称查找连接端口的接口；此处不使用索引。
gem5::Port& Bridge::gem5_getPort(const std::string& n, int) {
    // 本桥只暴露 tlm 端口，其他名字均抛出异常。
    if (n != "tlm") throw std::runtime_error("unknown AxiBridge port: " + n);
    // 返回包装主端 TLM 目标 socket 的 gem5 端口。
    return wrapper;
// 结束端口查询函数。
}
// 断言当前 SystemC 时间刻度等于 gem5 curTick，检查两套调度时间一致。
void Bridge::checkClock() { sc_assert(sc_time_stamp().value() == gem5::curTick()); }
// 定义由 Python 调用的最终结果导出函数。
void Bridge::finish() {
    // 如果已经执行过 finish，则直接返回。
    if (finished) return;
    // 提前设置已收尾标志，防止重复进入。
    finished = true;
    // 组装主端接收数、完成数与在途峰值统计。
    AxiMasterStats stats{master.accepted, master.completed, master.maxActive,
                         // 继续加入容量拒绝计数和是否排空的状态。
                         master.capacityDenials, master.idle()};
    // 将 AXI 周期和主端统计交给监控器导出。
    monitor.finish(clock.period().value(), &stats);
    // 后端存在时要求它导出最终存储记录。
    if (aou) aou->finish(directory);
    // 波形句柄有效时执行最后采样和关闭。
    if (vcd) {
        // gem5 can exit before the lowest-priority SystemC trace event runs.
        // Record current signal values at the SAME tick before closing. The
        // VCD destructor alone emits only a timestamp, losing the final edge.
        // 将句柄转成 gem5 TraceFile，在同一时间刻度显式采样当前信号，保留最终边沿。
        static_cast<sc_gem5::TraceFile*>(vcd)->trace(false);
        // 关闭 VCD 文件并写出已采样的波形数据。
        sc_close_vcd_trace_file(vcd);
        // 清空波形句柄，标记文件已关闭。
        vcd = nullptr;
    // 结束波形收尾分支。
    }
// 结束最终导出函数。
}
// 结束 storage_axi 命名空间。
}
