// 异构协同负载的 host 那一段。
//
// 编：make -C workloads/shared_buffer
// 跑：gem5 的 configs/het/het_system.py --cmd .../host_main --options "--npu"
//
// 它做的事只有一件：按 addrmap.json 的地址把数据交给加速器，再把结果收回来核对。
//
//   in  [0x90000000, +256B)  host 写 -> 加速器读
//   out [0x90001000, +256B)  加速器写 -> host 读
//
// 为什么这些地址能直接当指针用：het_system.py 在 m5.instantiate() 之后调
// Process::map(pa, pa, size, cacheable=False)，把设备与共享区间按 VA==PA 映射进本
// 进程。所以下面所有 volatile 指针都是**物理地址**，与 addrmap.json 逐字一致。
// uncacheable 也是那边定的，本程序不需要（也无法）做任何 cache 维护。
//
// 退出码是有讲究的 —— gem5 不管被仿真程序 return 几都是 0 退出，het_system.py 会
// 把它取出来判定。每种失败给一个不同的码，光看码就知道断在哪：
//   0 全过  2 NPU 没停下来  3 out[] 内容不对  4 mailbox tag 不对  5 校验和不对
//   6 NPU 起不来（写了 CTRL 但 started 位没起）

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- addrmap.json: regions.shared_buffer ----------------------------------
// 与 coralnpuint/ddr_touch.cc 里的 kIn/kOut 必须一致；那边是 NPU 侧的同一段。
#define SHARED_IN   0x90000000u
#define SHARED_OUT  0x90001000u
#define WORDS       64

// ---- addrmap.json: regions.npu_pio + CoralNPU.py 的寄存器表 ---------------
// 这不是 NPU 的 AXI slave 窗口 (npu_slave)，是 gem5 CoralNPU SimObject 自己的
// 寄存器窗口。偏移取自 gem5int/src/dev/coralnpu/coralnpu_dev.hh 的 Reg 枚举。
#define NPU_PIO     0x30000000u
#define NPU_CTRL    0x00u  // W: bit0 启动
#define NPU_STATUS  0x04u  // R: bit0 halted, bit1 wfi, bit2 还在 tick
#define NPU_ENTRY   0x08u  // R: load_elf 报的入口 PC
#define NPU_EMITTED 0x0Cu  // R: 可选设备内诊断 tap 已写出的记录数
#define NPU_MBOX0   0x10u  // R/W: mailbox[0..3]

// ddr_touch.cc 里的完成标志高 16 位。
#define NPU_DONE_TAG 0x600Du

// NPU 跑 ddr_touch 只要约 943 个 500MHz 周期（≈1.9us，host 2GHz 下约 3800 周期）。
// 上限给到百万级纯粹是兜底：真出问题时要的是"有界地失败"，而不是把 gem5 挂到
// --max-ticks 上限 —— 那样报出来的原因是"到达 tick 上限"，看不出是谁卡住了。
#define POLL_LIMIT 2000000u

static volatile uint32_t *const in_buf  = (volatile uint32_t *)(uintptr_t)SHARED_IN;
static volatile uint32_t *const out_buf = (volatile uint32_t *)(uintptr_t)SHARED_OUT;

static volatile uint32_t *npu_reg(uint32_t off)
{
    return (volatile uint32_t *)(uintptr_t)(NPU_PIO + off);
}

// host 侧算一遍期望值。in[i] 故意不是纯递增：全 0 或全 1 的话，"内存后端没接通、
// NPU 读到的全是零"这种失败模式算出来的校验和可能碰巧对上。
static uint32_t pattern(uint32_t i)
{
    return 0x1000u * (i + 1u) + (i ^ 0x5Au);
}

static int run_npu(void)
{
    uint32_t expect_sum = 0;
    for (uint32_t i = 0; i < WORDS; ++i) {
        expect_sum += pattern(i);
    }

    printf("host: entry PC = 0x%08x\n", *npu_reg(NPU_ENTRY));

    // 启动。写之前 in[] 已经填好 —— 顺序很重要，反过来的话 NPU 可能读到旧值，
    // 而且那种竞态在 trace 上要靠时间戳才看得出来。
    *npu_reg(NPU_CTRL) = 1u;
    if ((*npu_reg(NPU_CTRL) & 1u) == 0u) {
        printf("host: 错误 —— 写了 CTRL 但 started 位没起（没给 --npu-kernel？）\n");
        return 6;
    }

    uint32_t status = 0;
    uint32_t spins  = 0;
    // 轮询 halted|wfi。ddr_touch 结尾是 wfi，所以正常路径上看到的是 bit1。
    while (spins < POLL_LIMIT) {
        status = *npu_reg(NPU_STATUS);
        if ((status & 0x3u) != 0u) {
            break;
        }
        ++spins;
    }
    if ((status & 0x3u) == 0u) {
        printf("host: 错误 —— 轮询 %u 次 NPU 仍未停 (status=0x%x)\n",
               spins, status);
        return 2;
    }
    printf("host: NPU 停了, status=0x%x, 轮询 %u 次, 设备内诊断 tap 已写 %u 条\n",
           status, spins, *npu_reg(NPU_EMITTED));

    // 结果核对。out[i] = in[i]*2+1 是 ddr_touch.cc 里的算法。
    int bad = 0;
    for (uint32_t i = 0; i < WORDS; ++i) {
        const uint32_t want = pattern(i) * 2u + 1u;
        const uint32_t got  = out_buf[i];
        if (got != want) {
            if (bad < 4) {
                printf("host: out[%u] = 0x%08x, 期望 0x%08x\n", i, got, want);
            }
            ++bad;
        }
    }
    if (bad) {
        printf("host: 错误 —— out[] 有 %d/%u 个字不对\n", bad, WORDS);
        return 3;
    }

    // mailbox 走 PIO 读，不走共享内存：mailbox 是设备库内部的寄存器，NPU 对
    // 0xc0000000 的 AXI 访问被 RTL 收进那 4 个寄存器，根本没出核。
    const uint32_t mbox = *npu_reg(NPU_MBOX0);
    if ((mbox >> 16) != NPU_DONE_TAG) {
        printf("host: 错误 —— mailbox tag = 0x%04x, 期望 0x%04x\n",
               mbox >> 16, NPU_DONE_TAG);
        return 4;
    }
    // 这一项是"共享内存真的共享了数据"的判据：校验和是 NPU 自己把 in[] 读出来
    // 累加的。后端没接通时它读到的是零，和就是 0，而 out[] 也会全是 1 —— 上面
    // 那关已经拦住了，这里是第二道，防的是"读对了但写错了"的对称情况。
    if ((mbox & 0xffffu) != (expect_sum & 0xffffu)) {
        printf("host: 错误 —— NPU 算的校验和 0x%04x, host 期望 0x%04x\n",
               mbox & 0xffffu, expect_sum & 0xffffu);
        return 5;
    }
    printf("host: mailbox ok (tag=0x%04x sum=0x%04x)\n",
           mbox >> 16, mbox & 0xffffu);
    return 0;
}

int main(int argc, char **argv)
{
    int want_npu = 0;
    for (int i = 1; i < argc; ++i) {
        // 两种写法都收。het_system.py 的 --options 是 argparse 解析的，值以 "--"
        // 开头时得写成 --options=--npu 才不会被当成新选项；"npu" 这个写法把这个坑
        // 绕开了。
        if (strcmp(argv[i], "--npu") == 0 || strcmp(argv[i], "npu") == 0) {
            want_npu = 1;
        } else {
            printf("host: 未知参数 %s\n", argv[i]);
            return 1;
        }
    }

    // in[] 先填好。这 64 次写是 uncacheable 的，每一次都会以独立的包穿过 LLC 之下
    // 的 CommMonitor，于是 host trace 里能看到 64 条落在 shared_buffer 的写 ——
    // 这正是归并工具用来判定"host 与 NPU 真的碰了同一段地址"的东西。
    for (uint32_t i = 0; i < WORDS; ++i) {
        in_buf[i] = pattern(i);
    }
    // out[] 清零，这样"NPU 一个字都没写"和"NPU 写错了"在 out[] 上是可区分的。
    for (uint32_t i = 0; i < WORDS; ++i) {
        out_buf[i] = 0u;
    }
    printf("host: in[0..%u) 已写入 shared_buffer 0x%08x\n", WORDS, SHARED_IN);

    if (!want_npu) {
        // 没有加速器时的自检：读回自己写的，确认这段映射真的通到了内存。
        for (uint32_t i = 0; i < WORDS; ++i) {
            if (in_buf[i] != pattern(i)) {
                printf("host: 错误 —— 回读 in[%u] 不一致\n", i);
                return 3;
            }
        }
        printf("host: 无加速器模式，共享区读写自检通过\n");
        return 0;
    }

    const int rc = run_npu();
    printf(rc == 0 ? "host: 全部通过\n" : "host: 失败\n");
    return rc;
}
