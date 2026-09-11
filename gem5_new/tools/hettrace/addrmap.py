"""本文件由 scripts/gen_addrmap.py 从 addrmap.json 生成，请勿手改。

统一物理地址空间 —— Python 侧镜像。"""

# MAP_ADDR_BITS 是这张图的宽度；NPU_ADDR_BITS 是 CoralNPU 的 AXI 地址位宽，
# 只约束 NPU 需要触及的区域（vortex_bar 就在它之外）。
MAP_ADDR_BITS = 64
NPU_ADDR_BITS = 32
TICKS_PER_SECOND = 1000000000000

LEVELS = {
    "post_llc": 0,
    "pre_cache": 1,
    "axi_master": 2,
    "interconnect": 3,
}
LEVEL_NAMES = {v: k for k, v in LEVELS.items()}

# name -> (id, level, clock_mhz, clock_period_ticks)
SOURCES = {
    "host": (0, "post_llc", 2000, 500),
    "vortex": (1, "post_llc", 1000, 1000),
    "coralnpu": (2, "axi_master", 500, 2000),
}
SRC_NAME_BY_ID = {v[0]: k for k, v in SOURCES.items()}

# name -> (base, size, kind, accessors)
REGIONS = {
    "boot_rom": (0x0, 0x10000000, "rom", ('host',)),
    "npu_slave": (0x10000000, 0x10000000, "mmio", ('host',)),
    "vortex_cp": (0x20000000, 0x200, "mmio", ('host',)),
    "npu_pio": (0x30000000, 0x1000, "mmio", ('host',)),
    "host_heap": (0x80000000, 0x10000000, "dram", ('host',)),
    "shared_buffer": (0x90000000, 0x10000000, "dram", ('host', 'coralnpu')),
    "vortex_vram": (0xA0000000, 0x10000000, "dram", ('vortex',)),
    "npu_work": (0xB0000000, 0x10000000, "dram", ('host', 'coralnpu')),
    "npu_mailbox": (0xC0000000, 0x10, "mmio", ('host', 'coralnpu')),
    "vortex_bar": (0x100000000, 0x100000000, "bar", ('host', 'vortex')),
}

DRAM_WINDOW = (0x80000000, 0x40000000)

# HETTRACE_FILTER=dram 时 writer 只记录落在这些窗口里的访问。它比 dram_window 多一个 vortex_bar：过滤器要挡掉的是 core-local 命中与 MMIO 寄存器读写，而经 BAR 走的访问是真实的内存流量，两侧（host 与 vortex）都必须留下才能对出共享字节。dram_window 本身的含义不动 —— IsDram() 仍然只表示 CoralNPU 的 DDR 判定。
TRACE_WINDOWS = (
    ("dram_window", 0x80000000, 0x40000000),
    ("vortex_bar", 0x100000000, 0x100000000),
)

# 任意两源都可能在此交接的区域（accessor >= 2）。validate 用它回答"这批 trace
# 里到底有没有跨源交接"—— 不同的源两两配对，交接区不是同一个：host+NPU 在
# shared_buffer / npu_work，host+Vortex 在 vortex_bar。当前地址宽度约束下
# 不存在三方以同一物理地址直连共享的区域。
HANDOFF_REGIONS = ('shared_buffer', 'npu_work', 'npu_mailbox', 'vortex_bar')


def region_of(addr):
    """返回 addr 所属区域名，未映射返回 None。"""
    for name, (base, size, _kind, _acc) in REGIONS.items():
        if base <= addr < base + size:
            return name
    return None


def is_dram(addr):
    base, size = DRAM_WINDOW
    return base <= addr < base + size


def is_traced(addr):
    """HETTRACE_FILTER=dram 时该地址是否会被记录。C++ 侧 IsTraced() 的镜像。"""
    for _name, base, size in TRACE_WINDOWS:
        if base <= addr < base + size:
            return True
    return False


def may_access(src_name, addr):
    """该源是否被允许访问此地址。用于 validate 阶段发现地址映射违约。"""
    name = region_of(addr)
    if name is None:
        return False
    return src_name in REGIONS[name][3]
