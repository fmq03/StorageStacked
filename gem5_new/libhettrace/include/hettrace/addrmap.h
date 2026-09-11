// 本文件由 scripts/gen_addrmap.py 从 addrmap.json 生成，请勿手改。
//
// 统一物理地址空间。三个 trace 产生者共用。

#ifndef HETTRACE_ADDRMAP_H_
#define HETTRACE_ADDRMAP_H_

#include <cstdint>
#include <cstddef>

namespace hettrace {

// kMapAddrBits 是这张图的宽度；kNpuAddrBits 是 CoralNPU 的 AXI 地址位宽，
// 只约束 NPU 需要触及的区域（vortex_bar 就在它之外）。
constexpr int      kMapAddrBits    = 64;
constexpr int      kNpuAddrBits    = 32;
constexpr uint64_t kTicksPerSecond = 1000000000000ull;

// ---- 源 ID ----
enum SrcId : uint16_t {
    kSrcHost = 0,
    kSrcVortex = 1,
    kSrcCoralnpu = 2,
    kSrcCount = 3,
};

// tap 挂载层级。同一条 trace 里混用不同层级是无意义的，
// 因此每个源在文件头中显式声明自己的层级。
enum TapLevel : uint8_t {
    kLevelPostLlc = 0,
    kLevelPreCache = 1,
    kLevelAxiMaster = 2,
    kLevelInterconnect = 3,
};

// 各源标称时钟（用于把源内 cycle 折算成全局 tick）
constexpr uint64_t kClockPeriodTicks_host = 500ull;  // 2000 MHz
constexpr uint64_t kClockPeriodTicks_vortex = 1000ull;  // 1000 MHz
constexpr uint64_t kClockPeriodTicks_coralnpu = 2000ull;  // 500 MHz

// ---- 区域 ----
// boot_rom: 预留 / 引导。不参与 trace 分析。
constexpr uint64_t kBootRomBase = 0x0ull;
constexpr uint64_t kBootRomSize = 0x10000000ull;

// npu_slave: CoralNPU AXI slave 窗口。host 经此写 TCM 与控制寄存器。
constexpr uint64_t kNpuSlaveBase = 0x10000000ull;
constexpr uint64_t kNpuSlaveSize = 0x10000000ull;
constexpr uint64_t kNpuTcmAddr = 0x10000000ull;  // ITCM + DTCM，core-local。命中此处不算 DRAM 流量，默认不入 trace。
constexpr uint64_t kNpuCtrlResetAddr = 0x10030000ull;  // 写 1 再写 0 触发启动 (core_mini_axi_simulator.cc Run())
constexpr uint64_t kNpuCtrlPcAddr = 0x10030004ull;  // 起始 PC

// vortex_cp: Vortex CP 寄存器堆 (PIO)。与 VortexGPGPU.py 的 pio_addr 默认值一致，勿改。
constexpr uint64_t kVortexCpBase = 0x20000000ull;
constexpr uint64_t kVortexCpSize = 0x200ull;

// npu_pio: gem5 CoralNPU SimObject 的控制窗口 (ctrl/status/entry/emitted/mailbox×4)，与 CoralNPU.py 的 pio_addr 默认值一致，勿改。注意它与上面的 npu_slave 是两回事：npu_slave 是设备库内部的 AXI slave 窗口（host 经它写 TCM），npu_pio 是 gem5 这一侧的寄存器。实际只用到头 0x20 字节，占满一页是为了 host 侧能整页映射。
constexpr uint64_t kNpuPioBase = 0x30000000ull;
constexpr uint64_t kNpuPioSize = 0x1000ull;

// host_heap: host 私有堆。落在 NPU 的 DDR 窗口内但 NPU 不应触及。
constexpr uint64_t kHostHeapBase = 0x80000000ull;
constexpr uint64_t kHostHeapSize = 0x10000000ull;

// shared_buffer: host 与 CoralNPU 的显式交接区。Vortex 的设备地址会加上 4 GiB pin base 后落入 vortex_bar，不能以同一物理地址访问这里。
constexpr uint64_t kSharedBufferBase = 0x90000000ull;
constexpr uint64_t kSharedBufferSize = 0x10000000ull;

// vortex_vram: Vortex 设备**内部**地址空间里的一段，给 vortex_only.py 的裸机内核用（workloads/vortex_smoke/kernel.S 写死了这个基址）。accessors 只有 vortex：Vortex 的内存是设备内的 simx::RAM，host 碰不到这个地址 —— 同一批字节在 host 侧的物理地址是 pin_addr + dev_addr，落在 vortex_bar 里。写成 host+vortex 会让它进 HANDOFF_REGIONS，而那是个永远不可能被两源共同触及的候选，等于给 validate 塞了个假线索。
constexpr uint64_t kVortexVramBase = 0xa0000000ull;
constexpr uint64_t kVortexVramSize = 0x10000000ull;

// npu_work: NPU 私有工作区（权重、activation 暂存）。
constexpr uint64_t kNpuWorkBase = 0xb0000000ull;
constexpr uint64_t kNpuWorkSize = 0x10000000ull;

// npu_mailbox: NPU 经 AXI master 访问的 4×u32 mailbox。参考实现把所有非 DDR 的 master 访问都当 mailbox；本工程收窄为显式窗口，落在窗口外一律记为 unmapped 并计数。
constexpr uint64_t kNpuMailboxBase = 0xc0000000ull;
constexpr uint64_t kNpuMailboxSize = 0x10ull;

// vortex_bar: host 看向 Vortex 设备内存的窗口，host_pa = pin_addr + dev_addr。base/size 都不是自由参数：sw/runtime/gem5/driver.h 把 PIN_BASE_ADDR/PIN_REGION_SIZE 写成 constexpr，host 运行时按这两个数直接算地址、不做 mmap。size 必须是整 4GiB，因为 mem_alloc 可以发出任意 32 位设备地址、且 .vxbin 装在设备地址 0x80000000；base 必须在 4GiB 之上，否则会撞上 SE 模式下被仿真进程自己的低位 VA 布局。因此本区域**超出** npu_addr_bits —— CoralNPU 的 32 位 AXI 连表达它都做不到，NPU 与 Vortex 无法直接共享字节，见 docs/03-limitations.md。kind 为 bar 而非 dram：它不在 CoralNPU 的 DDR 判定区间内，但它是真实内存流量，所以进 trace_windows。
constexpr uint64_t kVortexBarBase = 0x100000000ull;
constexpr uint64_t kVortexBarSize = 0x100000000ull;

// CoralNPU IsDdrAddress() 判定区间
constexpr uint64_t kDramWindowBase = 0x80000000ull;
constexpr uint64_t kDramWindowSize = 0x40000000ull;

struct Region {
    const char* name;
    uint64_t    base;
    uint64_t    size;
    bool        is_dram;
};

constexpr Region kRegions[] = {
    { "boot_rom", 0x0ull, 0x10000000ull, false },
    { "npu_slave", 0x10000000ull, 0x10000000ull, false },
    { "vortex_cp", 0x20000000ull, 0x200ull, false },
    { "npu_pio", 0x30000000ull, 0x1000ull, false },
    { "host_heap", 0x80000000ull, 0x10000000ull, true },
    { "shared_buffer", 0x90000000ull, 0x10000000ull, true },
    { "vortex_vram", 0xa0000000ull, 0x10000000ull, true },
    { "npu_work", 0xb0000000ull, 0x10000000ull, true },
    { "npu_mailbox", 0xc0000000ull, 0x10ull, false },
    { "vortex_bar", 0x100000000ull, 0x100000000ull, false },
};
constexpr size_t kNumRegions = sizeof(kRegions) / sizeof(kRegions[0]);

// 返回 addr 所属区域名，未映射返回 nullptr。线性扫描；仅用于诊断路径，
// 不要放进 per-access 热路径。
inline const char* RegionOf(uint64_t addr) {
    for (size_t i = 0; i < kNumRegions; ++i) {
        if (addr >= kRegions[i].base &&
            addr <  kRegions[i].base + kRegions[i].size) {
            return kRegions[i].name;
        }
    }
    return nullptr;
}

inline bool IsDram(uint64_t addr) {
    return addr >= kDramWindowBase &&
           addr <  kDramWindowBase + kDramWindowSize;
}

// ---- trace 过滤窗口 ----
// HETTRACE_FILTER=dram 时 writer 只记录落在这些窗口里的访问。它比 dram_window 多一个 vortex_bar：过滤器要挡掉的是 core-local 命中与 MMIO 寄存器读写，而经 BAR 走的访问是真实的内存流量，两侧（host 与 vortex）都必须留下才能对出共享字节。dram_window 本身的含义不动 —— IsDram() 仍然只表示 CoralNPU 的 DDR 判定。
struct TraceWindow {
    const char* name;
    uint64_t    base;
    uint64_t    size;
};

constexpr TraceWindow kTraceWindows[] = {
    { "dram_window", 0x80000000ull, 0x40000000ull },
    { "vortex_bar", 0x100000000ull, 0x100000000ull },
};
constexpr size_t kNumTraceWindows =
    sizeof(kTraceWindows) / sizeof(kTraceWindows[0]);

// HETTRACE_FILTER=dram 时 writer 的判据。窗口只有两三个，线性扫描即可 ——
// 但它**在** per-access 热路径上，所以别往里加东西。
inline bool IsTraced(uint64_t addr) {
    for (size_t i = 0; i < kNumTraceWindows; ++i) {
        if (addr >= kTraceWindows[i].base &&
            addr <  kTraceWindows[i].base + kTraceWindows[i].size) {
            return true;
        }
    }
    return false;
}

}  // namespace hettrace

#endif  // HETTRACE_ADDRMAP_H_
