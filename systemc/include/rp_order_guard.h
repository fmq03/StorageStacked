/**
 * @file rp_order_guard.h
 * @brief 「同一 AXI ID 只能映射到固定 RP」的约束检查器
 *
 * ============================================================
 *  为什么需要这个东西
 * ============================================================
 * AXI4 对顺序的要求是**按 ID、按方向**的：同一个 AxID 上的多笔事务，
 * 响应（B / R）必须按请求发出的顺序返回；不同 ID 之间则可以任意乱序。
 *
 * 而 AoU 的 Resource Plane 是**互相独立**的流控平面：
 *   - 每个 RP 有自己的 credit 计数与自己的接收 FIFO；
 *   - 本模型收响应时在各 RP 之间做 round-robin 仲裁；
 *   - 对端（HBM 控制器侧）也完全可以让不同 RP 以不同速率推进。
 * 因此**两条落在不同 RP 上的事务，响应顺序是不确定的**。
 *
 * 本模型的 RP 映射是 `AxQOS % RP_COUNT`。把这两件事放到一起看，
 * 就出现了一个真实的顺序风险：
 *
 *      同一个 AxID、不同的 AxQOS  →  落到不同 RP  →  响应可能乱序
 *                                  →  违反 AXI4 的同 ID 顺序要求
 *
 * 举例（RP_COUNT = 2）：
 *      AR: id=5, qos=0  → RP0
 *      AR: id=5, qos=1  → RP1        ← 同一个 ID 劈到了两个平面
 *   若 RP1 的读数据先回来，主设备看到的就是乱序的 id=5 响应。
 *
 * ============================================================
 *  为什么用「约束 + 断言」而不是「跨 RP 记分板」
 * ============================================================
 * 理论上可以在桥内加一个跨 RP 的重排序缓冲：给每笔事务打序号，响应
 * 到齐后再按序送上 AXI。但代价很大且不划算：
 *   - 需要按 ID 保存乱序到达的整条 burst，缓冲面积随 outstanding 深度线性增长；
 *   - 会把「快 RP」的延迟拖慢到「慢 RP」的水平，等于抵消了分平面的意义；
 *   - AoU 规范本身并没有要求桥做这件事。
 *
 * 所以这里采取业界常规做法：**把它定为集成约束，并在模型里断言**。
 *
 *   【集成约束 C-1】同一个 AXI ID 在同一方向上未完成的事务，必须映射到
 *   同一个 RP。等价的充分条件（任选其一即可，由 SoC 侧保证）：
 *     a) RP_COUNT = 1（本项目默认配置，天然满足）；
 *     b) 同一个 AxID 上的所有事务使用同一个 AxQOS 值；
 *     c) ID 空间按 RP 静态划分，例如 AxID[9:8] 直接作为 RP 号。
 *
 * 违反时本类不会去"修正"数据流——那样会掩盖问题；它只计一次违例并
 * 打印现场（ID、原 RP、新 RP），让问题在仿真阶段就暴露出来。
 *
 * ============================================================
 *  读写为什么分开两张表
 * ============================================================
 * AXI4 里读和写是两条独立的顺序域：ARID=3 与 AWID=3 之间没有任何顺序
 * 关系。合用一张表会把「读 id=3 在 RP0、写 id=3 在 RP1」误报成违例。
 */

#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#include "axi_if.h"

class RpOrderGuard {
public:
    explicit RpOrderGuard(std::string dir_name = "") : dir_(std::move(dir_name)) {}

    void reset() {
        table_.fill(Entry{});
        violations_ = 0;
    }

    /**
     * 请求上车时调用（AW 握手 / AR 握手）。
     * @return true 表示合规；false 表示这笔事务违反了约束 C-1。
     *
     * 判据是「未完成事务」而不是「历史上用过的 RP」：一个 ID 的事务全部
     * 完成之后，它当然可以改用另一个 RP —— 此时已经不存在顺序歧义。
     */
    bool bind(uint16_t id, uint8_t rp) {
        Entry& e = table_[id & AXI_ID_MASK];
        if (e.outstanding != 0 && e.rp != rp) {
            ++violations_;
            std::cout << "[RpOrderGuard]" << (dir_.empty() ? "" : " " + dir_)
                      << " 违反约束 C-1：AXI ID 0x" << std::hex << (id & AXI_ID_MASK)
                      << std::dec << " 已有 " << e.outstanding
                      << " 笔未完成事务挂在 RP" << (int)e.rp
                      << "，新事务却被映射到 RP" << (int)rp
                      << " —— 同 ID 响应顺序无法保证。" << std::endl;
            // 不改写绑定：保留最早的 RP，后续同 ID 事务只会重复报同一个冲突，
            // 便于定位第一个出错点，而不是每来一笔都翻转绑定、刷屏。
            ++e.outstanding;
            return false;
        }
        e.rp = rp;
        ++e.outstanding;
        return true;
    }

    /// 响应完成时调用（B beat 收下 / R 通道的 RLAST 收下）
    void retire(uint16_t id) {
        Entry& e = table_[id & AXI_ID_MASK];
        if (e.outstanding) --e.outstanding;
    }

    unsigned long violations() const { return violations_; }

private:
    struct Entry {
        uint8_t  rp          = 0;
        unsigned outstanding = 0;
    };
    std::string dir_;
    std::array<Entry, AXI_ID_MASK + 1> table_{};
    unsigned long violations_ = 0;
};
