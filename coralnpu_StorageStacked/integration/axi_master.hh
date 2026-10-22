// 防止本头文件被重复包含。
#pragma once
// 引入共享 AXI 端口和信号数据类型。
#include "axi_signals.hh"
// 引入设备与存储集成参数。
#include "config.hh"
// 引入 CoralNPU 原生模型的创建、推进和完成回调接口。
#include "coralnpu_native.h"
// 引入固定长度数组，用于保存 16 字节设备数据。
#include <array>
// 引入双端队列，用于各通道排队和同 ID 响应保序。
#include <deque>
// 引入文件输出流，用于记录事务和设备请求。
#include <fstream>
// 引入关联容器，用于通过 AXI ID 查找在途事务。
#include <map>
// 引入标准内存管理工具头文件。
#include <memory>
// 将 AXI 适配器声明放入共享存储命名空间。
namespace storage_axi {
// 定义 SystemC 模块，把设备原生请求转换成 AXI 通道传输。
class Master : public sc_core::sc_module {
 // 以下成员是顶层模块可访问的端口、状态及接口。
 public:
    // 声明 AXI 时钟输入和低有效复位输入。
    sc_core::sc_in<bool> clk{"clk"}, resetn{"resetn"};
    // 声明 AW、W、B、AR、R 五个 AXI 通道的主端口集合。
    MasterPorts axi;
    // 声明此类包含 SystemC 进程，支持在自定义构造函数中注册进程。
    SC_HAS_PROCESS(Master);
    // 构造主端模块，并读取本次运行的配置参数。
    Master(sc_core::sc_module_name,const ConfigParams&);
    // 析构时销毁所持有的原生设备模型。
    ~Master();
    // 在途表为空即表示所有已接收的原生访问均已回送设备。
    bool idle() const {return active.empty();}
    // 记录设备模型是否已经停止执行，初始为尚未完成。
    bool done=false;
    // 分别统计接收数、完成数、在途峰值、容量拒绝数、AXI 周期数和设备完成时刻。
    uint64_t accepted=0, completed=0, maxActive=0, capacityDenials=0, cycle=0, doneTick=0;
    // 从原生模型读取设备执行周期数。
    uint64_t cycles() const {return coralnpu_cycles(device);}
    // 从原生模型读取程序写出的 mailbox 状态值。
    uint32_t mailbox() const {return coralnpu_mailbox(device);}
 // 以下成员仅由适配器内部实现使用。
 private:
    // 保存一笔固定 16 字节原生请求及其 AXI 执行状态。
    struct Txn {
        // 保存原生序号、地址、开始及接收时间；AXI 完成时间初始为零。
        uint64_t seq, address, begin, accept, axiDone=0;
        // 保存分配的 AXI ID 和原生请求的 16 位字节掩码。
        uint16_t id, mask;
        // 保存原生请求 ID 和累计 AXI 响应码，响应码初始表示成功。
        uint8_t nativeId, response=0;
        // 区分读写请求，并标记其 AXI 响应是否已收齐。
        bool write, ready=false;
        // 保存写数据或重组后的读数据，初始各字节为零。
        std::array<uint8_t,16> data{};
        // 分别记录已握手的写数据拍数和已收到的读数据拍数。
        unsigned wbeat=0, rbeat=0;
        // 保存 AW 与 W 通道最早允许发出有效信号的 AXI 周期。
        uint64_t awAfter, wAfter;
    // 结束事务状态结构。
    };
    // 保存构造时传入的运行配置副本。
    ConfigParams config;
    // 保存原生设备模型的不透明句柄，创建前为空。
    coralnpu_handle device=nullptr;
    // 独立生成设备时钟，使设备推进与 AXI 时钟分别调度。
    sc_core::sc_clock nativeClock;
    // 从 1 开始分配 AXI ID，实际在实现中按 1 到 1023 循环。
    uint16_t nextId=1;
    // 保存每拍字节数及其以 2 为底的指数，后者用于 AxSIZE。
    unsigned beatBytes=0, beatSize=0;
    // 用 AXI ID 索引所有尚未回送设备的事务。
    std::map<uint16_t,Txn> active;
    // 按读写方向和原生 ID 建立顺序队列，保证同组响应依接收顺序回送。
    std::map<std::pair<bool,uint8_t>,std::deque<uint16_t>> order;
    // 分别保存待发送写地址、写数据与读地址的事务 ID。
    std::deque<uint16_t> awq,wq,arq;
    // 分别输出通用事务日志和原生请求/响应日志。
    std::ofstream trace,source;
    // 在设备时钟沿回送已完成访问并推进原生模型。
    void nativeTick();
    // 在 AXI 时钟沿采样握手、接收响应并更新驱动信号。
    void tick();
    // 根据通道队首事务驱动下一周期的 AXI 输出。
    void drive();
    // 接收原生回调中的序号、地址、ID、读写方向、数据和字节掩码。
    void submit(uint64_t,uint32_t,uint8_t,bool,const uint8_t*,uint16_t);
    // 将指定事件及事务内容写入原生请求日志。
    void sourceRow(const char*,const Txn&);
    // 按各原生 ID 的顺序回送已完成的 AXI 事务。
    void retire();
// 结束 Master 类声明。
};
// 结束 storage_axi 命名空间。
}
