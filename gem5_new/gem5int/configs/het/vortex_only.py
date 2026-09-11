# Vortex 单设备 gem5 配置 —— 只验 tap，不是异构系统。
#
#   HETTRACE_DIR=/tmp/t HETTRACE_FILTER=all $GEM5_HOME/build/X86/gem5.opt \
#       $GEM5_HOME/configs/het/vortex_only.py \
#       --library <vortex build>/sim/simx/libvortex-gem5.so \
#       --kernel  workloads/vortex_smoke/build/kernel.bin
#
# 与 coralnpu_only.py 同一个角色：系统里没有 CPU，要回答的问题只有一个 —— gem5
# 能不能把 Vortex 跑起来，并且 tap 能不能落出带 curTick() 时间戳的记录。掺进 host
# 只会让失败原因变模糊。
#
# ---- 为什么它不是异构测试 ----
#
# 设置了 kernel= 之后 VortexGPGPU 进入 standalone 模式：自己 preload、自己 tick、
# 跑完直接 exitSimLoop()。这个"跑完就结束整个仿真"的行为在有 host 的配置里是不能
# 接受的（host 还没跑完），所以 het_system.py 里的 Vortex 腿走的是 CP 驱动的路，
# 需要真正的 .vxbin。host <-> Vortex 的字节共享**没有**在本脚本里验证，见
# docs/03-limitations.md。
#
# ---- BAR 的取舍 ----
#
# pin_size 默认是 0（BAR 关掉）。这里显式打开，并把 pin_addr 放在 0xa0000000
# 而不是上游默认的 0x100000000。
#
# 这么做**只在本脚本里成立**，别照抄到 het_system.py：pin_addr/pin_size 不是自由
# 参数，Vortex 的 host runtime（sw/runtime/gem5/driver.h）把
# PIN_BASE_ADDR=0x100000000、PIN_REGION_SIZE=0x100000000 写成了 constexpr，改了这
# 边不改那边，host 的写会静默落到别的设备地址上（docs/01-address-map.md 硬约束 1）。
# 本脚本里根本没有 host runtime —— standalone 模式下内核由设备自己 preload，
# kernel.S 直接用设备地址 0xa0000000 —— 所以那条约束在这里没有约束对象，BAR 摆在
# 哪儿都不影响结果，摆低一点只是让整个配置留在 32 位地址内、看 log 时省心。
#
# tap 记的是设备地址（trace_addr_offset=0），与 pin_addr 无关，所以记录落在
# addrmap.json 的 vortex_vram 区间里，不是 vortex_bar。

import argparse
import os
import sys

import m5
from m5.objects import (
    AddrRange,
    DDR3_1600_8x8,
    IOXBar,
    MemCtrl,
    Root,
    SrcClockDomain,
    System,
    VoltageDomain,
    VortexGPGPU,
)

# 与 addrmap.json 一致，手抄。理由同 coralnpu_only.py。
HOST_HEAP_BASE = 0x80000000
HOST_HEAP_SIZE = 0x10000000
VORTEX_CP_ADDR = 0x20000000
VORTEX_CP_SIZE = 0x0200
VORTEX_VRAM = 0xA0000000
VORTEX_VRAM_SIZE = 0x10000000
VORTEX_CLOCK = "1GHz"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--library", required=True,
                    help="libvortex-gem5.so 的绝对路径")
    ap.add_argument("--kernel", required=True,
                    help="要跑的 .vxbin/.bin/.hex（见 workloads/vortex_smoke）")
    ap.add_argument("--max-ticks", type=int, default=int(1e9),
                    help="兜底上限，防止内核跑飞（默认 1ms）")
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
    system.mem_mode = "atomic"
    system.mem_ranges = [AddrRange(HOST_HEAP_BASE, size=HOST_HEAP_SIZE)]

    system.membus = IOXBar()
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8(range=system.mem_ranges[0])
    system.mem_ctrl.port = system.membus.mem_side_ports

    # 内存控制器只盖 host_heap，**不盖** vortex_vram：那段地址由设备的 BAR 自己
    # 声明，两个 responder 声明同一段地址会让 xbar fatal。这正是
    # docs/01-address-map.md 硬约束 3 说的那件事，在这个最小配置里也一样成立。
    system.system_port = system.membus.cpu_side_ports

    system.vortex = VortexGPGPU(
        library=args.library,
        kernel=args.kernel,
        pio_addr=VORTEX_CP_ADDR,
        pio_size=VORTEX_CP_SIZE,
        pin_addr=VORTEX_VRAM,
        pin_size=VORTEX_VRAM_SIZE,
        clk_domain=SrcClockDomain(
            clock=VORTEX_CLOCK, voltage_domain=system.clk_domain.voltage_domain
        ),
        trace_enable=not args.no_trace,
        # 内核直接用统一物理空间里的地址（kernel.S 里写死 0xa0000000），所以
        # 不需要偏移。
        trace_addr_offset=0,
    )
    system.vortex.pio = system.membus.mem_side_ports
    system.vortex.dma = system.membus.cpu_side_ports

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"---- 开跑 (max_ticks={args.max_ticks}) ----")
    event = m5.simulate(args.max_ticks)
    print(f"---- 结束: {event.getCause()} @ tick {m5.curTick()} ----")

    if "simulate() limit reached" in event.getCause():
        print("错误: 到达 tick 上限，内核没有终止（vx_tmc 没生效？）")
        raise SystemExit(1)


main()
