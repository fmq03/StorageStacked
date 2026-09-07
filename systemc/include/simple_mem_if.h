/**
 * @file simple_mem_if.h
 * @brief 下一阶段存储侧使用的整 burst 事务接口，不含 HBM/DFI 引脚时序。
 *
 * AoU 解包端先按 RP 配对 WREQ/WDATA，收齐写 burst 后才提交 Request；读取
 * Request 没有 write_beats。每个数组保留完整 AXI lane 顺序，窄写只有对应
 * strobe 有效。AoU WDATA 不携带 WLAST，解包端根据 beats 重建事务边界。
 * Response 由 id/rp 配对，读响应数组长度等于请求 beats；发送端最后一项
 * 生成 RLAST。该类型冻结接口，不代表存储模型或 outstanding 管理已实现。
 */
#pragma once
#include "axi_if.h"
#include <array>
#include <vector>

struct SimpleMemWriteBeat {
    std::array<uint8_t, AXI_DATA_BYTES> data{};
    std::array<uint8_t, AXI_DATA_BYTES> strobe{}; // 每字节 0/非零，内存仅更新有效 lane
    uint16_t user = 0;                         // WUSER/FLEX
};
struct SimpleMemReadBeat {
    std::array<uint8_t, AXI_DATA_BYTES> data{};
    uint8_t resp = 0;                          // RRESP，每 beat 独立
    uint16_t user = 0;                         // RUSER/FLEX
};
struct SimpleMemRequest {
    bool write = false;
    uint8_t rp = 0;
    AxChannel address;                        // id/addr/len/size/prot/cache/qos/lock/user
    std::vector<SimpleMemWriteBeat> write_beats;
    unsigned beats() const { return unsigned(address.len) + 1; }
};
struct SimpleMemResponse {
    bool write = false;
    uint8_t rp = 0;
    uint16_t id = 0;
    uint8_t resp = 0;                          // 写响应 BRESP；读使用 read_beats[].resp
    uint16_t user = 0;                         // BUSER
    std::vector<SimpleMemReadBeat> read_beats;  // 写响应为空，读响应恰好请求 beats 项
};

// sc_fifo 的诊断输出支持；事务有效性由 FIFO 成功读写表达，不使用 address.valid。
inline std::ostream& operator<<(std::ostream& os, const SimpleMemRequest& r) {
    return os << "MemReq{write=" << r.write << ",rp=" << unsigned(r.rp)
              << ",id=" << r.address.id << ",beats=" << r.beats() << "}";
}
inline std::ostream& operator<<(std::ostream& os, const SimpleMemResponse& r) {
    return os << "MemResp{write=" << r.write << ",rp=" << unsigned(r.rp)
              << ",id=" << r.id << ",beats=" << r.read_beats.size() << "}";
}
