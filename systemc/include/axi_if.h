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
#include <cstring>
#include <string>

// ============================================================
//  AXI 字段宽度常量
// ============================================================
// AXID 位宽 = 10。AoU 的请求/响应消息中 ID 字段固定为 10bit
// （参考 RTL st_write_req_packet.awid[9:0]），因此桥接单元的 AXI 侧
// ID 位宽最大只能是 10；超过 10bit 的 ID 需要在桥外做压缩/重映射。
static constexpr int AXI_ID_WIDTH   = 10;
static constexpr uint32_t AXI_ID_MASK = (1u << AXI_ID_WIDTH) - 1;   // 0x3FF

static constexpr int AXI_ADDR_WIDTH = 64;   // 地址位宽（字节地址）

// ------------------------------------------------------------
//  AXI 数据位宽（P1-8：编译期可配置）
//
//  AoU 的 DLENGTH 字段只有 256b / 512b / 1024b 三档，所以 AXI 侧数据位宽
//  必须与其中之一对齐，一个 AXI beat 恰好对应一条 WriteData/ReadData 消息。
//
//  用法：编译时加 -DAXI_DATA_WIDTH_CFG=1024 即可切换到 1024b 配置，
//  msg_builder / msg_decoder / testbench 会自动跟随，无需改任何代码。
//
//  为什么带宽实验必须用 1024b：
//    @500MHz 时 AXI 侧吞吐 = AXI_DATA_WIDTH/8 / 2ns
//      256b  → 16 GB/s   < 链路 48 GB/s  ⇒ 瓶颈在 AXI，测不出链路利用率
//      1024b → 64 GB/s   > 链路 48 GB/s  ⇒ 瓶颈在链路，带宽利用率才有意义
//    同时 1024b 的协议开销也最小（WriteDataFull1024 = 128B 数据 / 27 granule）。
// ------------------------------------------------------------
#ifndef AXI_DATA_WIDTH_CFG
#define AXI_DATA_WIDTH_CFG 256
#endif
static constexpr int AXI_DATA_WIDTH = AXI_DATA_WIDTH_CFG;
static_assert(AXI_DATA_WIDTH == 256 || AXI_DATA_WIDTH == 512 || AXI_DATA_WIDTH == 1024,
              "AXI_DATA_WIDTH 必须是 256/512/1024 之一，以对齐 AoU 的 DLENGTH 编码");

// 一个 beat 的数据字节数：32 / 64 / 128
static constexpr int AXI_DATA_BYTES = AXI_DATA_WIDTH / 8;

// WSTRB 在本模型中用"每个数据字节一个数组元素"表示（0 = 屏蔽，非 0 = 有效），
// 便于 testbench 直接按字节设置；打包到线上时再压缩成每字节 8 个 strobe 位。
static constexpr int AXI_STRB_WIDTH = AXI_DATA_BYTES;        // 数组元素个数
static constexpr int AXI_STRB_BYTES = AXI_DATA_BYTES / 8;    // 线上 WSTRB 字节数 4/8/16

// AxSIZE 编码 = log2(每 beat 字节数)：32B→5，64B→6，128B→7
static constexpr uint8_t AXI_SIZE_CODE =
    (AXI_DATA_WIDTH == 256) ? 5 : ((AXI_DATA_WIDTH == 512) ? 6 : 7);

// AXUSER 位宽 = 16。AoU v0.8 把原先的 PROF[11:0] + PROFEXTLEN[3:0] 合并
// 重定义为 FLEX[15:0]（v0.8 变更记录：“Repurposed PROF/PROFEXTLEN fields as
// FLEX fields, defined use of FLEX fields as USER bits in the Basic Profile”），
// 即 Basic Profile 下 FLEX[15:0] 就是 xUSER[15:0]。所以桥接单元的 AXI 侧
// USER 位宽是 16，超出部分无法跨链路传递。
//
// 注：参考 RTL（tt-oca-harness-aou）仍是 PROF/PROFEXTLEN 的旧划分，属 v0.8
// 之前的版本，本模型以 PDF 规范为准。
static constexpr int AXI_USER_WIDTH = 16;
static constexpr uint32_t AXI_USER_MASK = 0xFFFFu;

// ============================================================
//  AW / AR 通道（写/读地址通道）
// ============================================================
struct AxChannel {
    // ------ 有效/就绪握手 ------
    bool     valid   = false;  // 发送方驱动：本拍请求有效
    bool     ready   = false;  // 接收方驱动：本拍可以接收

    // ------ 地址与控制字段 ------
    uint16_t id      = 0;     // AXID[9:0]：事务标识（追踪 outstanding 事务）
    uint64_t addr    = 0;     // AXADDR：目标字节地址
    uint8_t  len     = 0;     // AXLEN：burst 长度 - 1（即共 len+1 个 beat）
    uint8_t  size    = AXI_SIZE_CODE;  // AXSIZE：每 beat 字节数 log2，随 AXI_DATA_WIDTH 变化
    uint8_t  burst   = 1;     // AXBURST：burst 类型，AoU 只支持 INCR（=1）
    uint8_t  lock    = 0;     // AXLOCK：原子操作类型（0=正常，1=独占）
    uint8_t  cache   = 0;     // AXCACHE：内存属性
    uint8_t  prot    = 0;     // AXPROT：保护属性
    uint8_t  qos     = 0;     // AXQOS：服务质量
    uint16_t user    = 0;     // AXUSER[15:0]：用户自定义字段（映射到 AoU FLEX[15:0]）
};

// ============================================================
//  W 通道（写数据通道）
// ============================================================
struct WChannel {
    bool     valid          = false;          // 本拍写数据有效
    bool     ready          = false;          // 接收方就绪

    uint8_t  data[AXI_DATA_BYTES] = {};       // WDATA：AXI_DATA_BYTES 字节数据
    uint8_t  strb[AXI_STRB_WIDTH] = {};       // WSTRB：每个数据字节一个元素（0=屏蔽）
    bool     last            = false;         // WLAST：burst 最后一个 beat 标志
    uint16_t user            = 0;             // WUSER[15:0] → FLEX[15:0]
};

// ============================================================
//  B 通道（写响应通道）
// ============================================================
struct BChannel {
    bool    valid  = false;  // 写响应有效
    bool    ready  = false;  // 主机就绪（可接收响应）

    uint16_t id    = 0;      // BID[9:0]：对应 AWID
    uint8_t resp   = 0;      // BRESP：0=OKAY, 1=EXOKAY, 2=SLVERR, 3=DECERR
    uint16_t user  = 0;      // BUSER[15:0] → FLEX[15:0]
};

// ============================================================
//  R 通道（读数据通道）
// ============================================================
struct RChannel {
    bool     valid          = false;   // 读数据有效
    bool     ready          = false;   // 主机就绪

    uint16_t id             = 0;       // RID[9:0]：对应 ARID
    uint8_t  data[AXI_DATA_BYTES] = {}; // RDATA：AXI_DATA_BYTES 字节读数据
    uint8_t  resp           = 0;       // RRESP
    bool     last           = false;   // RLAST：burst 最后一拍
    uint16_t user           = 0;       // RUSER[15:0] → FLEX[15:0]
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
    return a.valid == b.valid && a.ready == b.ready && a.id == b.id &&
           a.addr == b.addr && a.len == b.len && a.size == b.size &&
           a.burst == b.burst && a.lock == b.lock && a.cache == b.cache &&
           a.prot == b.prot && a.qos == b.qos && a.user == b.user;
}
inline bool operator!=(const AxChannel& a, const AxChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const AxChannel& ax) {
    os << "[AxCh valid=" << ax.valid << " id=" << (int)ax.id
       << " addr=0x" << std::hex << ax.addr << std::dec
       << " len=" << (int)ax.len << "]";
    return os;
}

inline bool operator==(const WChannel& a, const WChannel& b) {
    // 必须比较完整数据。仅比较 data[0] 会使首字节相同、其余字节不同的连续
    // beat 被 sc_signal 误认为没有变化。
    return a.valid == b.valid && a.ready == b.ready && a.last == b.last &&
           a.user == b.user &&
           std::memcmp(a.data, b.data, AXI_DATA_BYTES) == 0 &&
           std::memcmp(a.strb, b.strb, AXI_STRB_WIDTH) == 0;
}
inline bool operator!=(const WChannel& a, const WChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const WChannel& w) {
    os << "[WCh valid=" << w.valid << " last=" << w.last << "]";
    return os;
}

inline bool operator==(const BChannel& a, const BChannel& b) {
    return a.valid == b.valid && a.ready == b.ready && a.id == b.id &&
           a.resp == b.resp && a.user == b.user;
}
inline bool operator!=(const BChannel& a, const BChannel& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const BChannel& b) {
    os << "[BCh valid=" << b.valid << " id=" << (int)b.id << " resp=" << (int)b.resp << "]";
    return os;
}

inline bool operator==(const RChannel& a, const RChannel& b) {
    return a.valid == b.valid && a.ready == b.ready && a.id == b.id &&
           a.resp == b.resp && a.last == b.last && a.user == b.user &&
           std::memcmp(a.data, b.data, AXI_DATA_BYTES) == 0;
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
