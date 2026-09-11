# 异构系统 gem5 配置 —— host CPU + CoralNPU + Vortex，经同一个
# memory-side 互连与内存 responder 运行。
#
#   HETTRACE_DIR=/tmp/t $GEM5_HOME/build/X86/gem5.opt \
#       $GEM5_HOME/configs/het/het_system.py \
#       --cmd   <host x86 可执行文件> \
#       --npu-library    <libcoralnpu-gem5.so> --npu-kernel <xx.elf> \
#       --vortex-library <libvortex-gem5.so>
#
# 两个加速器都是**可选**的：只给 --cmd 就是纯 host 系统（用来单独验 host tap），
# 加上 --npu-* 就是 host+NPU，再加 --vortex-* 才是完整三方。分阶段开是刻意的 ——
# 三个 trace 一起出问题时，能一条腿一条腿地关掉去定位。
#
# 与 coralnpu_only.py 的分工：那个没有 CPU，验的是"gem5 能不能自己把 NPU 跑起来"；
# 这个有 host，验的是 host↔NPU 经 shared_buffer、host↔Vortex 经
# vortex_bar 的两组字节交接，以及三份 trace 能否对齐到同一时间轴。
# CoralNPU 只有 32 位地址，无法直接访问 4GiB 以上的 Vortex BAR；
# 所以不声称 NPU 与 Vortex 直接共享同一物理字节。
#
# ---- 唯一 tap 在哪 ----
# system.axi_monitor 位于主互连的 memory-side、所有内存 responder 的共同上游。
# host、Vortex core/CP DMA、CoralNPU DMA 到内存的 packet 都经过这里，再按 gem5
# requestor 名分写 host/vortex/coralnpu 三个 HETTrace v2 文件。设备库自己的 tap 在
# 本配置中关闭，避免多观察点带来的重复记录与文件名冲突。

import argparse
import os
import shlex
import sys

import m5
from m5.objects import (
    AddrRange,
    AtomicSimpleCPU,
    Cache,
    HetAxiMonitor,
    L2XBar,
    NoncoherentXBar,
    Process,
    Root,
    SEWorkload,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    UnifiedTimingMemory,
    VoltageDomain,
)

# ---------------------------------------------------------------------------
# 地址映射：与 addrmap.json 逐字对应（(base, size)）。
#
# 这里写死而不去 import 本项目的 tools/hettrace/addrmap.py：配置脚本跑在 gem5 自带
# 的 python 解释器里，sys.path 和工作目录都不受本项目控制。对不上的后果是可观测的
# —— trace 里会出现 unmapped 记录，hettrace validate 直接报出来。
# ---------------------------------------------------------------------------
HOST_HEAP     = (0x80000000, 0x10000000)  # host 私有：代码/堆/栈的物理页都从这出
SHARED_BUFFER = (0x90000000, 0x10000000)  # 当前为 host↔NPU 功能交接区
VORTEX_VRAM   = (0xA0000000, 0x10000000)  # trace 中保留的 Vortex 设备地址视图。
                                          # 在线数据由 VORTEX_BAR 对应的统一稀疏内存持有；
                                          # host 仍只能经 VORTEX_BAR 访问，见下。
NPU_WORK      = (0xB0000000, 0x10000000)  # NPU 工作区，host 预置权重
NPU_PIO       = (0x30000000, 0x00001000)  # CoralNPU SimObject 的寄存器窗口
VORTEX_CP     = (0x20000000, 0x00000200)  # Vortex CP 寄存器堆

# Vortex 的 BAR 窗口 —— host 和 Vortex 以不同地址视图访问同一份统一稀疏内存。
#
# 这两个数**不是**可以随便选的：Vortex 的 host runtime
# (sw/runtime/gem5/driver.h) 把 PIN_BASE_ADDR / PIN_REGION_SIZE 写成了
# constexpr，host 侧访问 VRAM 就是往这个固定 VA 上做 volatile 访问。配置这边填的
# pin_addr 只是决定 membus 把哪段物理地址路由给设备，两边对不上的话 host 的写会落
# 到别的设备地址上 —— 而设备**不会报错**，只是算出来的结果不对。所以改这里必须同时
# 改 driver.h，反之亦然。
#
# 4 GiB 之上 + 4 GiB 大小是上游选的，理由也成立：BAR 是 host 看设备地址空间的窗口，
# `dev_addr = 访问地址 - pin_addr`，而 mem_alloc 可以在整个 32 位设备空间里发地址，
# 所以窗口必须盖满 4 GiB；放在 4 GiB 之上则是为了不撞被仿真进程的低位 VA 布局。
#
# 这也是它与 addrmap.json 里 vortex_vram (0xa0000000) 的区别：那一条描述的是**设备
# 内部**的地址（Vortex tap 记的就是设备地址），这一条是**host 物理**窗口。同一段字节
# 在两侧有两个地址，差一个 pin_addr。
VORTEX_BAR    = (0x100000000, 0x100000000)

PAGE = 0x1000

HOST_CLOCK   = "2GHz"    # = addrmap.json 里 host 的 clock_mhz 2000
VORTEX_CLOCK = "1GHz"    # = vortex 的 clock_mhz 1000
NPU_CLOCK    = "500MHz"  # = coralnpu 的 clock_mhz 500


# ---------------------------------------------------------------------------
# Cache 层次。gem5 没有自带这几个类，各家配置脚本都是自己定义的；这里给一份最小
# 的、够用的。数值不追求对标某颗真实芯片 —— 本项目要的是"有一层 LLC，其下的流量
# 可观测"，尺寸只影响 trace 的条数，不影响正确性。
# ---------------------------------------------------------------------------
class L1Cache(Cache):
    assoc = 8
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20


class L1ICache(L1Cache):
    size = "32KiB"
    is_read_only = True
    writeback_clean = True


class L1DCache(L1Cache):
    size = "32KiB"


class L2Cache(Cache):
    size = "1MiB"
    assoc = 16
    tag_latency = 10
    data_latency = 10
    response_latency = 10
    mshrs = 32
    tgts_per_mshr = 12
    write_buffers = 16


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cmd", required=True,
                    help="host 侧 x86 可执行文件（SE 模式，静态或动态都行）")
    ap.add_argument("--options", default="",
                    help="传给它的参数，按 shell 规则分词。值以 - 开头时必须写成"
                         " --options=-x 的形式，否则 argparse 把它当成新选项")
    ap.add_argument("--env", action="append", default=[],
                    help="KEY=VAL，可重复。被仿真进程的环境变量")

    ap.add_argument("--npu-library", default="",
                    help="libcoralnpu-gem5.so；不给就不实例化 CoralNPU")
    ap.add_argument("--npu-kernel", default="",
                    help="NPU 上跑的 RISC-V ELF。给了但不 --npu-auto-start 的话，"
                         "由 host 写 NPU_PIO+0x00 的 bit0 启动")
    ap.add_argument("--npu-auto-start", action="store_true",
                    help="startup() 里就启动 NPU，不等 host 按启动键")
    ap.add_argument("--npu-no-share", action="store_true",
                    help="反向对照用：让 NPU 的 AXI master 回落到设备库内部的私有 "
                         "DDR 数组。地址还是那些地址，但数据不再共享，协同负载的"
                         "校验和必须因此对不上。加这个开关就是为了能主动制造出"
                         "'共享是假的'那种情况，证明正向那一遍不是碰巧过的")

    ap.add_argument("--vortex-library", default="",
                    help="libvortex-gem5.so；不给就不实例化 VortexGPGPU")
    ap.add_argument("--vortex-kernel", default="",
                    help="预载到设备里的 .vxbin。走 host runtime 提交的话留空")
    ap.add_argument("--vortex-host-rt-dir", default="",
                    help="含 libvortex.so 与 libvortex-gem5-x86_64.so 的目录；"
                         "给了就自动补 LD_LIBRARY_PATH 与 VORTEX_DRIVER")
    ap.add_argument("--vortex-bar-skew", type=lambda s: int(s, 0), default=0,
                    help="反向对照用：把 BAR 的物理基址挪开这么多字节，而 host "
                         "runtime 里那个 constexpr 不动。于是 host 的写落到别的设备"
                         "地址上 —— 地址流看着一切正常，只有数据是错的。协同负载的"
                         "自检必须因此失败，否则说明'共享'那一遍是碰巧过的")
    ap.add_argument("--vortex-fast-forward", action="store_true",
                    help="用 AtomicSimpleCPU 跳过动态 loader；第一次 Vortex CP "
                         "写寄存器时切回 TimingSimpleCPU。只缩短初始化，不跳过"
                         "任何设备或存储请求。仅用于回归；性能区间从切换 tick 起算")

    ap.add_argument("--num-cpus", type=int, default=4,
                    help="CPU 线程上下文数。Vortex 的 host runtime 会起工作线程，"
                         "SE 模式下 clone() 需要空闲上下文，所以默认给 4")
    ap.add_argument("--max-ticks", type=int, default=int(1e11),
                    help="兜底上限（默认 100ms）。到了上限退出码非 0")
    ap.add_argument("--mem-latency", default="30ns",
                    help="gem5 功能 responder 的固定延迟；不作为最终 DRAM 时序")
    ap.add_argument("--mem-bandwidth", default="12.8GiB/s",
                    help="gem5 功能 responder 的总带宽；不作为最终 DRAM 时序")
    ap.add_argument("--no-axi-trace", action="store_true",
                    help="关闭统一内存侧 AXI4 HETTrace monitor")
    ap.add_argument("--host-trace-inst", action="store_true",
                    help="把 host 取指流量也记进 trace。默认不记：条数会翻好几倍，"
                         "而共享内存分析根本用不到它")
    ap.add_argument("--axi-data-bytes", type=int, default=16,
                    help="packet 投影到 AXI4 时的数据总线字节宽度（默认 16）")
    ap.add_argument("--axi-id-bits", type=int, default=8,
                    help="packet 投影到 AXI4 时的 ID 位宽（默认 8）")
    return ap.parse_args()


def build_memories(system, args):
    """host 页池与 XPU 共享窗口的功能内存。

    为什么分成三个而不是一段连续的 [0x80000000, 0xc0000000)：中间的
    vortex_vram (0xa0000000) 是 trace 的设备内地址视图，不是 host 物理页池。
    Vortex 数据由 4 GiB 以上的 VORTEX_BAR 物理窗口映射到共享后端；低 4 GiB
    这里仍只声明 host/NPU 使用的物理区域。

    conf_table_reported 是这里的关键开关，它决定一段内存是否进 SE 模式的物理页
    池（SEWorkload::setSystem -> MemPools::populate 只收 conf-reported 的段）。
    只有 host_heap 报上去，于是被仿真进程的代码/堆/栈物理页只可能落在
    [0x80000000, 0x90000000)，**永远不会**被分配到 shared_buffer 里去。不这么做的
    话，进程一多占几页就悄悄踩进交接区，而 trace 上看起来就只是"host 也访问了
    shared_buffer"，完全看不出是踩坏了。

    另外 MemPools::allocPhysPages 默认只用 pool 0，也就是地址最低的那段
    conf-reported 内存 —— host_heap 的 0x80000000 正好是最低的。

    host_heap 保留独立 SimpleMemory，只用于 SE 进程的代码/堆/栈页池。显式 XPU
    窗口由 UnifiedTimingMemory 保持共享字节与 gem5 timing response，保证功能可跑；
    它的定延迟不是 DRAM 性能真值。统一 monitor 导出的 open-loop trace 再由外部
    mem_sim/hbm_sim 给出存储时序结果。
    """
    system.host_mem = SimpleMemory(
        range=AddrRange(HOST_HEAP[0], size=HOST_HEAP[1]),
        conf_table_reported=True,
        latency=args.mem_latency,
        bandwidth=args.mem_bandwidth,
    )
    shared_ranges = [
        AddrRange(SHARED_BUFFER[0], size=SHARED_BUFFER[1]),
        AddrRange(NPU_WORK[0], size=NPU_WORK[1]),
    ]
    if args.vortex_library:
        shared_ranges.append(AddrRange(VORTEX_BAR[0], size=VORTEX_BAR[1]))
    system.unified_mem = UnifiedTimingMemory(
        ranges=shared_ranges,
        latency=args.mem_latency,
        bandwidth=args.mem_bandwidth,
    )
    system.host_mem.port = system.memory_bus.mem_side_ports
    system.unified_mem.port = system.memory_bus.mem_side_ports

    # mem_ranges 在 SE 模式下是给别人看的说明；真正的路由靠每个 responder 自己
    # 声明的区间。这里只列 host_heap：SEWorkload 拿的是 physmem 的 conf 区间，
    # 不是这个列表，写全了反而容易让人以为改这里就能改页池。
    system.mem_ranges = [AddrRange(HOST_HEAP[0], size=HOST_HEAP[1])]


def build_cpus(system, args, process):
    cpu_class = AtomicSimpleCPU if args.vortex_fast_forward else TimingSimpleCPU
    system.cpu = [cpu_class(cpu_id=i) for i in range(args.num_cpus)]
    system.multi_thread = args.num_cpus > 1
    system.l2bus = L2XBar()

    for i, cpu in enumerate(system.cpu):
        cpu.icache = L1ICache()
        cpu.dcache = L1DCache()
        cpu.icache.cpu_side = cpu.icache_port
        cpu.dcache.cpu_side = cpu.dcache_port
        cpu.icache.mem_side = system.l2bus.cpu_side_ports
        cpu.dcache.mem_side = system.l2bus.cpu_side_ports
        # CPU model hand-over also transfers the x86 page-table walker ports.
        # They must be connected on the active fast-forward CPU or
        # BaseMMU::takeOverFrom correctly rejects the incomplete topology.
        if args.vortex_fast_forward:
            cpu.mmu.connectWalkerPorts(
                system.membus.cpu_side_ports, system.membus.cpu_side_ports
            )

        cpu.createInterruptController()
        # x86 的 InterruptController 有自己的 pio/int_requestor/int_responder，
        # 不接到 membus 上 gem5 会在 instantiate() 阶段就报未连接的 port。
        cpu.interrupts[0].pio = system.membus.mem_side_ports
        cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
        cpu.interrupts[0].int_responder = system.membus.mem_side_ports

        # SE 模式下每个 CPU 都要有 workload；除 cpu[0] 外都停着，等 clone() 找上来。
        cpu.workload = process
        cpu.createThreads()

    if args.vortex_fast_forward:
        system.timing_cpu = [
            TimingSimpleCPU(cpu_id=i, switched_out=True)
            for i in range(args.num_cpus)
        ]
        for cpu in system.timing_cpu:
            cpu.workload = process
            cpu.createThreads()

    system.l2cache = L2Cache()
    system.l2cache.cpu_side = system.l2bus.mem_side_ports

    system.l2cache.mem_side = system.membus.cpu_side_ports


def build_npu(system, args):
    from m5.objects import CoralNPU

    system.coralnpu = CoralNPU(
        library=args.npu_library,
        kernel=args.npu_kernel,
        pio_addr=NPU_PIO[0],
        # 让设备声明整整一页，而不是默认的 0x20 字节。host 是整页映射进来的，
        # 一次越界访问在 0x20 之后就会变成 xbar 的 "Unable to find destination"
        # fatal —— 那个错离现场很远。整页覆盖的话，设备自己会 warn 未知偏移。
        pio_size=NPU_PIO[1],
        clk_domain=SrcClockDomain(
            clock=NPU_CLOCK, voltage_domain=system.clk_domain.voltage_domain
        ),
        auto_start=args.npu_auto_start,
        # 不让 NPU 结束仿真：这套系统里是 host 说什么时候完事。NPU 跑完只是把
        # tick 链停下，host 轮询 REG_STATUS 的 halted 位就能看见。
        exit_on_complete=False,
        share_memory=not args.npu_no_share,
        # het_system 只允许统一 interconnect monitor 写 trace；设备内 tap 留给
        # coralnpu_only.py 这类独立诊断配置。
        trace_enable=False,
    )
    system.coralnpu.pio = system.membus.mem_side_ports
    # Coral AXI master 的请求从这个 timing DMA port 进入与 host 相同的 membus；
    # R/B 只有在功能内存 response 回来后才注入 NPU，保持真实停等/反压语义。
    system.coralnpu.dma = system.membus.cpu_side_ports


def build_vortex(system, args):
    from m5.objects import VortexGPGPU

    system.vortex = VortexGPGPU(
        library=args.vortex_library,
        kernel=args.vortex_kernel,
        pio_addr=VORTEX_CP[0],
        pio_size=VORTEX_CP[1],
        # 见 VORTEX_BAR 处的说明：这个值由 driver.h 的 constexpr 决定，不是自由参数。
        # --vortex-bar-skew 会故意把它挪开，用来做反向对照。
        pin_addr=VORTEX_BAR[0] + args.vortex_bar_skew,
        pin_size=VORTEX_BAR[1],
        clk_domain=SrcClockDomain(
            clock=VORTEX_CLOCK, voltage_domain=system.clk_domain.voltage_domain
        ),
        # core 访存与 CP DMA 都走 DmaPort；BAR 数据归统一稀疏内存所有。只有收到
        # 收到功能内存的 response 后才推进后继设备周期。
        timing_memory=True,
        exit_on_first_pio=args.vortex_fast_forward,
        trace_enable=False,
        # 主配置关闭设备 tap；仍给兼容 ABI 设置物理 BAR offset，避免诊断时把设备
        # 局部地址误归到别的源区域。正式记录只来自统一 HetAxiMonitor。
        trace_addr_offset=VORTEX_BAR[0],
    )
    system.vortex.pio = system.membus.mem_side_ports
    system.vortex.dma = system.membus.cpu_side_ports


def host_env(args):
    env = list(args.env)
    if args.vortex_host_rt_dir:
        # libvortex.so 会按 VORTEX_DRIVER 去 dlopen libvortex-<driver>.so；
        # 两个 .so 都在这个目录里，所以一并塞进 LD_LIBRARY_PATH。
        env.append(f"LD_LIBRARY_PATH={args.vortex_host_rt_dir}")
        if not any(e.startswith("VORTEX_DRIVER=") for e in env):
            env.append("VORTEX_DRIVER=gem5-x86_64")
    return env


def map_device_windows(process, args):
    """把设备持有的物理区间按 VA==PA 映射进被仿真进程。

    必须在 m5.instantiate() 之后调用。host 侧于是可以像普通内存一样读写这些窗口，
    路由由 membus 按 responder 声明的区间完成。

    cacheable=False 不是保险起见，是必需的：
      - MMIO 窗口（NPU_PIO / VORTEX_CP）本来就不能进 cache；
      - shared_buffer 是 CPU 与 DMA 设备的显式交接区。当前测试程序没有实现 cache
        flush/coherence 协议，因此把它映射成 uncacheable，让 host 与 NPU 的 timing
        request 都到达同一个内存 responder；代价是 host 侧慢一点。
    """
    regions = [("shared_buffer", SHARED_BUFFER), ("npu_work", NPU_WORK)]
    if args.npu_library:
        regions.append(("npu_pio", NPU_PIO))
    if args.vortex_library:
        # CP 窗口只有 0x200 字节，映射得按页对齐，所以给整页。
        regions.append(("vortex_cp", (VORTEX_CP[0], PAGE)))
        # BAR 窗口按 driver.h 的 constexpr 映射，**不带** --vortex-bar-skew ——
        # 反向对照要制造的正是"host 以为 BAR 在这、设备其实在那"的错位。
        regions.append(("vortex_bar", VORTEX_BAR))

    for name, (base, size) in regions:
        process.map(base, base, size, cacheable=False)
        print(f"  map {name:<14} VA=PA=0x{base:08x} +0x{size:x} uncacheable")


def die(msg):
    """报一句话然后退出。

    **不要**写成 `raise SystemExit("错误: ...")`。在 gem5 里那句话根本不会被打出
    来：src/sim/main.cc 捕获 SystemExit 之后做的是 `e.value().attr("code")
    .cast<int>()`，code 是个 str 就抛 pybind11::cast_error，进程 terminate 掉，屏幕
    上只剩

        terminate called after throwing an instance of 'pybind11::cast_error'
          what():  Unable to cast Python instance of type <class 'str'> to C++ type 'int'
        Program aborted at tick 0

    加一段 libc backtrace。那个现象与真实原因（某个 --xxx 指的文件不存在）毫无关
    系，而且看着像 gem5 或设备库炸了。本项目在三源负载上花了一轮排查才找到这里。
    """
    print(msg, file=sys.stderr)
    raise SystemExit(1)


def main():
    args = parse_args()

    checks = [(args.cmd, "cmd")]
    if args.npu_library:
        checks.append((args.npu_library, "npu-library"))
    if args.npu_kernel:
        checks.append((args.npu_kernel, "npu-kernel"))
    if args.vortex_library:
        checks.append((args.vortex_library, "vortex-library"))
    if args.vortex_kernel:
        checks.append((args.vortex_kernel, "vortex-kernel"))
    for path, what in checks:
        if not os.path.isfile(path):
            die(f"错误: --{what} 指向的文件不存在: {path}")
    if args.npu_kernel and not args.npu_library:
        die("错误: 给了 --npu-kernel 却没给 --npu-library")
    if args.vortex_kernel and not args.vortex_library:
        die("错误: 给了 --vortex-kernel 却没给 --vortex-library")
    if args.vortex_fast_forward and not args.vortex_library:
        die("错误: --vortex-fast-forward 需要 --vortex-library；"
            "切换触发点来自第一次 Vortex CP 写")
    if args.vortex_kernel:
        die("错误: het_system.py 的 Vortex 功能路径只支持 host runtime/CP 提交；"
            "standalone 预载内核请使用 vortex_only.py")

    system = System()
    system.clk_domain = SrcClockDomain(
        clock=HOST_CLOCK, voltage_domain=VoltageDomain()
    )
    # timing 模式保证 CPU load/store 与 Coral AXI master DMA 都等待功能 response。
    # 这些延迟只服务可执行性；最终 DRAM 时序由离线 hbm_sim 给出。
    system.mem_mode = "atomic" if args.vortex_fast_forward else "timing"

    system.membus = SystemXBar()
    # functional 初始化/调试访问仍可能使用 system_port；正常 NPU AXI 数据流走上面
    # 连接的 dma timing port。
    system.system_port = system.membus.cpu_side_ports

    # 唯一内存观测点。下游 NoncoherentXBar 只做地址解码，让 host_mem 与
    # unified_mem 共享同一个上游端口；其流水延迟设为 0，不冒充存储时序模型。
    system.axi_monitor = HetAxiMonitor(
        trace_enable=not args.no_axi_trace,
        trace_host=True,
        trace_vortex=bool(args.vortex_library),
        trace_coralnpu=bool(args.npu_library),
        trace_inst_fetch=args.host_trace_inst,
        axi_data_bytes=args.axi_data_bytes,
        axi_id_bits=args.axi_id_bits,
    )
    system.memory_bus = NoncoherentXBar(
        width=args.axi_data_bytes,
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        header_latency=0,
    )
    system.axi_monitor.cpu_side_port = system.membus.mem_side_ports
    system.axi_monitor.mem_side_port = system.memory_bus.cpu_side_ports

    build_memories(system, args)

    argv = [args.cmd] + shlex.split(args.options)
    process = Process(pid=100, cmd=argv, executable=args.cmd, env=host_env(args))
    system.workload = SEWorkload.init_compatible(args.cmd)

    build_cpus(system, args, process)

    if args.npu_library:
        build_npu(system, args)
    if args.vortex_library:
        build_vortex(system, args)

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print("---- 地址窗口 ----")
    map_device_windows(system.cpu[0].workload[0], args)

    legs = ["host"]
    if args.npu_library:
        legs.append("coralnpu")
    if args.vortex_library:
        legs.append("vortex")
    print(f"---- 开跑: {' + '.join(legs)}, max_ticks={args.max_ticks} ----")
    print(f"     cmd: {' '.join(argv)}")

    event = m5.simulate(args.max_ticks)
    if args.vortex_fast_forward:
        if event.getCause() != "VortexGPGPU: timing phase":
            die("错误: fast-forward 阶段未到达 Vortex CP 写寄存器；"
                f"提前结束原因: {event.getCause()}")
        print(f"---- 首次 Vortex CP 写 @ tick {m5.curTick()}：切换到 timing CPU ----")
        m5.switchCpus(system, list(zip(system.cpu, system.timing_cpu)))
        m5.stats.reset()
        remaining = args.max_ticks - m5.curTick()
        if remaining <= 0:
            die("错误: fast-forward 已耗尽 --max-ticks")
        event = m5.simulate(remaining)
    print(f"---- 结束: {event.getCause()} @ tick {m5.curTick()} ----")

    if "simulate() limit reached" in event.getCause():
        print("错误: 到达 tick 上限 —— host 或某个设备没有停下来")
        raise SystemExit(1)
    # host 进程的退出码要透出来，否则 workload 里的自检失败在 gem5 这一层是静默的
    # —— gem5 自己无论 host 程序 return 几都是 0 退出。
    if event.getCode() != 0:
        print(f"错误: host 进程退出码 {event.getCode()}")
        raise SystemExit(1)


if __name__ in ("__main__", "__m5_main__"):
    main()
