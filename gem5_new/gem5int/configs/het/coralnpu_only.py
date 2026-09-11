# CoralNPU 单设备 gem5 配置 —— 设备本身的验收脚本，不是异构系统。
#
#   HETTRACE_DIR=/tmp/t $GEM5_HOME/build/X86/gem5.opt \
#       $GEM5_HOME/configs/het/coralnpu_only.py \
#       --library $CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so \
#       --kernel  <ddr_touch.elf>
#
# 为什么单独存在：coralnpuint/tests/run_smoke.sh 已经在纯 C 里把设备库验过一遍，
# 但那条路径里"gem5"是假的 —— 时钟由测试程序推，内存后端是测试程序里的数组。
# 本脚本换成真的：时钟是 gem5 事件队列，AXI 请求经 DmaPort 进入 gem5 timing
# 内存，只有 response 返回才给 NPU R/B，时间戳也是真的 curTick()。
#
# 系统里没有 CPU。这是有意的：要验的是"gem5 能不能自己把 NPU 跑起来并落 trace"，
# 掺进一个 host CPU 只会把失败原因变模糊。host 侧的配合在 het_system.py 里。

import argparse
import os
import sys

import m5
from m5.objects import (
    AddrRange,
    CoralNPU,
    DDR3_1600_8x8,
    IOXBar,
    MemCtrl,
    Root,
    SrcClockDomain,
    System,
    VoltageDomain,
)

# 与 addrmap.json 保持一致。写死而不是去解析 json：gem5 的配置脚本跑在 gem5 自带
# 的 python 里，工作目录和 sys.path 都不受本项目控制，import 本项目的模块很脆。
# 数值对不上时 hettrace validate 会立刻报出来（区域分布会出现 unmapped）。
DRAM_BASE = 0x80000000
DRAM_SIZE = 0x40000000  # [0x80000000, 0xc0000000) —— 库里 IsDdrAddress() 的区间
NPU_PIO_ADDR = 0x30000000
NPU_CLOCK = "500MHz"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--library", required=True,
                    help="libcoralnpu-gem5.so 的绝对路径")
    ap.add_argument("--kernel", required=True,
                    help="要跑的 RISC-V ELF（推荐 gem5int/ddr_touch.elf）")
    ap.add_argument("--max-ticks", type=int, default=int(1e9),
                    help="兜底上限，防止内核跑飞时 gem5 一直转（默认 1ms）")
    ap.add_argument("--no-trace", action="store_true",
                    help="关掉 tap，用来分离'设备跑不动'和'trace 写不出'")
    args = ap.parse_args()

    for path, what in ((args.library, "library"), (args.kernel, "kernel")):
        if not os.path.isfile(path):
            # 先 print 再 SystemExit(1)，不能 SystemExit("消息")：gem5 的
            # src/sim/main.cc 会把 SystemExit.code 强转成 int，字符串会让进程死在
            # 一个 pybind11::cast_error 上，消息一个字都看不到。
            print(f"错误: --{what} 指向的文件不存在: {path}", file=sys.stderr)
            raise SystemExit(1)

    system = System()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    # NPU 的 AXI master 现在走 timing DmaPort；必须使用 timing 模式，才能让 DDR
    # 控制器的排队/响应时间反压 NPU。
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange(DRAM_BASE, size=DRAM_SIZE)]

    system.membus = IOXBar()
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8(range=system.mem_ranges[0])
    system.mem_ctrl.port = system.membus.mem_side_ports

    # system_port 留给 functional 初始化/调试访问；正常 AXI 数据流走 dma timing
    # port（见下方连接）。
    system.system_port = system.membus.cpu_side_ports

    system.coralnpu = CoralNPU(
        library=args.library,
        kernel=args.kernel,
        pio_addr=NPU_PIO_ADDR,
        clk_domain=SrcClockDomain(
            clock=NPU_CLOCK, voltage_domain=system.clk_domain.voltage_domain
        ),
        # 没有 host 来按启动键，所以自启；跑完直接结束仿真，否则事件队列空了
        # gem5 也会退出，但那样就分不清"正常跑完"和"一个事件都没排上"。
        auto_start=True,
        exit_on_complete=True,
        share_memory=True,
        trace_enable=not args.no_trace,
    )
    system.coralnpu.pio = system.membus.mem_side_ports
    system.coralnpu.dma = system.membus.cpu_side_ports

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"---- 开跑 (max_ticks={args.max_ticks}) ----")
    event = m5.simulate(args.max_ticks)
    print(f"---- 结束: {event.getCause()} @ tick {m5.curTick()} ----")

    # 跑到上限说明内核没停下来。exit(1) 而不是安静返回：这个脚本是被 CI 和人
    # 直接盯着看的，"跑飞"必须体现在退出码上。
    if "simulate() limit reached" in event.getCause():
        print("错误: 到达 tick 上限，内核没有 halt/wfi")
        raise SystemExit(1)


main()
