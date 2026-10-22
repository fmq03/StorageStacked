// Vortex 侧访存 trace tap。
//
// 安装位置: $VORTEX_HOME/sim/simx/gem5/vortex_trace.h（由 vortexint/install.sh 拷入）
// 依赖的 hettrace 头装在 $VORTEX_HOME/sim/simx/hettrace/，靠 SimX Makefile 里
// 已有的 -I$(SRC_DIR) 解析，因此 Vortex 的构建文件一行都不用改。
//
// ---- tap 挂在哪一层 ----
//
// 挂 vortex::Memory 的 pre_send_hook，经由 Processor::set_mem_telemetry_hook
// 抵达。这是 **LLC 之后、DRAM 之前** 的位置，也就是 Vortex 自己的 cache 层级
// 全部过滤完的 miss 流。
//
// 这个选择是本工程里最关键的一处判断。若改挂 LSU 层（lsu_unit.h），拿到的是
// 每个 warp 每条 load/store 的地址流 —— 那是访存行为分析的数据，不是 DRAM
// 流量：条数会高出一两个数量级，带宽统计虚高，与 host 侧挂在 LLC 下方的
// trace 也没法比较。详见 docs/02-trace-format.md。
//
// ---- 时间戳来自哪里 ----
//
// 不用 Vortex 自己的 cycle 计数，而是通过 tick_provider 回调向 gem5 取当前
// 全局 Tick。设备库被 dlopen 时不链接 gem5，所以只能用回调把 curTick() 递
// 进来。三个源共用同一个 gem5 时间基准，是归并的前提。

#ifndef VORTEX_GEM5_VORTEX_TRACE_H_
#define VORTEX_GEM5_VORTEX_TRACE_H_

#include <cstdint>
#include <functional>

#include <hettrace/addrmap.h>
#include <hettrace/record.h>
#include <hettrace/writer.h>

#include "processor.h"
#include "types.h"
#include <VX_config.h>

namespace vortex_gem5 {

// gem5 侧提供当前全局 tick 的回调。
typedef uint64_t (*TickProvider)(void* ctx);

class VortexTraceTap {
  public:
    // DMA 记录的 ctx。核发出的访问用 hart_id 填 ctx，而 CP 没有 hart —— 用 0 会
    // 让 DMA 混进 hart 0 的足迹里，所以给一个不可能是 hart_id 的哨兵值。
    static constexpr uint32_t kDmaCtx = 0xffffffffu;

    VortexTraceTap() = default;

    // 安装 tap。HETTRACE_DIR 未设置时返回 false 且什么都不做 —— 调用方应把它
    // 当成"用户没要 trace"，而不是错误。
    //
    // addr_offset 加在 MemReq::addr 上，用于把 Vortex 的设备地址搬到
    // addrmap.json 的统一物理空间。若负载已直接用全局物理地址（推荐做法，
    // 见 workloads/shared_buffer），传 0。
    bool Install(vortex::Processor* proc, TickProvider tick_fn, void* tick_ctx,
                 int64_t addr_offset) {
        if (proc == nullptr) return false;
        if (!writer_.Open(hettrace::kSrcVortex, "vortex",
                          hettrace::kLevelPostLlc,
                          hettrace::kClockPeriodTicks_vortex,
                          /*axi_data_bytes=*/VX_CFG_MEM_BLOCK_SIZE)) {
            return false;
        }
        tick_fn_     = tick_fn;
        tick_ctx_    = tick_ctx;
        addr_offset_ = addr_offset;

        proc->set_mem_telemetry_hook(
            [this](const vortex::MemReq& req) { this->OnRequest(req); });
        return true;
    }

    // CP 的 DMA。由 vortex_gpgpu.cpp 的 CommandProcessor::Hooks::dram_{read,write}
    // 调过来，**不是**从 pre_send hook 来的。
    //
    // 为什么必须单独接一条：上游 CP 路径绕过 Vortex 的 cache 层级和
    // vortex::Memory，所以 pre_send hook 完全看不到它；当前 timing bridge 也正是在
    // 这个 hook 上提交 DMA 与门控后继 CP tick。而它恰好是设备内存流量里
    // 最大的一块 —— .vxbin 镜像上传、以及每次载荷在"暂存区 <-> 设备缓冲"之间的中转
    // 都走这里。漏掉它有两个后果，都不是"少一点数据"那么轻：
    //
    //   1. 带宽被显著低估（一次 vecadd 少算掉几十 KB）；
    //   2. 更要紧的是 host 与 Vortex 的 trace 会**看起来毫不相关**。host 只碰暂存
    //      区（driver.h 里那个 PIN 窗口顶部的 64MB aperture），核只碰设备缓冲，两边
    //      地址集不相交 —— 而把两边接起来的那一次搬运正是 CP 干的。少了这条记录，
    //      下游会得出"两个源没有共享"的结论，而字节其实是共享的。
    //
    // 不接 vram_read/vram_write（host 经 BAR 的那条路）：那些访问已经由 gem5 侧的
    // host tap 记过一遍了，在这里再记一遍是重复计入。
    void OnDma(uint64_t addr, uint64_t bytes, hettrace::Op op) {
        if (!writer_.is_open() || bytes == 0) return;
        const uint64_t tick = (tick_fn_ != nullptr) ? tick_fn_(tick_ctx_) : 0;
        // 一次 DMA 可以有几十 KB。按块粒度展开，理由与 EmitBurst 相同：记成一条
        // 巨大的记录会让局部性分析和 hettrace convert 的下游都失真 —— DRAM 是按块
        // 搬的。每块由 Emit() 分配独立 txn、且 axi_len=0，所以它们都是单拍
        // 访问，不能借用 kFlagBurstBeat（该位只表示同一 txn 的非首拍）。
        const uint64_t blk = VX_CFG_MEM_BLOCK_SIZE;
        const uint64_t end = addr + bytes;
        for (uint64_t p = addr & ~(blk - 1); p < end; p += blk) {
            writer_.Emit(tick, static_cast<uint64_t>(
                                   static_cast<int64_t>(p) + addr_offset_),
                         static_cast<uint32_t>(blk), op, kDmaCtx,
                         hettrace::kFlagDma);
        }
    }

    void Close() { writer_.Close(); }

    bool is_open() const { return writer_.is_open(); }

    const hettrace::Stats& stats() const { return writer_.stats(); }

  private:
    void OnRequest(const vortex::MemReq& req) {
        // FLUSH 不搬运数据；io 是 uncacheable MMIO 而非 DRAM 流量。
        // 两者计入都会让带宽虚高。
        if (req.op == vortex::MemOp::FLUSH) return;
        if (req.flags.io) return;

        // LD 记为读，其余（ST 与 AMO_* 全家）记为写。AMO 在 DRAM 侧实际是
        // read-modify-write，记为写是偏保守的选择：宁可高估写带宽，不低估。
        const hettrace::Op op = (req.op == vortex::MemOp::LD)
                                    ? hettrace::kRead
                                    : hettrace::kWrite;

        // MemReq 没有 size 字段：在 Memory 这一层请求恒为块粒度，块大小即
        // processor.cpp 传给 Memory::Config 的 VX_CFG_MEM_BLOCK_SIZE
        // （processor.cpp:41 有 static_assert 保证它等于平台数据宽度）。
        const uint32_t size = VX_CFG_MEM_BLOCK_SIZE;

        const uint64_t tick = (tick_fn_ != nullptr) ? tick_fn_(tick_ctx_) : 0;
        const uint64_t addr =
            static_cast<uint64_t>(static_cast<int64_t>(req.addr) + addr_offset_);

        // ctx 用 hart_id：下游可据此看单个 hart 的访存足迹。
        writer_.Emit(tick, addr, size, op, req.hart_id);
    }

    hettrace::TraceWriter writer_;
    TickProvider          tick_fn_     = nullptr;
    void*                 tick_ctx_    = nullptr;
    int64_t               addr_offset_ = 0;
};

}  // namespace vortex_gem5

#endif  // VORTEX_GEM5_VORTEX_TRACE_H_
