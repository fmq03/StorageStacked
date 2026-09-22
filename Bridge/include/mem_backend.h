// 堆叠存储后端：把 mem_sim 的 MemorySystem 包成一个 Bridge 侧可用的、
// 与 SystemC 无关的简单对象。
//
// 本头刻意保持 C++17 干净、且不包含任何 mem_sim 头——它的实现
// (src/mem_backend.cpp) 才需要 mem_sim，并以 C++20 单独编译。
//
// 这样切分的起因是系统 libsystemc 3.0.2 以 C++17 构建（导出
// sc_api_version_..._cxx201703L），任何链接它的 TU 都必须是 C++17；
// 而 mem_sim 上游要求 C++20。于是把 mem_sim 关进一个 TU，SystemC 侧
// 只看到本头里这个纯 C++ 类。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bridge {

// 一笔存储事务。对应 AXI 侧的一次突发搬运，或其中的一个切分片。
struct MemTxn {
    bool write = false;
    // stack-local 字节地址（已减去 Bridge 的地址窗口基址）
    std::uint64_t address = 0;
    // 搬运字节数。必须 <= transaction_bytes()。0 表示由 payload.size() 推导
    // （仅写请求可用）。
    std::uint32_t bytes = 0;
    std::vector<std::uint8_t> payload;    // 写数据；读请求留空
    std::vector<std::uint8_t> byte_mask;  // 逐字节掩码，非 0 表示写入该字节
    bool has_byte_mask = false;
    // 上层用于把响应配回原始请求。必须在 pop 到对应响应之前保持唯一。
    std::uint64_t host_request_id = 0;
};

// 一笔存储响应。读请求的 data 是从 MemTxn::address 起、长度等于请求的
// transfer_bytes 的连续字节流。
struct MemResp {
    std::uint64_t host_request_id = 0;
    bool write = false;
    std::vector<std::uint8_t> data;
    // mem_sim 的 ResponseStatus 序数：
    // 0=Ok 1=EccCorrected 2=UninitializedData 3=DataMismatch
    // 4=Retry 5=Timeout 6=EccUncorrectable 7=InvalidTransaction
    int status = 0;
};

class MemBackend {
public:
    struct Options {
        // mem_sim 标准：hbm3 / hbm4 / lpddr5 / lpddr6
        std::string standard = "hbm4";
        // 压成单 controller。mem_sim 的 HBM4 默认是 32 个 channel，直接用会
        // 建出 32 个 Controller，对单链路集成既无意义又慢。
        int channels = 1;
        // 让一笔 mem_sim transaction 覆盖一个 AXI beat（512 位 = 64 字节）。
        int transaction_bytes = 64;
        // 可选：显式指定地址窗口大小；0 表示用 spec 的可寻址容量。
        std::uint64_t window_bytes = 0;
        // 关闭命令 trace。mem_sim 默认开启，长跑会把每条 DRAM 命令连同
        // payload 都留下来，必然 OOM。
        bool retain_command_trace = false;
        // 有界响应队列；0 表示无界。
        std::size_t response_queue_capacity = 0;
    };

    // 不能写成 `const Options& = {}`：嵌套类的默认成员初始化器在所在类
    // 自身尚未完整时不可用。拆成两个构造函数。
    MemBackend();
    explicit MemBackend(const Options& options);
    ~MemBackend();

    MemBackend(const MemBackend&) = delete;
    MemBackend& operator=(const MemBackend&) = delete;

    // 地址窗口大小（字节）。上层据此判定越界——mem_sim 对越界是抛异常而不是
    // 返回错误码，必须在提交前挡住。
    std::uint64_t window_bytes() const;
    // 一个 mem_sim tick 的时间，单位皮秒。
    std::uint64_t tick_ps() const;
    // 一笔 transaction 搬运的字节数。
    std::uint32_t transaction_bytes() const;
    // 到目前为止推进过的 system cycle 数。
    std::uint64_t clock() const;

    // 推进 ticks 个 mem_sim tick，并收下这期间产生的响应。
    void step(std::uint32_t ticks = 1);

    // 提交一笔事务。
    //   0 = 已接受；1 = 反压，必须原样重试同一笔；<0 = 出错（见 last_error）
    int submit(const MemTxn& txn);

    // 取出一笔已完成的响应。返回 false 表示当前没有响应。
    bool poll(MemResp& out);

    // 所有已提交事务是否都已完成、且响应队列已空。
    bool quiescent() const;

    // 收尾：flush 脏行、生成聚合统计。退出前必须调用一次。
    // unsubmitted 是在 cycle 上限处仍未提交的 frontend 请求数。
    void finish(std::uint64_t unsubmitted = 0);

    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bridge
