// CoralNPU 侧访存 trace tap（AXI4 master 通道）。
//
// 安装位置: $CORALNPU_HOME/hw_sim/coralnpu_trace.h（由 coralnpuint/install.sh 拷入）
//
// ---- tap 挂在哪一层 ----
//
// 挂 AxiMasterReadDriver / AxiMasterWriteDriver 的回调。这是 CoralNPU **核外**
// 的 AXI4 master 端口，也就是它访问外部 DDR 的唯一通路 —— TCM 命中不经过这里。
// 语义上等价于 host / Vortex 那两个 tap 的"LLC 之后"，三者因此可比。
//
// CoralNPU 侧不需要改一行 RTL：这两个回调本来就是参考实现挂 DDR 存储的地方。
//
// ---- 为什么不展开 burst ----
//
// 这一处是整个 CoralNPU tap 里最容易做错的判断，而且我原本打算做错。
//
// hw_primitives.h 的 AxiMasterWriteDriver::OnFallingEdge 只在 AWVALID 拉高的
// 那一拍锁存 axi_addr_，之后每一拍只要 WVALID 就带着**同一个**地址回调。看上去
// 就该按 addr_bits_len 展开成多拍。
//
// 但参考实现 core_mini_axi_simulator.cc 的两个回调都把每次调用当成一个
// 16 字节对齐窗口来处理：
//     aligned_offset = (addr - 0x80000000) & ~15
//     写: 对 i in [0,16)，strb 第 i 位为 1 则写 ddr[aligned_offset + i]
//     读: 恒定拷 16 字节，且恒置 read_data_bits_last = 1
// 也就是说：一次回调 = 一个 16 字节窗口，addr_bits_len 根本没被用到。读侧驱动
// 更是一次 handshake 只 push 一个 AxiRData，len>0 的 burst 读会直接欠应答。
//
// 结论：在这一层，一次回调就是一拍，展开 burst 会让 trace 的字节数远高于实际
// 搬运量。所以这里按"一次回调一条记录"记，字节数取 strb 的实际置位数。
//
// 代价是：如果 CoralNPU 哪天真发出 len>0 的 master burst，上面这套解读（以及
// 参考实现本身）就都错了。所以 tap 会计数 addr_bits_len != 0 的次数并写进
// .meta.json —— 宁可让它显式暴露，也不要静悄悄给出一份错的 trace。

#ifndef CORALNPU_GEM5_CORALNPU_TRACE_H_
#define CORALNPU_GEM5_CORALNPU_TRACE_H_

#include <cstdint>

// 引号形式而非 <>：bazel 只给 -iquote，不给 -I，所以 <hettrace/...> 解析不到。
// Vortex 那条腿是 Makefile + -I$(SRC_DIR)，两种写法都行，这里必须是引号。
#include "hettrace/addrmap.h"
#include "hettrace/record.h"
#include "hettrace/writer.h"

#include "hw_sim/hw_primitives.h"

namespace coralnpu_gem5 {

// AXI 数据总线宽度：VlWide<4> = 4 × 32 bit = 16 字节。strb 是 16 位，一位一字节。
constexpr uint32_t kAxiBeatBytes = 16;

// gem5 侧提供当前全局 tick 的回调。设备库被 dlopen 时不链接 gem5，
// 只能用回调把 curTick() 递进来。
typedef uint64_t (*TickProvider)(void* ctx);

class CoralNpuTraceTap {
  public:
    CoralNpuTraceTap() = default;

    // HETTRACE_DIR 未设置时返回 false 且什么都不做 —— 那是"用户没要 trace"，
    // 不是错误。
    bool Open(TickProvider tick_fn, void* tick_ctx, int64_t addr_offset) {
        if (!writer_.Open(hettrace::kSrcCoralnpu, "coralnpu",
                          hettrace::kLevelAxiMaster,
                          hettrace::kClockPeriodTicks_coralnpu,
                          /*axi_data_bytes=*/kAxiBeatBytes)) {
            return false;
        }
        tick_fn_     = tick_fn;
        tick_ctx_    = tick_ctx;
        addr_offset_ = addr_offset;
        return true;
    }

    void Close() { writer_.Close(); }

    bool is_open() const { return writer_.is_open(); }

    const hettrace::Stats& stats() const { return writer_.stats(); }

    // 见文件头：读侧恒为一个 16 字节对齐窗口。
    void OnRead(const AxiAddr& addr) {
        if (!writer_.is_open()) return;
        NoteBurstLen(addr);
        const uint64_t base = Relocate(addr.addr_bits_addr & ~(uint32_t)(kAxiBeatBytes - 1));
        writer_.Emit(Tick(), base, kAxiBeatBytes, hettrace::kRead,
                     addr.addr_bits_id);
    }

    // 写侧按 strb 的置位情况记。strb 通常是一段连续的位，但不保证 ——
    // 非连续时按每一段连续区间各记一条，这样每条记录的 (addr,size) 都是
    // 真实搬运的字节，不会把中间没写的洞算进带宽。
    void OnWrite(const AxiAddr& addr, const AxiWData& data) {
        if (!writer_.is_open()) return;
        NoteBurstLen(addr);
        const uint16_t strb = data.write_data_bits_strb;
        if (strb == 0) return;  // 这一拍没有字节真的落地

        const uint64_t base =
            Relocate(addr.addr_bits_addr & ~(uint32_t)(kAxiBeatBytes - 1));
        const uint64_t tick = Tick();

        uint32_t i = 0;
        while (i < kAxiBeatBytes) {
            if ((strb & (1u << i)) == 0) {
                ++i;
                continue;
            }
            const uint32_t run_start = i;
            while (i < kAxiBeatBytes && (strb & (1u << i)) != 0) ++i;
            writer_.Emit(tick, base + run_start, i - run_start,
                         hettrace::kWrite, addr.addr_bits_id);
        }
    }

    // 见文件头：非 0 说明核发了多拍 master burst，此时本 tap 与参考实现的
    // DDR 后端解读都不成立。计数供 .meta.json 与上层告警使用。
    uint64_t unexpected_burst_count() const { return unexpected_burst_; }

  private:
    uint64_t Tick() const {
        return (tick_fn_ != nullptr) ? tick_fn_(tick_ctx_) : 0;
    }

    uint64_t Relocate(uint32_t dev_addr) const {
        return static_cast<uint64_t>(static_cast<int64_t>(dev_addr) +
                                     addr_offset_);
    }

    void NoteBurstLen(const AxiAddr& addr) {
        if (addr.addr_bits_len != 0) ++unexpected_burst_;
    }

    hettrace::TraceWriter writer_;
    TickProvider          tick_fn_          = nullptr;
    void*                 tick_ctx_         = nullptr;
    int64_t               addr_offset_      = 0;
    uint64_t              unexpected_burst_ = 0;
};

}  // namespace coralnpu_gem5

#endif  // CORALNPU_GEM5_CORALNPU_TRACE_H_
