// 扮演 gem5 的 CommMonitor，用于在 gem5 尚未构建/接入时先验证整条链路。
//
// 行为严格照 comm_monitor.cc：作为 AF_UNIX server bind+listen，逐笔发送
// AXI4 INCR 突发并同步等待响应，最后关闭连接（对端据此判定 gem5 正常退出）。
//
// 它自己带回一个字节级参考模型：写进去什么，读回来必须一模一样。因此
// 这一个程序就能端到端验证
//   socket → Gem5AxiAgent → Axi2Flit → UCIe → AouTarget → Bridge → mem_sim
// 整条路径上的数据正确性，而不只是"跑完了"。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint32_t kMagic = 0x31495841;
constexpr std::uint32_t kReq = 1;
constexpr std::uint32_t kResp = 2;
// 必须与 tb_gem5_full_chain 的地址窗口一致。默认取 0——真 gem5 在 SE 模式下
// 就是从 0 开始布局镜像的；用别的基址会让所有访问落到窗口外，mem_sim 收不到
// 任何事务，而表面看仍然"跑通了"。
std::uint64_t kBase = 0;
unsigned kBeatBytes = 64;
unsigned kBeatLog2 = 6;

int failures = 0;

bool write_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

bool read_all(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// 参考内存：按 64B 行存。
std::map<std::uint64_t, std::vector<std::uint8_t>> golden;

struct Server {
    int listen_fd = -1, fd = -1;
    std::uint64_t txn = 0;

    bool setup(const std::string& path) {
        ::unlink(path.c_str());
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            return false;
        return ::listen(listen_fd, 1) == 0;
    }

    bool accept_one() {
        fd = ::accept(listen_fd, nullptr, nullptr);
        return fd >= 0;
    }

    // 发出请求并等待响应。write=true 时读回数据为空；否则 data_out 收数据。
    bool transact(bool write, std::uint64_t addr, std::uint32_t beats,
                  std::uint32_t log2_beat, const std::vector<std::uint8_t>& payload,
                  std::vector<std::uint8_t>* data_out) {
        const std::uint32_t length = beats - 1;
        const std::uint32_t bytes = (length + 1) << log2_beat;
        const std::uint32_t id = 0;
        const std::uint32_t type = kReq, wflag = write ? 1u : 0u, reserved = 0u;
        const std::uint32_t magic = kMagic;
        const std::uint64_t t = txn++;

        if (!write_all(fd, &magic, 4) || !write_all(fd, &type, 4) ||
            !write_all(fd, &wflag, 4) || !write_all(fd, &reserved, 4) ||
            !write_all(fd, &t, 8) || !write_all(fd, &addr, 8) ||
            !write_all(fd, &length, 4) || !write_all(fd, &log2_beat, 4) ||
            !write_all(fd, &id, 4) || !write_all(fd, &bytes, 4)) {
            std::printf("[FAIL] 发送请求头失败\n");
            return false;
        }
        if (write && bytes > 0 && !write_all(fd, payload.data(), bytes)) {
            std::printf("[FAIL] 发送写数据失败\n");
            return false;
        }

        std::uint32_t rmagic = 0, rtype = 0, status = 0, rbytes = 0;
        if (!read_all(fd, &rmagic, 4) || !read_all(fd, &rtype, 4) ||
            !read_all(fd, &status, 4) || !read_all(fd, &rbytes, 4)) {
            std::printf("[FAIL] 读取响应头失败\n");
            return false;
        }
        if (rmagic != kMagic || rtype != kResp) {
            std::printf("[FAIL] 响应头非法\n");
            return false;
        }
        // gem5 对非 0 status 会直接 fatal，所以协议上它必须恒为 0。
        if (status != 0) {
            std::printf("[FAIL] 响应 status=%u（gem5 侧会 fatal）\n", status);
            return false;
        }

        if (write) {
            if (rbytes != 0) {
                std::printf("[FAIL] 写响应的 response_bytes=%u，应为 0\n", rbytes);
                return false;
            }
            golden[addr] = payload;
            return true;
        }
        if (rbytes != bytes) {
            std::printf("[FAIL] 读响应长度 %u != 请求的 %u\n", rbytes, bytes);
            return false;
        }
        data_out->resize(rbytes);
        return rbytes == 0 || read_all(fd, data_out->data(), rbytes);
    }

    void teardown() {
        if (fd >= 0) ::close(fd);
        if (listen_fd >= 0) ::close(listen_fd);
    }
};

void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = "/tmp/bridge_mock_gem5.sock";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (std::strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            kBase = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--beat-bytes") == 0 && i + 1 < argc) {
            kBeatBytes = std::strtoul(argv[++i], nullptr, 0);
            if (kBeatBytes == 0 || (kBeatBytes & (kBeatBytes - 1)) != 0 ||
                kBeatBytes > 128) {
                std::fprintf(stderr, "--beat-bytes 必须是 1..128 的 2 次幂\n");
                return 2;
            }
            kBeatLog2 = 0;
            for (unsigned n = kBeatBytes; n > 1; n >>= 1) ++kBeatLog2;
        } else {
            std::fprintf(stderr,
                         "用法: %s [--socket path] [--base addr] [--beat-bytes N]\n",
                         argv[0]);
            return 2;
        }
    }

    std::printf("=== mock gem5：先于真 gem5 验证整条链路 ===\n");
    std::printf("socket=%s\n\n", path.c_str());

    Server srv;
    if (!srv.setup(path)) {
        std::printf("[FAIL] 建立监听失败\n");
        return 1;
    }
    std::printf("等待链路侧连接...\n");
    if (!srv.accept_one()) {
        std::printf("[FAIL] accept 失败\n");
        return 1;
    }
    std::printf("已连接，开始收发\n\n");

    // ---- 1. 整行写 → 整行读回 -----------------------------------------
    std::printf("整行写读\n");
    {
        const std::uint64_t addr = kBase + 0x1000;
        std::vector<std::uint8_t> wd(kBeatBytes);
        for (unsigned i = 0; i < wd.size(); ++i)
            wd[i] = static_cast<std::uint8_t>(0x40 + (i % 64));
        std::vector<std::uint8_t> rd;
        check(srv.transact(true, addr, 1, kBeatLog2, wd, nullptr),
              "写 " + std::to_string(kBeatBytes) + "B");
        check(srv.transact(false, addr, 1, kBeatLog2, {}, &rd),
              "读 " + std::to_string(kBeatBytes) + "B");
        check(rd == wd, "读回数据逐字节一致");
    }

    // ---- 2. 多拍突发 ---------------------------------------------------
    std::printf("\n多拍突发\n");
    {
        const std::uint64_t addr = kBase + 0x2000;
        const unsigned beats = 8;
        std::vector<std::uint8_t> wd(beats * kBeatBytes);
        for (unsigned i = 0; i < wd.size(); ++i)
            wd[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
        std::vector<std::uint8_t> rd;
        check(srv.transact(true, addr, beats, kBeatLog2, wd, nullptr),
              "写 8 拍 x " + std::to_string(kBeatBytes) + "B");
        check(srv.transact(false, addr, beats, kBeatLog2, {}, &rd),
              "读 8 拍 x " + std::to_string(kBeatBytes) + "B");
        check(rd == wd, "多拍突发逐字节一致");
    }

    // ---- 3. 窄访问（AxSIZE=2 → 每拍 4 字节）---------------------------
    std::printf("\n窄访问\n");
    {
        // 地址 4 字节对齐但不在总线拍边界上，逼出 lane 旋转
        const std::uint64_t addr = kBase + 0x3004;
        std::vector<std::uint8_t> wd(4);
        for (unsigned i = 0; i < 4; ++i) wd[i] = static_cast<std::uint8_t>(0xC0 + i);
        std::vector<std::uint8_t> rd;
        check(srv.transact(true, addr, 1, 2, wd, nullptr), "窄写 4B（非 64B 对齐）");
        check(srv.transact(false, addr, 1, 2, {}, &rd), "窄读 4B");
        check(rd == wd, "窄访问 lane 旋转下数据正确");
    }

    // ---- 4. 4KB 边界的最大突发 ----------------------------------------
    std::printf("\n4KB 边界突发\n");
    {
        const std::uint64_t addr = kBase + 0x4000;
        const unsigned beats = 4096 / kBeatBytes;
        std::vector<std::uint8_t> wd(beats * kBeatBytes);
        for (unsigned i = 0; i < wd.size(); ++i)
            wd[i] = static_cast<std::uint8_t>((i * 13 + 5) & 0xFF);
        std::vector<std::uint8_t> rd;
        check(srv.transact(true, addr, beats, kBeatLog2, wd, nullptr),
              "写 " + std::to_string(beats) + " 拍 = 4KB");
        check(srv.transact(false, addr, beats, kBeatLog2, {}, &rd), "读回 4KB");
        check(rd == wd, "4KB 突发逐字节一致");
    }

    // ---- 5. 写后即刻覆写 ----------------------------------------------
    std::printf("\n覆写\n");
    {
        const std::uint64_t addr = kBase + 0x1000;
        std::vector<std::uint8_t> wd(kBeatBytes, 0xEE);
        std::vector<std::uint8_t> rd;
        check(srv.transact(true, addr, 1, kBeatLog2, wd, nullptr), "覆写同一行");
        check(srv.transact(false, addr, 1, kBeatLog2, {}, &rd), "读回覆写后的行");
        check(rd == wd, "覆写后读出的是新值");
    }

    // ---- 6. 关闭连接 = gem5 正常退出 -----------------------------------
    std::printf("\n退出\n");
    ::close(srv.fd);
    srv.fd = -1;
    srv.teardown();

    std::printf("\n%s（参考模型中 %zu 行）\n",
                failures ? "mock gem5 FAILED" : "mock gem5 PASSED", golden.size());
    return failures ? 1 : 0;
}
