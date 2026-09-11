// CoralNPU 侧的共享 buffer 内核。
//
// 安装位置: $CORALNPU_HOME/gem5int/ddr_touch.cc（由 coralnpuint/install.sh 拷入）
//
// 两个用途，一份代码：
//   1. AXI master tap 的验证载荷。tests/cocotb 里那些内核（wfi_slot_0 等）全在
//      TCM 里跑完，一次外部访存都没有，跑它们 tap 只会输出 0 条记录 —— 看着
//      "通过"，其实什么都没测到。这个内核强制走外部 DDR。
//   2. 异构协同负载里 NPU 那一段：host 写 in[]，NPU 读 in[] 算完写 out[]，
//      host 再读回来核对。host/NPU 共用 addrmap.json 里的这段
//      物理地址；Vortex 与 host 的交接另走 4GiB 以上的 BAR。
//
// 地址全部取自 addrmap.json，那是整个工程的地址真值源，别在这里另起一套。
//
// shared_buffer (0x90000000) 落在 CoralNPU 的 DDR 窗口 [0x80000000,0xc0000000)
// 里，所以访问它会走 AXI master 出核，才会被 tap 看到；TCM 命中不出核，永远
// 不在 trace 里 —— 这与 host 侧只记 LLC 之后、Vortex 侧只记 post-LLC 是同一
// 条口径。
//
// npu_mailbox (0xc0000000) 恰好是 DDR 窗口的**上界**，即窗口之外。落到窗口外的
// master 访问被 RTL 路由到 4 × 32bit 的 mailbox 寄存器 —— addrmap.json 把
// mailbox 放在 0xc0000000 不是巧合，就是为了让"窗口外"这个条件自然成立。
// tap 有意不记 mailbox：那是控制面，不是访存流量。

#include <cstdint>

namespace {

// addrmap.json: regions.shared_buffer，accessors = host / coralnpu。
// in / out 各 4 KiB 且不重叠，这样归并 trace 时能靠地址区间区分谁读谁写。
volatile uint32_t* const kIn  = reinterpret_cast<volatile uint32_t*>(0x90000000u);
volatile uint32_t* const kOut = reinterpret_cast<volatile uint32_t*>(0x90001000u);

// addrmap.json: regions.npu_mailbox。message[0] 用作完成标志 + 校验和。
volatile uint32_t* const kMailbox =
    reinterpret_cast<volatile uint32_t*>(0xc0000000u);

constexpr uint32_t kWords = 64;

// 完成标志的高 16 位。低 16 位放校验和，host 侧据此确认 NPU 真的读到了
// host 写进去的数据 —— 如果内存后端没接通，NPU 读到的是零，校验和也就是 0。
constexpr uint32_t kDoneTag = 0x600du;

}  // namespace

int main() {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < kWords; ++i) {
        // volatile 是必需的：否则编译器完全可以把这一整个循环消掉，
        // 或者把 64 次访问合并成向量访问，那样 trace 记的就不是我们要的东西。
        const uint32_t v = kIn[i];
        sum += v;
        kOut[i] = v * 2u + 1u;
    }

    kMailbox[0] = (kDoneTag << 16) | (sum & 0xffffu);

    // 交还控制权。gem5 侧看到 wfi 就停止调度 tick 事件。
    asm volatile("wfi");
    return 0;
}
