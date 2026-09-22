// P0b：堆叠存储后端的自证测试。
//
// 不接 SystemC、不接 UCIe、不接 gem5——只有 MemBackend 和一个"参考字节数组"。
// 这个阶段的价值在于：后面 Bridge 若出现数据错乱，能立刻分清是后端映射错了
// 还是链路层错了。最典型的一类缺陷（Request::decoded 没有填导致所有 cache
// line 塌缩到同一个 bank/row/column）只有在这里才容易被抓出来。
#include "mem_backend.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

// 参考模型：一个纯字节数组，按地址索引。所有内存操作都在它上面镜像一遍，
// 之后逐字节对拍。
struct Golden {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> written;

    explicit Golden(std::size_t n) : bytes(n, 0), written(n, false) {}

    void write(std::uint64_t addr, const std::vector<std::uint8_t>& data,
               const std::vector<std::uint8_t>* mask) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            const bool enabled = (mask == nullptr) || ((*mask)[i] != 0);
            if (!enabled) continue;
            bytes[addr + i] = data[i];
            written[addr + i] = true;
        }
    }

    bool matches(std::uint64_t addr, const std::vector<std::uint8_t>& got) const {
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (bytes[addr + i] != got[i]) return false;
        }
        return true;
    }
};

// 提交一笔事务并在反压下重试，直到拿到它的响应。
bool run_one(bridge::MemBackend& be, bridge::MemTxn txn, bridge::MemResp& out) {
    for (int guard = 0; guard < 2000000; ++guard) {
        const int rc = be.submit(txn);
        if (rc < 0) {
            std::printf("  submit 失败: %s\n", be.last_error().c_str());
            return false;
        }
        if (rc == 0) {
            for (int spin = 0; spin < 2000000; ++spin) {
                be.step(1);
                bridge::MemResp r;
                while (be.poll(r)) {
                    if (r.host_request_id == txn.host_request_id) {
                        out = std::move(r);
                        return true;
                    }
                }
            }
            return false;
        }
        be.step(1);  // rc == 1：反压，推进一拍后原样重试
    }
    return false;
}

std::uint64_t rnd_offset(std::mt19937_64& rng, std::uint64_t span,
                         std::uint32_t chunk) {
    const std::uint64_t slots = span / chunk;
    return (rng() % slots) * chunk;
}

}  // namespace

int main(int argc, char** argv) {
    int ops = argc > 1 ? std::atoi(argv[1]) : 2000;

    std::printf("=== P0b：堆叠存储后端自证 ===\n");

    bridge::MemBackend::Options opt;
    opt.standard = "hbm4";
    opt.channels = 1;
    opt.transaction_bytes = 64;
    opt.window_bytes = 1u << 20;  // 1 MiB 工作窗口

    bridge::MemBackend backend{opt};
    const std::uint32_t chunk = backend.transaction_bytes();

    std::printf("标准=%s  窗口=%llu B  transaction=%u B  tick=%llu ps\n\n",
                opt.standard.c_str(),
                static_cast<unsigned long long>(backend.window_bytes()), chunk,
                static_cast<unsigned long long>(backend.tick_ps()));

    check(chunk == 64, "transaction_bytes 为 64");
    check(backend.tick_ps() == 250, "HBM4 单 tick 为 250 ps（tCK 500 / multiplier 2）");

    // 只在低 64 KiB 内活动，参考数组与之等大。
    const std::uint64_t region = 64u << 10;
    Golden golden{region};

    // ---- 1. 定向用例：两个相隔很远的地址必须互不干扰 -------------------
    // 这是 decoded 塌缩缺陷最直接的探测器：若 decoded 全 0，两个地址会落到
    // 同一个 (bank,row,column)，后写的会覆盖先写的。
    std::printf("定向用例\n");
    {
        const std::uint64_t addr_a = 0x0000;
        const std::uint64_t addr_b = 0x8000;  // 相隔 32 KiB
        std::vector<std::uint8_t> pat_a(chunk, 0xA5), pat_b(chunk, 0x5A);

        bridge::MemResp r;
        bridge::MemTxn wa; wa.write = true; wa.address = addr_a;
        wa.bytes = chunk; wa.payload = pat_a; wa.host_request_id = 1;
        check(run_one(backend, wa, r), "写入地址 A 完成");
        golden.write(addr_a, pat_a, nullptr);

        bridge::MemTxn wb; wb.write = true; wb.address = addr_b;
        wb.bytes = chunk; wb.payload = pat_b; wb.host_request_id = 2;
        check(run_one(backend, wb, r), "写入地址 B 完成");
        golden.write(addr_b, pat_b, nullptr);

        bridge::MemTxn ra; ra.write = false; ra.address = addr_a;
        ra.bytes = chunk; ra.host_request_id = 3;
        check(run_one(backend, ra, r), "读回地址 A 完成");
        check(r.data == pat_a, "写 A 写 B 后读 A 仍为 A 的模式（decoded 未塌缩）");

        bridge::MemTxn rb; rb.write = false; rb.address = addr_b;
        rb.bytes = chunk; rb.host_request_id = 4;
        check(run_one(backend, rb, r), "读回地址 B 完成");
        check(r.data == pat_b, "写 A 写 B 后读 B 仍为 B 的模式");

        // 相邻行也必须各自独立
        bridge::MemTxn ra2; ra2.write = false; ra2.address = addr_a + chunk;
        ra2.bytes = chunk; ra2.host_request_id = 5;
        check(run_one(backend, ra2, r), "读相邻行完成");
        check(r.status == static_cast<int>(2),
              "从未写过的地址返回 UninitializedData");
        check(std::all_of(r.data.begin(), r.data.end(),
                          [](std::uint8_t b) { return b == 0; }),
              "未初始化读返回全 0");
    }

    // ---- 2. 随机读写对拍 ------------------------------------------------
    std::printf("\n随机对拍（%d 次操作）\n", ops);
    std::mt19937_64 rng{20260915};
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> mask_dist(0, 99);
    std::uint64_t next_id = 100;
    std::uint64_t mismatches = 0;

    for (int i = 0; i < ops; ++i) {
        const std::uint64_t addr = rnd_offset(rng, region, chunk);
        const bool is_write = op_dist(rng) < 50;
        bridge::MemTxn txn;
        txn.address = addr;
        txn.bytes = chunk;
        txn.write = is_write;
        txn.host_request_id = next_id++;
        bridge::MemResp r;

        if (is_write) {
            txn.payload.resize(chunk);
            for (auto& b : txn.payload) b = static_cast<std::uint8_t>(rng());
            // 三成概率做部分字节写，逼出 byte_mask 路径
            if (mask_dist(rng) < 30) {
                txn.byte_mask.resize(chunk);
                for (auto& b : txn.byte_mask) b = (rng() & 1) ? 0xFF : 0x00;
                txn.has_byte_mask = true;
            }
            if (!run_one(backend, txn, r)) { ++mismatches; continue; }
            golden.write(addr, txn.payload,
                         txn.has_byte_mask ? &txn.byte_mask : nullptr);
            if (r.status != 0 && r.status != 1) ++mismatches;
        } else {
            if (!run_one(backend, txn, r)) { ++mismatches; continue; }
            if (!golden.matches(addr, r.data)) {
                ++mismatches;
                if (mismatches <= 3) {
                    std::printf("  [FAIL] 地址 0x%llx 读回数据与参考不一致\n",
                                static_cast<unsigned long long>(addr));
                }
            }
        }
    }

    check(mismatches == 0, "随机读写逐字节比对全部一致");

    // ---- 3. 收尾 --------------------------------------------------------
    std::printf("\n收尾\n");
    for (int i = 0; i < 200000 && !backend.quiescent(); ++i) backend.step(1);
    check(backend.quiescent(), "所有事务完成后进入 quiescent");
    backend.finish(0);
    check(backend.last_error().empty(),
          std::string("后端无错误（last_error=\"") + backend.last_error() + "\"）");

    std::printf("\n%s（%d 项检查，%d 项失败，%llu 次数据不一致）\n",
                failures ? "P0b FAILED" : "P0b PASSED", checks, failures,
                static_cast<unsigned long long>(mismatches));
    return failures ? 1 : 0;
}
