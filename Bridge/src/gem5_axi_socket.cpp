#include "gem5_axi_socket.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace bridge {
namespace {

// 与 gem5/src/mem/comm_monitor.cc:100-102 保持一致。
constexpr std::uint32_t kMagic = 0x31495841;  // "AXI1" 的小端字节序
constexpr std::uint32_t kTypeRequest = 1;
constexpr std::uint32_t kTypeResponse = 2;

// 读满 n 字节。EOF 返回 0，出错返回 -1，成功返回 1。
int read_all(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r == 0) return 0;  // 对端关闭
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += static_cast<std::size_t>(r);
    }
    return 1;
}

bool write_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

}  // namespace

Gem5AxiSocket::~Gem5AxiSocket() { close(); }

bool Gem5AxiSocket::connect(const std::string& path, int timeout_ms) {
    close();
    error_.clear();
    peer_closed_ = false;

    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        error_ = "socket 路径过长（上限 108 字节）";
        return false;
    }

    const int deadline_ms = timeout_ms;
    for (int waited = 0;; waited += 1) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            error_ = std::string("socket(): ") + std::strerror(errno);
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fd_ = fd;
            return true;
        }
        const int err = errno;
        ::close(fd);
        if (waited >= deadline_ms) {
            error_ = std::string("connect(") + path + ") 超时: " + std::strerror(err);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool Gem5AxiSocket::read_request(Gem5AxiRequest& out) {
    if (fd_ < 0) {
        error_ = "未连接";
        return false;
    }
    std::uint32_t magic = 0, type = 0, write_flag = 0, reserved = 0;
    std::uint64_t transaction = 0, addr = 0;
    std::uint32_t length = 0, size = 0, id = 0, bytes = 0;

    // 按 gem5 的写出顺序逐字段读，不依赖结构体布局。
    struct Field {
        void* p;
        std::size_t n;
    };
    const Field fields[] = {
        {&magic, 4},      {&type, 4},   {&write_flag, 4}, {&reserved, 4},
        {&transaction, 8}, {&addr, 8},  {&length, 4},     {&size, 4},
        {&id, 4},          {&bytes, 4},
    };
    for (const auto& f : fields) {
        const int rc = read_all(fd_, f.p, f.n);
        if (rc == 0) {
            peer_closed_ = true;  // gem5 正常退出
            return false;
        }
        if (rc < 0) {
            error_ = std::string("读取请求头失败: ") + std::strerror(errno);
            return false;
        }
    }

    if (magic != kMagic) {
        error_ = "请求头 magic 不匹配";
        return false;
    }
    if (type != kTypeRequest) {
        error_ = "请求头 type 不是 REQUEST";
        return false;
    }
    if (reserved != 0) {
        error_ = "请求头 reserved 非 0";
        return false;
    }
    if (write_flag > 1) {
        error_ = "请求头 write_flag 非法";
        return false;
    }
    if (size > 7 || length > 255) {
        error_ = "请求头 AxSIZE/AxLEN 越界";
        return false;
    }
    if (bytes != (static_cast<std::uint64_t>(length) + 1) << size) {
        error_ = "请求头 bytes 与 (AxLEN+1)<<AxSIZE 不一致";
        return false;
    }

    out = Gem5AxiRequest{};
    out.write = write_flag != 0;
    out.transaction = transaction;
    out.addr = addr;
    out.length = length;
    out.size = size;
    out.id = id;
    out.bytes = bytes;

    if (out.write && bytes > 0) {
        out.payload.resize(bytes);
        const int rc = read_all(fd_, out.payload.data(), bytes);
        if (rc <= 0) {
            error_ = rc == 0 ? "写 payload 中途 EOF" : "读取写 payload 失败";
            return false;
        }
    }
    return true;
}

bool Gem5AxiSocket::send_response(const std::vector<std::uint8_t>& read_data) {
    if (fd_ < 0) {
        error_ = "未连接";
        return false;
    }
    const std::uint32_t magic = kMagic;
    const std::uint32_t type = kTypeResponse;
    // status 必须为 0：gem5 侧对非 0 直接 fatal()，协议里没有表达错误的通路。
    const std::uint32_t status = 0;
    const std::uint32_t response_bytes =
        static_cast<std::uint32_t>(read_data.size());

    if (!write_all(fd_, &magic, 4) || !write_all(fd_, &type, 4) ||
        !write_all(fd_, &status, 4) || !write_all(fd_, &response_bytes, 4)) {
        error_ = std::string("写响应头失败: ") + std::strerror(errno);
        return false;
    }
    if (response_bytes > 0 &&
        !write_all(fd_, read_data.data(), response_bytes)) {
        error_ = std::string("写读数据失败: ") + std::strerror(errno);
        return false;
    }
    return true;
}

void Gem5AxiSocket::close() {
    if (fd_ >= 0) {
        // shutdown 才能唤醒可能阻塞在 recv 上的线程；单靠 close 不可靠。
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace bridge
