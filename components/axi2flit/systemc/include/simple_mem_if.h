/**
 * 存储侧整笔突发请求与响应类型。FIFO写入成功表示接受排队，响应表示操作完成。
 * 后端保持同方向、同资源平面、同ID的完成顺序；读响应包含完整突发的所有数据拍。
 */
#pragma once
#include "axi_if.h"
#include <array>
#include <vector>

struct SimpleMemWriteBeat {
    std::array<uint8_t, AXI_DATA_BYTES> data{};
    std::array<uint8_t, AXI_DATA_BYTES> strobe{}; // 每字节 0/非零，内存仅更新有效 lane
    uint16_t user = 0;
};
struct SimpleMemReadBeat {
    std::array<uint8_t, AXI_DATA_BYTES> data{};
    uint8_t resp = 0;                          // RRESP，每 beat 独立
    uint16_t user = 0;
};
struct SimpleMemRequest {
    bool write = false;
    uint8_t rp = 0;
    AxChannel address;
    std::vector<SimpleMemWriteBeat> write_beats;
    unsigned beats() const { return unsigned(address.len) + 1; }
};
struct SimpleMemResponse {
    bool write = false;
    uint8_t rp = 0;
    uint16_t id = 0;
    uint8_t resp = 0;                          // 写响应 BRESP；读使用 read_beats[].resp
    uint16_t user = 0;
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
