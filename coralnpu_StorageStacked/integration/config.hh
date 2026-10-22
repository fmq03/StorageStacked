// 防止同一翻译单元重复包含此头文件。
#pragma once
// 引入共享存储的配置基类及其参数类型。
#include "storage_config.hh"
// 将本项目的接入参数放入 storage_axi 命名空间。
namespace storage_axi {
// 继承公共存储配置，并补充设备和 AXI 主端参数。
struct ConfigParams : StorageConfig {
    // 默认 AXI 与设备周期均为 2000000 fs（2 ns），仿真上限为 20000000000000 fs（20 ms）。
    uint64_t period=2000000, device_period=2000000, max_ticks=20000000000000;
    // 默认允许同时保留 16 笔尚未回送设备的请求。
    unsigned outstanding=16;
    // 默认将一笔 16 字节设备访问拆成 4 个 AXI 数据拍。
    unsigned burst_beats=4;
    // 默认启用确定性的通道延迟与响应反压。
    bool stalls=true;
    // 保存待加载到设备模型的 ELF 程序路径。
    std::string kernel;
// 结束配置参数结构体。
};
// 结束 storage_axi 命名空间。
}
