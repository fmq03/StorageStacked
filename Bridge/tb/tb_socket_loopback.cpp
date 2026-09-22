// P3a：gem5 在线事务协议的回环验证。
//
// 用一个假 server 扮演 gem5 的 CommMonitor（bind/listen/发请求/收响应），
// 驱动真正的客户端 Gem5AxiSocket。不牵扯 gem5 本体、不牵扯 SystemC——
// 目的是在把整条链路和 gem5 拼到一起之前，先把 48B 请求头、16B 响应头、
// payload 长度和 EOF 语义这些最容易出错的地方钉死。
#include "gem5_axi_socket.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint32_t kMagic = 0x31495841;
constexpr std::uint32_t kReq = 1;
constexpr std::uint32_t kResp = 2;

int failures = 0, checks = 0;
std::vector<std::string> server_notes;

void check(bool ok, const std::string& what) {
    ++checks;
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

int read_all(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r == 0) return 0;
        if (r < 0) return -1;
        got += static_cast<std::size_t>(r);
    }
    return 1;
}

bool write_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

// 假 gem5：完全照 comm_monitor.cc 的写出顺序与校验规则行事。
struct MockGem5 {
    std::string path;
    int listen_fd = -1;
    int fd = -1;
    bool ok = true;

    bool setup() {
        ::unlink(path.c_str());
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;
        return ::listen(listen_fd, 1) == 0;
    }

    bool accept_one() {
        fd = ::accept(listen_fd, nullptr, nullptr);
        return fd >= 0;
    }

    // 发一笔请求；写请求会附带 payload。
    bool send_request(bool write, std::uint64_t txn, std::uint64_t addr,
                      std::uint32_t beats, std::uint32_t size, std::uint32_t id,
                      const std::vector<std::uint8_t>& payload) {
        const std::uint32_t magic = kMagic, type = kReq;
        const std::uint32_t wflag = write ? 1u : 0u, reserved = 0u;
        const std::uint32_t length = beats - 1;
        const std::uint32_t bytes = (length + 1) << size;
        const bool okw =
            write_all(fd, &magic, 4) && write_all(fd, &type, 4) &&
            write_all(fd, &wflag, 4) && write_all(fd, &reserved, 4) &&
            write_all(fd, &txn, 8) && write_all(fd, &addr, 8) &&
            write_all(fd, &length, 4) && write_all(fd, &size, 4) &&
            write_all(fd, &id, 4) && write_all(fd, &bytes, 4);
        if (!okw) return false;
        if (write && bytes > 0) {
            if (payload.size() != bytes) return false;
            if (!write_all(fd, payload.data(), bytes)) return false;
        }
        return true;
    }

    // 收响应；read_expected 为真时校验并取回数据。
    bool recv_response(bool read_expected, std::uint32_t expected_bytes,
                       std::vector<std::uint8_t>* data_out) {
        std::uint32_t magic = 0, type = 0, status = 0, nbytes = 0;
        if (read_all(fd, &magic, 4) <= 0) { ok = false; return false; }
        if (read_all(fd, &type, 4) <= 0) { ok = false; return false; }
        if (read_all(fd, &status, 4) <= 0) { ok = false; return false; }
        if (read_all(fd, &nbytes, 4) <= 0) { ok = false; return false; }

        if (magic != kMagic) { server_notes.push_back("响应 magic 错误"); ok = false; }
        if (type != kResp) { server_notes.push_back("响应 type 错误"); ok = false; }
        if (status != 0) { server_notes.push_back("响应 status 非 0（gem5 会 fatal）"); ok = false; }

        if (read_expected) {
            if (nbytes != expected_bytes) {
                server_notes.push_back("读响应的 response_bytes 与请求不符");
                ok = false;
            }
            if (data_out) {
                data_out->resize(nbytes);
                if (nbytes && read_all(fd, data_out->data(), nbytes) <= 0) {
                    server_notes.push_back("读响应数据中途 EOF");
                    ok = false;
                    return false;
                }
            }
        } else if (nbytes != 0) {
            server_notes.push_back("写响应的 response_bytes 应为 0");
            ok = false;
        }
        return true;
    }

    void teardown() {
        if (fd >= 0) ::close(fd);
        if (listen_fd >= 0) ::close(listen_fd);
        ::unlink(path.c_str());
    }
};

}  // namespace

int main() {
    std::printf("=== P3a：gem5 在线事务协议回环验证 ===\n");

    const std::string path = "/tmp/bridge_p3a.sock";
    MockGem5 srv;
    srv.path = path;
    check(srv.setup(), "假 gem5 server 建立监听");

    // 客户端连接（在单独线程里 connect，避免阻塞主线程的 accept）
    bridge::Gem5AxiSocket client;
    std::atomic<bool> connected{false};
    std::thread connector([&] {
        connected = client.connect(path, 5000);
    });

    check(srv.accept_one(), "假 gem5 accept 客户端");
    connector.join();
    check(connected.load(), "客户端连接成功");
    check(client.connected(), "客户端认为已连接");

    // ---- 用例 1：写请求 ------------------------------------------------
    std::printf("\n写往返\n");
    {
        std::vector<std::uint8_t> payload(64);
        for (unsigned i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(0x30 + i);
        check(srv.send_request(true, 7, 0x1234567800001000ULL, 1, 6, 3, payload),
              "假 gem5 发出写请求（1 拍 × 64B）");

        bridge::Gem5AxiRequest req;
        check(client.read_request(req), "客户端读到请求");
        check(req.write, "解析出 write=true");
        check(req.transaction == 7, "transaction 字段正确");
        check(req.addr == 0x1234567800001000ULL, "addr 字段正确");
        check(req.length == 0 && req.beats() == 1, "AxLEN 解析正确（1 拍）");
        check(req.size == 6 && req.beat_bytes() == 64, "AxSIZE 解析正确（64B/拍）");
        check(req.id == 3, "id 字段正确");
        check(req.bytes == 64, "bytes 字段正确");
        check(req.payload == payload, "写 payload 逐字节正确");

        check(client.send_response({}), "客户端发回写响应");
        check(srv.recv_response(false, 0, nullptr), "假 gem5 收到合法写响应");
    }

    // ---- 用例 2：读请求 ------------------------------------------------
    std::printf("\n读往返\n");
    {
        check(srv.send_request(false, 8, 0x1234567800002000ULL, 4, 6, 5, {}),
              "假 gem5 发出读请求（4 拍 × 64B）");

        bridge::Gem5AxiRequest req;
        check(client.read_request(req), "客户端读到读请求");
        check(!req.write, "解析出 write=false");
        check(req.beats() == 4, "4 拍");
        check(req.bytes == 256, "bytes = 256");
        check(req.payload.empty(), "读请求没有 payload");

        std::vector<std::uint8_t> rd(256);
        for (unsigned i = 0; i < rd.size(); ++i) rd[i] = static_cast<std::uint8_t>(i & 0xFF);
        check(client.send_response(rd), "客户端发回读数据");

        std::vector<std::uint8_t> got;
        check(srv.recv_response(true, 256, &got), "假 gem5 收到合法读响应");
        check(got == rd, "读回数据逐字节一致");
    }

    // ---- 用例 3：非法请求头应被拒绝 ------------------------------------
    std::printf("\n协议校验\n");
    {
        // 手工构造一个 reserved 非 0 的非法头
        const std::uint32_t magic = kMagic, type = kReq, wflag = 0, reserved = 1;
        const std::uint64_t txn = 9, addr = 0x1000;
        const std::uint32_t length = 0, size = 6, id = 0, bytes = 64;
        write_all(srv.fd, &magic, 4); write_all(srv.fd, &type, 4);
        write_all(srv.fd, &wflag, 4); write_all(srv.fd, &reserved, 4);
        write_all(srv.fd, &txn, 8);   write_all(srv.fd, &addr, 8);
        write_all(srv.fd, &length, 4); write_all(srv.fd, &size, 4);
        write_all(srv.fd, &id, 4);     write_all(srv.fd, &bytes, 4);

        bridge::Gem5AxiRequest req;
        check(!client.read_request(req), "reserved 非 0 的请求头被拒绝");
        check(client.last_error().find("reserved") != std::string::npos,
              std::string("拒绝原因可诊断：") + client.last_error());
    }

    // ---- 用例 4：EOF 视为 gem5 正常退出 --------------------------------
    std::printf("\n远端退出\n");
    {
        ::close(srv.fd);
        srv.fd = -1;
        bridge::Gem5AxiRequest req;
        check(!client.read_request(req), "对端关闭后 read_request 返回 false");
        check(client.peer_closed(), "被识别为 peer_closed（正常退出）而非错误");
    }

    srv.teardown();
    client.close();

    for (const auto& n : server_notes) std::printf("  假 gem5 记录：%s\n", n.c_str());
    std::printf("\n%s（%d 项检查，%d 项失败）\n",
                failures ? "P3a FAILED" : "P3a PASSED", checks, failures);
    return failures ? 1 : 0;
}
