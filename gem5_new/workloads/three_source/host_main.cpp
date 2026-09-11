// 三源同时产 trace 的 host 负载。
//
// 编：VORTEX_HOME=... VORTEX_BUILD=... make -C workloads/three_source
// 跑：gem5int/tests/run_three_source.sh
//
// 它是 workloads/shared_buffer/host_main.c 的扩展版：那个只喂 CoralNPU，这个同时
// 喂 CoralNPU 和 Vortex，而且**故意让两个设备的工作在时间上重叠**。
//
// ---- 为什么要重叠，而不是串成一条链 ----
//
// 直觉上更漂亮的形状是 host -> Vortex -> host -> NPU 一条链。但那样两个设备的
// trace 区间必然不相交（后一个要等前一个的结果），而本项目产出的 trace 是喂给下游
// DRAM 模拟器的输入 —— 区间不相交的话，归并出来的流里根本不存在交错，下游连"两个
// 源同时压同一个内存控制器"这件事都无法复现。`hettrace validate` 会把这个形状报成
// WARN，它报得对。
//
// 所以这里的形状是：host 先把两份输入都摆好，然后**先把 Vortex 的活提交下去（异
// 步、不等）**，紧接着启动 NPU，最后才分别收两边的结果。NPU 只跑约 943 个 500MHz
// 周期（≈1.9 us），Vortex 那次 launch 是 ≈190 us，于是 NPU 的整个区间落在 Vortex
// 区间里面。
//
// 代价是"Vortex 的结果流给 NPU"这件事没被演示。那不是遗漏：Vortex 与 NPU 之间
// **不可能**直接共享字节（vortex_bar 在 4 GiB 之上，CoralNPU 的 AXI 只有 32 位），
// 唯一的路是 host 中转，而 host 中转就是上面那条串行链。两者不可兼得，本负载选了
// 对下游更有用的那个。理由写在 docs/03-limitations.md。
//
// ---- 两条腿各自的判据 ----
//
//   Vortex: dst[i] = src0[i] + src1[i]，用的是上游 vecadd 回归测试的 kernel.vxbin
//           （所以本负载不需要 RISC-V 工具链，只需要那个已经编好的 .vxbin）。
//   NPU:    out[i] = in[i]*2+1，且 mailbox 里带 in[] 的校验和 —— 与
//           coralnpuint/ddr_touch.cc 一致。
//
// 数据刻意选成能被 float 精确表示的整数（都 < 2^24），所以两边的核对都是**精确相
// 等**，不留浮点容差 —— 容差会把"设备算错了一点"和"本来就有误差"混在一起。
//
// 退出码（gem5 不管被仿真程序 return 几都是 0 退出，脚本会把它取出来判定）：
//   0 全过   2 NPU 没停下来          3 NPU 的 out[] 不对    4 mailbox tag 不对
//   5 mailbox 校验和不对             6 NPU 起不来           7 Vortex 结果不对
//   8 Vortex 运行时报错

#include <vortex2.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ---- addrmap.json: regions.shared_buffer ----------------------------------
// 与 coralnpuint/ddr_touch.cc 的 kIn/kOut 必须一致。
static constexpr uint32_t kSharedIn  = 0x90000000u;
static constexpr uint32_t kSharedOut = 0x90001000u;
static constexpr uint32_t kNpuWords  = 64;  // ddr_touch.elf 的固定协议长度
static constexpr uint32_t kGpuWords  = 4;   // 覆盖全路径，同时让 timing 回归可快速完成

// ---- addrmap.json: regions.npu_pio + coralnpu_dev.hh 的 Reg 枚举 ----------
static constexpr uint32_t kNpuPio     = 0x30000000u;
static constexpr uint32_t kNpuCtrl    = 0x00u;  // W: bit0 启动
static constexpr uint32_t kNpuStatus  = 0x04u;  // R: bit0 halted, bit1 wfi
static constexpr uint32_t kNpuEntry   = 0x08u;
static constexpr uint32_t kNpuEmitted = 0x0Cu;
static constexpr uint32_t kNpuMbox0   = 0x10u;

static constexpr uint32_t kNpuDoneTag = 0x600Du;  // ddr_touch.cc 里的完成标志

// 见 shared_buffer/host_main.c 里同一个常量：上限是为了"有界地失败"，不要把 gem5
// 挂到 --max-ticks 上，那样报出来的原因看不出是谁卡住的。
static constexpr uint32_t kPollLimit = 2000000u;

// 这些地址能直接当指针用，是因为 het_system.py 在 m5.instantiate() 之后调
// Process::map(pa, pa, size, cacheable=False) 把它们按 VA==PA 映射进本进程。
static volatile uint32_t* const in_buf =
    reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(kSharedIn));
static volatile uint32_t* const out_buf =
    reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(kSharedOut));

static volatile uint32_t* npu_reg(uint32_t off) {
    return reinterpret_cast<volatile uint32_t*>(
        static_cast<uintptr_t>(kNpuPio + off));
}

// 与 shared_buffer/host_main.c 的 pattern() 一致。不用纯递增：全 0 / 全 1 的话，
// "内存后端没接通、设备读到的全是零"这种失败算出来的校验和可能碰巧对上。
static uint32_t pattern(uint32_t i) {
    return 0x1000u * (i + 1u) + (i ^ 0x5Au);
}

// vecadd 的 kernel_arg_t，与 $VORTEX_HOME/tests/regression/vecadd/common.h 逐字一
// 致。抄一份而不是 include：那个头在 Vortex 树里、路径随版本变，而本结构体是 ABI
// 的一部分 —— 抄错的后果是内核读到垃圾指针，比一个找不到的头文件难查得多，所以宁
// 可让它显式地摆在这里。
struct kernel_arg_t {
    uint32_t num_points;
    uint64_t src0_addr;
    uint64_t src1_addr;
    uint64_t dst_addr;
};

#define VXCHECK(expr)                                                    \
    do {                                                                 \
        vx_result_t _r = (expr);                                         \
        if (_r != VX_SUCCESS) {                                          \
            std::printf("host: Vortex 运行时报错 %s:%d: '%s' -> %s\n",   \
                        __FILE__, __LINE__, #expr, vx_result_string(_r)); \
            return 8;                                                    \
        }                                                                \
    } while (0)

int main(int argc, char** argv) {
    const char* kernel_file = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            kernel_file = argv[++i];
        } else {
            std::printf("host: 用法: host_main -k <kernel.vxbin>\n");
            return 1;
        }
    }
    if (kernel_file == nullptr) {
        std::printf("host: 缺 -k <kernel.vxbin>\n");
        return 1;
    }

    // ---- 1. 把两份输入都摆好 ------------------------------------------------
    // NPU 那一份直接写共享区。这 64 次写是 uncacheable 的，每一次都以独立的包穿过
    // LLC 之下的探针，于是 host trace 里能看到 64 条落在 shared_buffer 的写。
    uint32_t npu_expect_sum = 0;
    for (uint32_t i = 0; i < kNpuWords; ++i) {
        in_buf[i] = pattern(i);
        npu_expect_sum += pattern(i);
    }
    // out[] 清零：这样"NPU 一个字都没写"和"NPU 写错了"在 out[] 上是可区分的。
    for (uint32_t i = 0; i < kNpuWords; ++i) {
        out_buf[i] = 0u;
    }

    std::vector<float> h_src0(kGpuWords), h_src1(kGpuWords),
                       h_dst(kGpuWords, 0.0f);
    for (uint32_t i = 0; i < kGpuWords; ++i) {
        h_src0[i] = static_cast<float>(pattern(i));
        h_src1[i] = static_cast<float>(i + 1);
    }
    std::printf("host: 输入就位 —— NPU 的 in[] 在 0x%08x，Vortex 的两份在 host 堆上\n",
                kSharedIn);

    // ---- 2. Vortex：建设备/队列/缓冲，装内核 -------------------------------
    vx_device_h dev = nullptr;
    VXCHECK(vx_device_open(0, &dev));

    // 注意：SE 模式下必须给 gem5 至少 2 个 CPU。host runtime 的每个队列有一个
    // worker 线程，只有 1 个 CPU 时 vx_queue_create 直接返回 VX_ERR_INTERNAL。
    vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
    vx_queue_h q = nullptr;
    VXCHECK(vx_queue_create(dev, &qi, &q));

    const uint64_t buf_size = kGpuWords * sizeof(float);
    vx_buffer_h src0_buf = nullptr, src1_buf = nullptr, dst_buf = nullptr;
    VXCHECK(vx_buffer_create(dev, buf_size, VX_MEM_READ,  &src0_buf));
    VXCHECK(vx_buffer_create(dev, buf_size, VX_MEM_READ,  &src1_buf));
    VXCHECK(vx_buffer_create(dev, buf_size, VX_MEM_WRITE, &dst_buf));

    vx_module_h mod  = nullptr;
    vx_kernel_h kern = nullptr;
    VXCHECK(vx_module_load_file(dev, kernel_file, &mod));
    VXCHECK(vx_module_get_kernel(mod, "main", &kern));

    kernel_arg_t karg{};
    karg.num_points = kGpuWords;
    VXCHECK(vx_buffer_address(src0_buf, &karg.src0_addr));
    VXCHECK(vx_buffer_address(src1_buf, &karg.src1_addr));
    VXCHECK(vx_buffer_address(dst_buf,  &karg.dst_addr));

    // ---- 3. 把 Vortex 的活异步提交下去，**先不等** -------------------------
    // 上传是 fire-and-forget，读回挂在 launch 事件上，最后只等一次。这一段就是
    // "两个设备的区间重叠"得以成立的地方：提交完立刻去启动 NPU。
    VXCHECK(vx_enqueue_write(q, src0_buf, 0, h_src0.data(), buf_size, 0, nullptr, nullptr));
    VXCHECK(vx_enqueue_write(q, src1_buf, 0, h_src1.data(), buf_size, 0, nullptr, nullptr));

    uint32_t grid[1], block[1];
    const uint32_t nd_size = kGpuWords;
    VXCHECK(vx_device_max_occupancy_grid(dev, 1, &nd_size, grid, block));

    vx_launch_info_t li{};
    li.struct_size  = sizeof(li);
    li.kernel       = kern;
    li.args_host    = &karg;
    li.args_size    = sizeof(karg);
    li.ndim         = 1;
    li.grid_dim[0]  = grid[0];
    li.block_dim[0] = block[0];

    vx_event_h launch_ev = nullptr, read_ev = nullptr;
    VXCHECK(vx_enqueue_launch(q, &li, 0, nullptr, &launch_ev));
    VXCHECK(vx_enqueue_read(q, h_dst.data(), dst_buf, 0, buf_size,
                            1, &launch_ev, &read_ev));
    std::printf("host: Vortex 的活已提交（grid=%u block=%u），不等，先起 NPU\n",
                grid[0], block[0]);

    // ---- 4. 启动 NPU -------------------------------------------------------
    std::printf("host: NPU entry PC = 0x%08x\n", *npu_reg(kNpuEntry));
    *npu_reg(kNpuCtrl) = 1u;
    if ((*npu_reg(kNpuCtrl) & 1u) == 0u) {
        std::printf("host: 错误 —— 写了 CTRL 但 started 位没起（没给 --npu-kernel？）\n");
        return 6;
    }

    // ---- 5. 收 NPU 的结果 --------------------------------------------------
    // 先收 NPU：它快得多（1.9us vs 190us）。反过来先等 Vortex 的话，NPU 早就跑完
    // 了，两个区间照样重叠 —— 顺序只影响 host 自己在哪儿转圈，不影响设备。
    uint32_t status = 0, spins = 0;
    while (spins < kPollLimit) {
        status = *npu_reg(kNpuStatus);
        if ((status & 0x3u) != 0u) break;
        ++spins;
    }
    if ((status & 0x3u) == 0u) {
        std::printf("host: 错误 —— 轮询 %u 次 NPU 仍未停 (status=0x%x)\n", spins, status);
        return 2;
    }
    std::printf("host: NPU 停了, status=0x%x, 轮询 %u 次, 设备内诊断 tap 已写 %u 条\n",
                status, spins, *npu_reg(kNpuEmitted));

    // ---- 6. 收 Vortex 的结果 ----------------------------------------------
    VXCHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
    std::printf("host: Vortex 跑完并读回\n");

    // ---- 7. 两边分别核对 ---------------------------------------------------
    int bad = 0;
    for (uint32_t i = 0; i < kNpuWords; ++i) {
        const uint32_t want = pattern(i) * 2u + 1u;
        const uint32_t got  = out_buf[i];
        if (got != want) {
            if (bad < 4) {
                std::printf("host: NPU out[%u] = 0x%08x, 期望 0x%08x\n", i, got, want);
            }
            ++bad;
        }
    }
    if (bad) {
        std::printf("host: 错误 —— NPU 的 out[] 有 %d/%u 个字不对\n",
                    bad, kNpuWords);
        return 3;
    }

    // mailbox 走 PIO 读，不走共享内存：它是设备库内部的寄存器，NPU 对
    // 0xc0000000 的 AXI 访问被 RTL 收进那 4 个寄存器，根本没出核。
    const uint32_t mbox = *npu_reg(kNpuMbox0);
    if ((mbox >> 16) != kNpuDoneTag) {
        std::printf("host: 错误 —— mailbox tag = 0x%04x, 期望 0x%04x\n",
                    mbox >> 16, kNpuDoneTag);
        return 4;
    }
    // 这一项才是"共享内存真的共享了数据"的判据：校验和是 NPU 自己把 in[] 读出来累
    // 加的。后端没接通时它读到零，和就是 0。
    if ((mbox & 0xffffu) != (npu_expect_sum & 0xffffu)) {
        std::printf("host: 错误 —— NPU 算的校验和 0x%04x, host 期望 0x%04x\n",
                    mbox & 0xffffu, npu_expect_sum & 0xffffu);
        return 5;
    }
    std::printf("host: NPU ok (tag=0x%04x sum=0x%04x)\n", mbox >> 16, mbox & 0xffffu);

    // Vortex：精确相等，不留容差。输入都 < 2^24，float 能精确表示，和也一样。
    bad = 0;
    for (uint32_t i = 0; i < kGpuWords; ++i) {
        const uint32_t want = pattern(i) + i + 1u;
        const uint32_t got  = static_cast<uint32_t>(h_dst[i]);
        if (got != want || h_dst[i] != static_cast<float>(want)) {
            if (bad < 4) {
                std::printf("host: Vortex dst[%u] = %.1f, 期望 %u\n", i, h_dst[i], want);
            }
            ++bad;
        }
    }
    if (bad) {
        std::printf("host: 错误 —— Vortex 的 dst[] 有 %d/%u 个字不对\n",
                    bad, kGpuWords);
        return 7;
    }
    std::printf("host: Vortex ok (dst[0]=%.1f dst[%u]=%.1f)\n",
                h_dst[0], kGpuWords - 1, h_dst[kGpuWords - 1]);

    // 收尾。vortex2 的对象是引用计数的，所以是 *_release 而不是 *_destroy。返回值
    // 刻意不查：上面两条腿已经核对完了，这里再返回个错误码只会把"算错了"和"没关干
    // 净"混在一起。
    vx_event_release(launch_ev);
    vx_event_release(read_ev);
    vx_buffer_release(src0_buf);
    vx_buffer_release(src1_buf);
    vx_buffer_release(dst_buf);
    vx_kernel_release(kern);
    vx_module_release(mod);
    vx_queue_release(q);
    vx_device_release(dev);

    std::printf("host: 全部通过 —— 两个设备各自与 host 共享了字节\n");
    return 0;
}
