// 防止本头文件被重复包含。
#pragma once
// 引入 AXI 信号、主端口与数据总线宽度定义。
#include "axi_signals.hh"
// 引入 TLM 事务、阶段、状态和扩展类型。
#include <tlm>
// 引入便于注册传输回调的 TLM 目标 socket 模板。
#include <tlm_utils/simple_target_socket.h>
// 引入双端队列，用于各通道请求与响应排队。
#include <deque>
// 引入事务日志的文件输出流。
#include <fstream>
// 引入 std::function，用于保存功能访问回调。
#include <functional>
// 引入按 AXI ID 索引在途事务的映射容器。
#include <map>
// 引入独占指针，管理在途事务对象的生命周期。
#include <memory>
// 引入动态数组，用于字节使能与 burst 分段列表。
#include <vector>

// 将 TLM/AXI 适配相关类型放入 storage_axi 命名空间。
namespace storage_axi {
// 定义随 TLM payload 携带的 gem5 请求属性扩展。
struct RequestAttributes : tlm::tlm_extension<RequestAttributes> {
    // 持有字节使能数据，确保 payload 引用的掩码内存持续有效。
    std::vector<unsigned char> enables;
    // 保存请求发起者、流和子流标识，缺省均为零。
    uint32_t requestor = 0, stream = 0, substream = 0;
    // 区分真实存在的流/子流 ID 与缺省填入的零值。
    bool hasStream = false, hasSubstream = false;
    // 保留 gem5 数据到达延迟，以仿真时间刻度计。
    uint64_t payloadDelay = 0;
    // 实现 TLM 扩展的多态复制接口。
    tlm::tlm_extension_base* clone() const override {
        // 复制当前扩展的全部字段，并返回新分配的对象。
        return new RequestAttributes(*this);
    // 结束 clone 实现。
    }
    // 实现从另一个同类 TLM 扩展复制内容的接口。
    void copy_from(const tlm::tlm_extension_base& other) override {
        // 将基类引用还原为本扩展类型，再复制向量与元数据。
        *this = static_cast<const RequestAttributes&>(other);
    // 结束 copy_from 实现。
    }
// 结束请求属性扩展结构。
};

// 定义把非阻塞 TLM 请求转换为定时 AXI 传输的 SystemC 模块。
class Master : public sc_core::sc_module {
  // 以下成员为外部可连接或查询的接口。
  public:
    // 声明 AXI 时钟和低有效复位输入。
    sc_core::sc_in<bool> clk{"clk"}, resetn{"resetn"};
    // 暴露 AW、W、B、AR、R 五个通道的主端口集合。
    MasterPorts axi;
    // TLM socket width is a binding contract, not a limit on payload length.
    // Keep the existing gem5 bridge binding; the timed AXI bus is DataBits wide.
    // 声明 64 位绑定接口的 TLM 目标 socket；实际 AXI 总线宽度由 DataBits 定义。
    tlm_utils::simple_target_socket<Master, 64> socket{"socket"};
    // 保存调试/阻塞路径使用的功能访问回调，返回处理的字节数。
    std::function<unsigned(tlm::tlm_generic_payload&)> functional;
    // 声明该类包含 SystemC 进程。
    SC_HAS_PROCESS(Master);
    // 构造主端，接收模块名、在途槽位数和 stalls 开关。
    Master(sc_core::sc_module_name, unsigned slots, bool stalls,
           // 继续接收事务 CSV 的输出目录。
           const std::string& traceDir);
    // 在途表为空且没有待接收请求时，适配器才处于排空状态。
    bool idle() const { return active.empty() && pending == nullptr; }
    // 允许分配的最大 AXI ID，包含该上限；外部需在仿真前设定。
    unsigned maxId = 65535; // Inclusive wire-ID limit; set before simulation.
    // 统计已接收数、已完成数、在途峰值和因容量不足而等待的次数。
    uint64_t accepted = 0, completed = 0, maxActive = 0, capacityDenials = 0;
    // 保存 AXI 时钟沿计数，供确定性通道延迟与反压使用。
    uint64_t cycle = 0;
  // 以下成员为适配器内部的事务和协议状态。
  private:
    // 描述一个 burst：起始地址、payload 偏移、每拍字节数、拍数和 AxSIZE 编码。
    struct Burst { uint64_t address; unsigned offset, bytes, beats, size; };
    // 保存一笔 TLM 事务的所有 AXI 分段及进度。
    struct Txn {
        // 指向上游 TLM payload，接收时 acquire，最终 END_RESP 时 release。
        tlm::tlm_generic_payload* gp;
        // 保存此事务占用的 AXI ID，所有分段复用该 ID。
        uint16_t id;
        // 保存按地址对齐、长度与 4 KiB 边界拆出的 burst 列表。
        std::vector<Burst> bursts;
        // 保存当前分段索引与该段已握手的写拍/读拍计数。
        unsigned segment = 0, wbeat = 0, rbeat = 0;
        // 保存当前段 AW、W、AR 各通道最早允许有效的 AXI 周期。
        uint64_t awAfter = 0, wAfter = 0, arAfter = 0;
        // 保存请求开始、正式接收和 AXI 全部完成的时间刻度。
        uint64_t begin = 0, accept = 0, axiDone = 0;
    // 结束在途事务结构。
    };
    // 保存允许并行保留的 TLM 事务槽位上限。
    unsigned slots;
    // 保存是否启用确定性通道延迟和响应反压。
    bool stalls;
    // 从 1 开始寻找可用 AXI ID，ID 0 不参与分配。
    uint16_t nextId = 1;
    // 暂存尚未获准进入在途表的请求，最多保留一个。
    tlm::tlm_generic_payload* pending = nullptr;
    // 保存待接收请求的数据最早可完整到达的时间。
    sc_core::sc_time pendingAt;
    // 保存待接收请求计入注入延迟后的开始时间刻度。
    uint64_t pendingBegin = 0;
    // 用 AXI ID 索引独占持有的事务对象。
    std::map<uint16_t, std::unique_ptr<Txn>> active;
    // 分别保存待发 AW、W、AR 的 ID，以及等待 TLM 响应的 ID。
    std::deque<uint16_t> awq, wq, arq, responses;
    // 保存已发 BEGIN_RESP、尚未收到 END_RESP 的 payload。
    tlm::tlm_generic_payload* responding = nullptr;
    // 保存 transactions.csv 输出流。
    std::ofstream trace;
    // 声明非阻塞前向传输处理函数，处理 BEGIN_REQ 和 END_RESP。
    tlm::tlm_sync_enum transport(tlm::tlm_generic_payload&, tlm::tlm_phase&,
                                // 延迟参数由调用方传入，供 BEGIN_REQ 的接收时间计算使用。
                                sc_core::sc_time&);
    // 声明阻塞传输入口，当前实现转到功能访问路径。
    void blocking(tlm::tlm_generic_payload&, sc_core::sc_time&);
    // 声明调试传输入口，返回实际处理的字节数。
    unsigned debug(tlm::tlm_generic_payload&);
    // 拒绝直接内存访问 DMI，要求访问经过适配器路径。
    bool dmi(tlm::tlm_generic_payload&, tlm::tlm_dmi&) { return false; }
    // 在 AXI 上升沿采样握手、接收响应并推进内部状态。
    void tick();
    // 在容量和到达时间满足要求时正式接收 pending 请求。
    void admit();
    // 将当前 burst 的地址与数据传输加入相应通道队列。
    void enqueueSegment(Txn&);
    // 完成一个 burst 后排入下一段，或将整笔事务加入响应队列。
    void finishSegment(Txn&);
    // 按通道队首事务驱动 AXI 输出。
    void drive();
    // 在 TLM 响应通道空闲时发出下一个 BEGIN_RESP。
    void respond();
    // 将任意合法地址与长度拆成符合 AXI 约束的 burst 序列。
    static std::vector<Burst> split(uint64_t address, unsigned length);
// 结束 Master 类声明。
};
// 结束 storage_axi 命名空间。
}
