/**
 * 跟踪同方向同ID的未完成事务。事务未完成期间，ID必须固定到同一资源平面。
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

    bool bind(uint16_t id, uint8_t rp) {
        Entry& e = table_[id & AXI_ID_MASK];
        if (e.outstanding != 0 && e.rp != rp) {
            ++violations_;
            std::cout << "[RpOrderGuard]" << (dir_.empty() ? "" : " " + dir_)
                      << " 违反资源平面顺序约束：AXI ID 0x" << std::hex << (id & AXI_ID_MASK)
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
