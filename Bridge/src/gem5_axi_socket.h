// gem5 CommMonitor 的 AXI4 在线事务协议 —— 客户端侧。
//
// 协议由 gem5 树内的本地提交 b90db1aff3 定义（src/mem/comm_monitor.cc）：
//   * gem5 是 **server**（AF_UNIX / SOCK_STREAM），先 bind+listen；对端必须
//     以 client 身份连接，并在 gem5 尚未就绪时重试。
//   * 请求头固定 48 字节，本机原生字节序、无结构体填充（gem5 逐字段写出）。
//   * 写请求头后紧跟 `bytes` 个字节的 WDATA。
//   * 响应头固定 16 字节；读请求后紧跟 response_bytes 个字节的数据。
//   * 严格 lock-step：同一时刻只有一笔在途，gem5 在等到响应前不会发出下一笔。
//   * 对端读到 EOF（recv 返回 0）表示 gem5 正常退出。
//
// 本头不依赖 SystemC，可被 C++17 的 TU 直接包含。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bridge {

// gem5 发给对端的一笔 AXI4 INCR 突发。
struct Gem5AxiRequest {
    bool write = false;
    std::uint64_t transaction = 0;  // 同一个 gem5 packet 切出的多个 burst 共享
    std::uint64_t addr = 0;
    std::uint32_t length = 0;  // AxLEN = 拍数 - 1
    std::uint32_t size = 0;    // AxSIZE = log2(每拍字节数)
    std::uint32_t id = 0;      // 截断后的 requestorId，不是事务唯一标识
    std::uint32_t bytes = 0;   // 本 burst 的字节数 = (length+1) << size
    std::vector<std::uint8_t> payload;  // 仅写请求

    unsigned beats() const { return length + 1; }
    unsigned beat_bytes() const { return 1u << size; }
};

class Gem5AxiSocket {
public:
    Gem5AxiSocket() = default;
    ~Gem5AxiSocket();

    Gem5AxiSocket(const Gem5AxiSocket&) = delete;
    Gem5AxiSocket& operator=(const Gem5AxiSocket&) = delete;

    // 连接到 gem5 的监听 socket。gem5 在 m5.instantiate() 阶段 bind/listen，
    // 因此这里按 timeout_ms 轮询重试。
    bool connect(const std::string& path, int timeout_ms = 30000);

    // 阻塞读取下一笔请求。返回 false 表示对端已关闭（gem5 正常退出）或出错。
    bool read_request(Gem5AxiRequest& out);

    // 发送响应。写请求传空的 read_data（response_bytes 必须为 0）；读请求必须
    // 恰好传回请求里 bytes 个字节——gem5 对长度不符会直接 fatal。
    bool send_response(const std::vector<std::uint8_t>& read_data);

    void close();
    bool connected() const { return fd_ >= 0; }
    const std::string& last_error() const { return error_; }
    // 对端正常关闭（EOF），与真正的错误区分开。
    bool peer_closed() const { return peer_closed_; }

private:
    int fd_ = -1;
    std::string error_;
    bool peer_closed_ = false;
};

}  // namespace bridge
