/**
 * @file aou_types.h
 * @brief AoU（AXI over UCIe）协议核心数据类型定义
 *
 * 本文件定义了 AoU Protocol Specification v0.8 中规定的所有消息类型、
 * 字段编码和 Flit 数据结构，是整个桥接模型的基础类型库。
 *
 * 关键概念：
 *   - 一个 Flit = 256B（UCIe Latency-Optimized Format 6）
 *   - 协议层使用其中 250B：10B 协议头 + 240B 载荷
 *   - 载荷分成 48 个 5B 粒度（granule），每条消息占整数个粒度
 *   - 消息类型由 MSGTYPE 字段（4bit）区分
 */

#pragma once

#include <systemc.h>
#include <array>
#include <cstdint>
#include <string>

// ============================================================
//  基本常量
// ============================================================

// UCIe Flit 总字节数（256B）
static constexpr int FLIT_TOTAL_BYTES   = 256;

// Link Layer Header + CRC 占用字节（6B），留给协议层 250B
static constexpr int FLIT_LINK_OVERHEAD = 6;

// 协议层 Protocol Header 字节数（10B）
static constexpr int PROTO_HEADER_BYTES = 10;

// 协议层载荷字节数：250 - 10 = 240B
static constexpr int PAYLOAD_BYTES      = 240;

// 粒度（granule）大小，5B
static constexpr int GRANULE_BYTES      = 5;

// 粒度总数：240 / 5 = 48
static constexpr int GRANULE_COUNT      = 48;

// ============================================================
//  消息类型编码（AoU Spec Table 1，MSGTYPE 4-bit 字段）
// ============================================================
enum class MsgType : uint8_t {
    Misc          = 0x0,   // 流控 / 接口管理（Misc）
    WriteReq      = 0x1,   // 写请求（对应 AXI AW 通道）
    ReadReq       = 0x2,   // 读请求（对应 AXI AR 通道）
    WriteData     = 0x3,   // 写数据（对应 AXI W 通道，含 strobe）
    ReadData      = 0x4,   // 读数据（对应 AXI R 通道）
    WriteResp     = 0x5,   // 写响应（对应 AXI B 通道）
    WriteDataFull = 0x6,   // 写数据（无 strobe，效率更高）
    Reserved      = 0xF
};

// 消息类型 → 可读字符串（调试用）
inline std::string msgtype_to_str(MsgType t) {
    switch (t) {
        case MsgType::Misc:          return "Misc";
        case MsgType::WriteReq:      return "WriteReq";
        case MsgType::ReadReq:       return "ReadReq";
        case MsgType::WriteData:     return "WriteData";
        case MsgType::ReadData:      return "ReadData";
        case MsgType::WriteResp:     return "WriteResp";
        case MsgType::WriteDataFull: return "WriteDataFull";
        default:                     return "Reserved";
    }
}

// ============================================================
//  数据长度选项（DLENGTH，WriteData / ReadData 消息专用）
// ============================================================
enum class DataLength : uint8_t {
    B256  = 0,   // 256-bit 数据，对应 8  granule
    B512  = 1,   // 512-bit 数据，对应 12 granule
    B1024 = 2    // 1024-bit 数据，对应 24 granule
};

// 将 DataLength 转换为对应的粒度数量
inline int dlength_to_granules(DataLength dl) {
    switch (dl) {
        case DataLength::B256:  return 8;
        case DataLength::B512:  return 12;
        case DataLength::B1024: return 24;
        default:                return 8;
    }
}

// ============================================================
//  各消息类型的粒度数（AoU Spec §5，Basic Profile）
// ============================================================
// WriteReq / ReadReq：4 granule = 20B
static constexpr int REQ_GRANULES       = 4;
// WriteResp：2 granule = 10B
static constexpr int WRESP_GRANULES     = 2;
// Misc：1 granule = 5B
static constexpr int MISC_GRANULES      = 1;

// ============================================================
//  AoU 消息结构体
//  采用"字节数组 + 元信息"的表示方式，方便后续直接填入 Flit 载荷
// ============================================================

// 消息最大字节数：24 granule × 5B = 120B（ReadData 1024b）
static constexpr int MSG_MAX_BYTES = GRANULE_COUNT * GRANULE_BYTES;

struct AouMessage {
    MsgType  type     = MsgType::Misc;     // 消息类型
    int      granules = 0;                 // 该消息占用的粒度数
    int      byte_len = 0;                 // 有效字节数（= granules × 5）
    uint8_t  data[MSG_MAX_BYTES] = {};     // 消息载荷（按粒度对齐填充）

    // 来自 AXI 的原始事务 ID，用于日志追踪（不放入 Flit，仅模型内部使用）
    uint32_t axi_id   = 0;
    uint64_t axi_addr = 0;
};

// SystemC sc_signal<AouMessage> 要求：相等比较、输出操作符
inline bool operator==(const AouMessage& a, const AouMessage& b) {
    return a.type == b.type && a.axi_id == b.axi_id && a.axi_addr == b.axi_addr;
}
inline bool operator!=(const AouMessage& a, const AouMessage& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const AouMessage& m) {
    os << "[AouMsg type=" << msgtype_to_str(m.type)
       << " granules=" << m.granules
       << " id=" << m.axi_id << "]";
    return os;
}

// sc_trace 必须定义在 sc_core 命名空间内，否则 sc_signal<AouMessage> 实例化时
// 编译器在 sc_core 内部做 unqualified lookup 时找不到该重载。
namespace sc_core {
inline void sc_trace(sc_trace_file* tf, const AouMessage& m, const std::string& nm) {
    sc_trace(tf, (uint8_t&)m.type, nm + ".type");
    sc_trace(tf, m.granules,       nm + ".granules");
    sc_trace(tf, m.axi_id,         nm + ".axi_id");
}
}  // namespace sc_core

// ============================================================
//  UCIe Flit 结构体
//  对应 UCIe Latency-Optimized 256B Flit Format 6
// ============================================================
struct AouFlit {
    // ---- Protocol Header（10B）----
    // FDId[1:0]：Flit 目的 ID，单桥场景固定为 0
    uint8_t  fdid        = 0;
    // MsgStart[47:0]：位图，标记 48 个 granule 中各自是否有新消息起始
    uint64_t msg_start   = 0;   // 使用 bit[47:0]，bit[i]=1 表示 granule i 起始新消息
    // MsgCredit[15:0]：随 flit 捎带回传的 credit 信息（发送方置 0，接收方回填）
    uint16_t msg_credit  = 0;

    // ---- Protocol Payload（240B，48 × 5B granule）----
    uint8_t  payload[PAYLOAD_BYTES] = {};

    // ---- 模型辅助字段（不对应 Flit 实际 bit）----
    int      used_granules = 0;   // 本 flit 已使用的粒度数
    bool     valid         = false; // 该 flit 是否包含有效数据

    // 将一条 AoU 消息追加写入载荷，并更新 msg_start 位图
    // 返回：追加成功返回 true，剩余空间不足返回 false
    bool pack_message(const AouMessage& msg) {
        if (used_granules + msg.granules > GRANULE_COUNT) return false;

        // 在当前 granule 起始位置设置 MsgStart bit
        msg_start |= (1ULL << used_granules);

        // 将消息字节写入载荷
        int offset = used_granules * GRANULE_BYTES;
        for (int i = 0; i < msg.byte_len; ++i) {
            payload[offset + i] = msg.data[i];
        }
        used_granules += msg.granules;
        valid = true;
        return true;
    }

    // 查询剩余可用粒度数
    int remaining_granules() const {
        return GRANULE_COUNT - used_granules;
    }

    // 清空 flit，准备重新填充
    void clear() {
        fdid          = 0;
        msg_start     = 0;
        msg_credit    = 0;
        used_granules = 0;
        valid         = false;
        std::fill(std::begin(payload), std::end(payload), 0);
    }
};

// ============================================================
//  FDI 接口上的 Flit 传输信号打包结构（用于 SystemC 端口传递）
// ============================================================
struct FlitTransfer {
    bool    valid = false;    // 本拍是否有有效 flit
    AouFlit flit;             // flit 内容

    FlitTransfer() = default;
    explicit FlitTransfer(const AouFlit& f) : valid(true), flit(f) {}
};

// SystemC 要求可赋值、可比较、可打印，为自定义类型提供这些操作
inline bool operator==(const FlitTransfer& a, const FlitTransfer& b) {
    // 仅比较 valid 和 used_granules，用于信号变化检测
    return (a.valid == b.valid) &&
           (a.flit.msg_start == b.flit.msg_start) &&
           (a.flit.used_granules == b.flit.used_granules);
}
inline bool operator!=(const FlitTransfer& a, const FlitTransfer& b) {
    return !(a == b);
}
inline std::ostream& operator<<(std::ostream& os, const FlitTransfer& ft) {
    if (ft.valid)
        os << "[Flit valid, granules=" << ft.flit.used_granules
           << ", msgstart=0x" << std::hex << ft.flit.msg_start << std::dec << "]";
    else
        os << "[Flit invalid]";
    return os;
}

// FlitTransfer 需要 sc_trace 支持才能输出 VCD
// 注意：sc_trace 的重载必须定义在 sc_core 命名空间内，
//       才能被 sc_signal<T> 内部的追踪机制正确找到。
namespace sc_core {
inline void sc_trace(sc_trace_file* tf, const FlitTransfer& ft, const std::string& name) {
    sc_trace(tf, ft.valid,              name + ".valid");
    sc_trace(tf, ft.flit.used_granules, name + ".used_granules");
    sc_trace(tf, ft.flit.msg_start,     name + ".msg_start");
    sc_trace(tf, ft.flit.fdid,          name + ".fdid");
}
}  // namespace sc_core
