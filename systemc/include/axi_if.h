/**
 * @file axi_if.h
 * @brief AXI4 接口信号定义
 *
 * 定义 AXI4 协议的五个通道信号结构体，用于 SystemC 模块间传递。
 * 本模型只关注桥接所需的字段，忽略部分扩展字段（如 AxREGION 等）。
 *
 * AXI4 五通道：
 *   - AW（写地址通道）：传送写请求地址和控制信息
 *   - W （写数据通道）：传送写数据和字节选通
 *   - B （写响应通道）：返回写事务完成状态
 *   - AR（读地址通道）：传送读请求地址和控制信息
 *   - R （读数据通道）：返回读数据和状态
 *
 * 握手协议：valid/ready 双向握手，valid 由发送方驱动，ready 由接收方驱动。
 * 当 valid && ready 同拍为高时，完成一次 beat 传输。
 */

#pragma once

#include <systemc.h>
#include <cstdint>
#include <string>

// ============================================================
//  AXI 字段宽度常量
// ============================================================
static constexpr int AXI_ID_WIDTH   = 8;    // AXID 位宽，支持最多 256 个 outstanding
static constexpr int AXI_ADDR_WIDTH = 64;   // 地址位宽（字节地址）
static constexpr int AXI_DATA_WIDTH = 256;  // 数据位宽（与 AoU 256b DLENGTH 对应）
static constexpr int AXI_STRB_WIDTH = AXI_DATA_WIDTH / 8;  // strobe 位宽 = 32B

// ============================================================
//  AW / AR 通道（写/读地址通道）
// ============================================================
struct AxChannel {
    // ------ 有效/就绪握手 ------
    bool     valid   = false;  // 发送方驱动：本拍请求有效
    bool     ready   = false;  // 接收方驱动：本拍可以接收

    // ------ 地址与控制字段 ------
    uint8_t  id      = 0;     // AXID：事务标识（追踪 outstanding 事务）
    uint64_t addr    = 0;     // AXADDR：目标字节地址
    uint8_t  len     = 0;     // AXLEN：burst 长度 - 1（即共 len+1 个 beat）
    uint8_t  size    = 5;     // AXSIZE：每个 beat 字节数的 log2（5=32B，对应 256-bit）
    uint8_t  burst   = 1;     // AXBURST：burst 类型，AoU 只支持 INCR（=1）
    uint8_t  lock    = 0;     // AXLOCK：原子操作类型（0=正常，1=独占）
    uint8_t  cache   = 0;     // AXCACHE：内存属性
    uint8_t  prot    = 0;     // AXPROT：保护属性
    uint8_t  qos     = 0;     // AXQOS：服务质量
    uint16_t user    = 0;     // AXUSER：用户自定义字段（映射到 AoU FLEX 字段）
};

// ============================================================
//  W 通道（写数据通道）
// ============================================================
struct WChannel {
    bool     valid          = false;          // 本拍写数据有效
    bool     ready          = false;          // 接收方就绪

    uint8_t  data[AXI_STRB_WIDTH] = {};       // WDATA：32B 数据（对应 256-bit）
    uint8_t  strb[AXI_STRB_WIDTH] = {};       // WSTRB：字节选通，每位控制一字节
    bool     last            = false;         // WLAST：burst 最后一个 beat 标志
    uint16_t user            = 0;             // WUSER
};

// ============================================================
//  B 通道（写响应通道）
// ============================================================
struct BChannel {
    bool    valid  = false;  // 写响应有效
    bool    ready  = false;  // 主机就绪（可接收响应）

    uint8_t id     = 0;      // BID：对应 AWID
    uint8_t resp   = 0;      // BRESP：0=OKAY, 1=EXOKAY, 2=SLVERR, 3=DECERR
    uint16_t user  = 0;      // BUSER
};

// ============================================================
//  R 通道（读数据通道）
// ============================================================
struct RChannel {
    bool     valid          = false;   // 读数据有效
    bool     ready          = false;   // 主机就绪

    uint8_t  id             = 0;       // RID：对应 ARID
    uint8_t  data[AXI_STRB_WIDTH] = {}; // RDATA：32B 读数据
    uint8_t  resp           = 0;       // RRESP
    bool     last           = false;   // RLAST：burst 最后一拍
    uint16_t user           = 0;       // RUSER
};

// ============================================================
//  AXI4 完整接口打包（供 SystemC sc_signal 传递）
// ============================================================
struct Axi4Beat {
    // 通道标识（用于区分 sc_signal 携带的是哪个通道的 beat）
    enum class Ch { AW, W, B, AR, R } channel = Ch::AW;

    AxChannel aw;  // 写地址通道
    WChannel  w;   // 写数据通道
    BChannel  b;   // 写响应通道
    AxChannel ar;  // 读地址通道
    RChannel  r;   // 读数据通道
};

// ============================================================
//  SystemC 信号要求的比较与打印操作符
// ============================================================
inline bool operator==(const AxChannel& a, const AxChannel& b) {
    return a.valid == b.valid && a.id == b.id && a.addr == b.addr && a.len == b.len;
}
inline bool operator!=(const AxChannel& a, const AxChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const AxChannel& ax) {
    os << "[AxCh valid=" << ax.valid << " id=" << (int)ax.id
       << " addr=0x" << std::hex << ax.addr << std::dec
       << " len=" << (int)ax.len << "]";
    return os;
}

inline bool operator==(const WChannel& a, const WChannel& b) {
    // 除 valid/last 外，还比较 data[0] 和 strb[0]，确保数据内容变化时
    // sc_signal<WChannel> 能正确检测到变化并更新缓存值，避免 strb 错误传递。
    return a.valid == b.valid && a.last == b.last &&
           a.data[0] == b.data[0] && a.strb[0] == b.strb[0];
}
inline bool operator!=(const WChannel& a, const WChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const WChannel& w) {
    os << "[WCh valid=" << w.valid << " last=" << w.last << "]";
    return os;
}

inline bool operator==(const BChannel& a, const BChannel& b) {
    return a.valid == b.valid && a.id == b.id && a.resp == b.resp;
}
inline bool operator!=(const BChannel& a, const BChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const BChannel& b) {
    os << "[BCh valid=" << b.valid << " id=" << (int)b.id << " resp=" << (int)b.resp << "]";
    return os;
}

inline bool operator==(const RChannel& a, const RChannel& b) {
    return a.valid == b.valid && a.id == b.id && a.last == b.last;
}
inline bool operator!=(const RChannel& a, const RChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const RChannel& r) {
    os << "[RCh valid=" << r.valid << " id=" << (int)r.id << " last=" << r.last << "]";
    return os;
}

// sc_trace 支持（输出 VCD 波形）
// 必须定义在 sc_core 命名空间内，才能被 sc_signal<T> 内部的查找机制找到。
namespace sc_core {
inline void sc_trace(sc_trace_file* tf, const AxChannel& ax, const std::string& nm) {
    sc_trace(tf, ax.valid, nm + ".valid");
    sc_trace(tf, ax.ready, nm + ".ready");
    sc_trace(tf, ax.id,    nm + ".id");
    sc_trace(tf, ax.addr,  nm + ".addr");
    sc_trace(tf, ax.len,   nm + ".len");
}
inline void sc_trace(sc_trace_file* tf, const WChannel& w, const std::string& nm) {
    sc_trace(tf, w.valid, nm + ".valid");
    sc_trace(tf, w.ready, nm + ".ready");
    sc_trace(tf, w.last,  nm + ".last");
}
inline void sc_trace(sc_trace_file* tf, const BChannel& b, const std::string& nm) {
    sc_trace(tf, b.valid, nm + ".valid");
    sc_trace(tf, b.ready, nm + ".ready");
    sc_trace(tf, b.id,    nm + ".id");
    sc_trace(tf, b.resp,  nm + ".resp");
}
inline void sc_trace(sc_trace_file* tf, const RChannel& r, const std::string& nm) {
    sc_trace(tf, r.valid, nm + ".valid");
    sc_trace(tf, r.ready, nm + ".ready");
    sc_trace(tf, r.id,    nm + ".id");
    sc_trace(tf, r.last,  nm + ".last");
}
}  // namespace sc_core
