// P1：Bridge 单元测试。
//
// 只例化 MemSimBackend，直接往它的 sc_fifo 灌 SimpleMemRequest。不接 AouTarget、
// 不接 UcieLink、不接 gem5——这样一旦出错，范围就锁在这一个模块里。
//
// 覆盖的都是"换成 mem_sim 之后才会暴露"的语义：窄访问的 lane 旋转、三类
// 预校验错误（越界/lock/非法 strobe）、部分字节写、零初始化读、以及同 ID
// 多笔的响应顺序。
#include "mem_sim_backend.h"

#include "axi_if.h"
#include "axi_contract.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <systemc.h>
#include <vector>

namespace {

constexpr std::uint64_t kBase = 0x1234567800000000ULL;
constexpr std::size_t kSize = 1u << 20;  // 1 MiB

int failures = 0, checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

SC_MODULE(BackendTb) {
    sc_clock clk{"clk", 2, SC_NS};
    sc_signal<bool> rst_n{"rst_n"};

    sc_fifo<SimpleMemRequest> req_fifo{16};
    sc_fifo<SimpleMemResponse> rsp_fifo{16};

    MemSimBackend mem{"mem", kBase, kSize};

    bool done = false;
    std::vector<std::string> notes;

    SC_HAS_PROCESS(BackendTb);
    explicit BackendTb(sc_module_name n) : sc_module(n) {
        mem.clk(clk);
        mem.rst_n(rst_n);
        mem.request(req_fifo);
        mem.response(rsp_fifo);
        SC_THREAD(stimulus);
    }

    // ---- 构造请求的工具 ------------------------------------------------

    // 一整笔突发。gen(i, lane) 返回第 i 拍、第 lane 通道上的数据字节。
    SimpleMemRequest make_write(std::uint64_t addr, unsigned beats, std::uint8_t size_code,
                                std::uint16_t id, std::uint16_t user,
                                const std::function<std::uint8_t(unsigned, unsigned)>& gen,
                                bool full_strobe = true) {
        SimpleMemRequest r;
        r.write = true;
        r.rp = 0;
        r.address.addr = addr;
        r.address.id = id;
        r.address.user = user;
        r.address.len = static_cast<std::uint8_t>(beats - 1);
        r.address.size = size_code;
        r.address.burst = 1;
        const unsigned width = 1u << size_code;
        r.write_beats.resize(beats);
        for (unsigned n = 0; n < beats; ++n) {
            const unsigned lane = static_cast<unsigned>(
                (addr + std::uint64_t(n) * width) % AXI_DATA_BYTES);
            for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
                r.write_beats[n].data[j] = gen(n, j);
                const bool in_lane = (j >= lane && j < lane + width);
                r.write_beats[n].strobe[j] = (in_lane && full_strobe) ? 0xFF : 0x00;
            }
            r.write_beats[n].user = user;
        }
        return r;
    }

    SimpleMemRequest make_read(std::uint64_t addr, unsigned beats,
                               std::uint8_t size_code, std::uint16_t id,
                               std::uint16_t user) {
        SimpleMemRequest r;
        r.write = false;
        r.rp = 0;
        r.address.addr = addr;
        r.address.id = id;
        r.address.user = user;
        r.address.len = static_cast<std::uint8_t>(beats - 1);
        r.address.size = size_code;
        r.address.burst = 1;
        return r;
    }

    SimpleMemResponse run(SimpleMemRequest r) {
        req_fifo.write(r);
        return rsp_fifo.read();
    }

    // 取出第 n 拍的有效通道区间内的字节。
    std::vector<std::uint8_t> lane_bytes(const SimpleMemResponse& r,
                                         std::uint64_t addr, unsigned n,
                                         unsigned width) {
        const unsigned lane =
            static_cast<unsigned>((addr + std::uint64_t(n) * width) % AXI_DATA_BYTES);
        std::vector<std::uint8_t> out(width);
        for (unsigned j = 0; j < width; ++j) {
            out[j] = r.read_beats[n].data[lane + j];
        }
        return out;
    }

    // ---- 用例 ----------------------------------------------------------

    void test_full_line() {
        std::printf("\n整行读写\n");
        const std::uint64_t addr = kBase + 0x1000;
        auto wr = make_write(addr, 1, 6, 1, 0x11,
                             [](unsigned, unsigned lane) {
                                 return static_cast<std::uint8_t>(0x40 + lane);
                             });
        auto rsp = run(wr);
        check(rsp.write && rsp.resp == 0, "整行写返回 OK");
        check(rsp.read_beats.empty(), "写响应不带读数据");

        auto rd = run(make_read(addr, 1, 6, 1, 0x22));
        check(rd.resp == 0, "整行读返回 OK");
        bool ok = rd.read_beats.size() == 1;
        for (unsigned j = 0; ok && j < AXI_DATA_BYTES; ++j) {
            ok = rd.read_beats[0].data[j] == static_cast<std::uint8_t>(0x40 + j);
        }
        check(ok, "整行读回数据逐字节一致");
    }

    void test_narrow_lane_rotation() {
        std::printf("\n窄访问与 lane 旋转\n");
        // size_code=2 → 每拍 4 字节；地址 +4 → 起始 lane 为 4。
        const std::uint64_t addr = kBase + 0x2004;
        const unsigned width = 4;
        auto wr = make_write(addr, 1, 2, 2, 0x33,
                             [](unsigned, unsigned) { return std::uint8_t{0}; });
        // 显式铺上可辨认的模式，只落在 lane 4..7 上
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            wr.write_beats[0].data[j] = static_cast<std::uint8_t>(0xA0 + j);
        }
        auto rsp = run(wr);
        check(rsp.resp == 0, "窄写返回 OK");

        auto rd = run(make_read(addr, 1, 2, 2, 0x44));
        check(rd.read_beats.size() == 1, "窄读返回一拍");
        auto got = lane_bytes(rd, addr, 0, width);
        bool ok = true;
        for (unsigned j = 0; j < width; ++j) {
            ok = ok && got[j] == static_cast<std::uint8_t>(0xA4 + j);
        }
        check(ok, "窄读在正确的 lane 上取回数据（lane 旋转正确）");

        // 同一行内、窄访问之外的通道必须保持零（未被写过）
        bool others_zero = true;
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            if (j >= 4 && j < 8) continue;
            if (rd.read_beats[0].data[j] != 0) others_zero = false;
        }
        check(others_zero, "有效通道之外的 lane 保持零");
    }

    void test_out_of_range() {
        std::printf("\n预校验错误\n");
        auto rsp = run(make_read(kBase + kSize + 0x1000, 1, 6, 3, 0));
        check(rsp.resp == 3, "越界读返回 DECERR(3)");

        auto rsp2 = run(make_read(kBase - 0x40, 1, 6, 3, 0));
        check(rsp2.resp == 3, "低于窗口基址的访问返回 DECERR(3)");
        check(rsp2.read_beats.size() == 1,
              "错误响应仍给出 len+1 拍（AouTarget 会硬校验）");

        auto lock = make_read(kBase + 0x3000, 1, 6, 4, 0);
        lock.address.lock = 1;
        check(run(lock).resp == 2, "独占访问返回 SLVERR(2)");

        // strobe 落在有效通道之外
        auto bad = make_write(kBase + 0x4000, 1, 2, 5, 0,
                              [](unsigned, unsigned) { return std::uint8_t{0x5A}; },
                              /*full_strobe=*/false);
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            bad.write_beats[0].strobe[j] = 0xFF;  // 全通道点亮，越出 4 字节窗口
        }
        check(run(bad).resp == 2, "非法 strobe 返回 SLVERR(2)");
    }

    void test_uninitialized_and_partial() {
        std::printf("\n零初始化与部分字节写\n");
        const std::uint64_t addr = kBase + 0x5000;
        auto rd = run(make_read(addr, 1, 6, 6, 0));
        check(rd.resp == 0, "读未写过的地址返回 OK（UninitializedData 映射为 OK）");
        bool zero = true;
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            zero = zero && rd.read_beats[0].data[j] == 0;
        }
        check(zero, "零初始化读返回全 0");

        // 先整行写 0x11，再用半掩码写 0x22，只有被选中的字节应改变
        auto w1 = make_write(addr, 1, 6, 7, 0,
                             [](unsigned, unsigned) { return std::uint8_t{0x11}; });
        check(run(w1).resp == 0, "整行写 0x11 完成");

        auto w2 = make_write(addr, 1, 6, 8, 0,
                             [](unsigned, unsigned) { return std::uint8_t{0x22}; });
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            w2.write_beats[0].strobe[j] = (j % 2 == 0) ? 0xFF : 0x00;
        }
        check(run(w2).resp == 0, "半掩码写 0x22 完成");

        auto rd2 = run(make_read(addr, 1, 6, 9, 0));
        bool ok = true;
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            const std::uint8_t want = (j % 2 == 0) ? 0x22 : 0x11;
            ok = ok && rd2.read_beats[0].data[j] == want;
        }
        check(ok, "掩码为 0 的字节保持原值（byte_mask 语义正确）");
    }

    void test_multi_beat() {
        std::printf("\n多拍突发\n");
        const std::uint64_t addr = kBase + 0x8000;
        const unsigned beats = 4;
        auto wr = make_write(addr, beats, 6, 10, 0,
                             [](unsigned n, unsigned lane) {
                                 return static_cast<std::uint8_t>(n * 16 + (lane & 0xF));
                             });
        check(run(wr).resp == 0, "4 拍突发写完成");

        auto rd = run(make_read(addr, beats, 6, 10, 0));
        check(rd.read_beats.size() == beats, "4 拍突发读返回 4 拍");
        bool ok = true;
        for (unsigned n = 0; n < beats; ++n) {
            for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
                const std::uint8_t want = static_cast<std::uint8_t>(n * 16 + (j & 0xF));
                ok = ok && rd.read_beats[n].data[j] == want;
            }
        }
        check(ok, "多拍突发逐字节一致");
    }

    void stimulus() {
        rst_n.write(false);
        for (int i = 0; i < 10; ++i) wait(clk.posedge_event());
        rst_n.write(true);
        for (int i = 0; i < 5; ++i) wait(clk.posedge_event());

        test_full_line();
        test_narrow_lane_rotation();
        test_out_of_range();
        test_uninitialized_and_partial();
        test_multi_beat();

        std::printf("\n计数器\n");
        check(mem.completed > 0, "completed 计数在推进");
        check(mem.error_responses == 4,
              "error_responses 恰好等于合成的错误响应数（4 笔）");
        std::printf("  completed=%llu error_responses=%llu submitted=%llu retried=%llu\n",
                    static_cast<unsigned long long>(mem.completed),
                    static_cast<unsigned long long>(mem.error_responses),
                    static_cast<unsigned long long>(mem.submitted_txns),
                    static_cast<unsigned long long>(mem.retried_submits));
        std::printf("  拒绝：越界=%llu lock=%llu strobe=%llu 契约=%llu\n",
                    static_cast<unsigned long long>(mem.rejected_out_of_range),
                    static_cast<unsigned long long>(mem.rejected_lock),
                    static_cast<unsigned long long>(mem.rejected_strobe),
                    static_cast<unsigned long long>(mem.rejected_contract));

        done = true;
        sc_stop();
    }
};

}  // namespace

int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_FS);
    std::printf("=== P1：Bridge（MemSimBackend）单元测试 ===\n");
    std::printf("AXI_DATA_WIDTH=%d  AXI_DATA_BYTES=%d  base=0x%llx size=%zu\n",
                AXI_DATA_WIDTH, AXI_DATA_BYTES,
                static_cast<unsigned long long>(kBase), kSize);

    BackendTb tb{"tb"};
    sc_start(50, SC_MS);

    if (!tb.done) {
        std::printf("\n[FAIL] 全局看门狗超时——仿真未在时限内结束\n");
        return 1;
    }
    std::printf("\n%s（%d 项检查，%d 项失败）\n",
                failures ? "P1 FAILED" : "P1 PASSED", checks, failures);
    return failures ? 1 : 0;
}
