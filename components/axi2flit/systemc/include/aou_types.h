/**
 * 桥接模型共用的数据类型、消息编码、粒度常量和容量计算。
 * 帧长256字节，协议内容250字节，载荷由48个5字节粒度构成。
 */
#pragma once

#include <systemc.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include "link_config.h"

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

// 本模型的 RP 字段宽度为 2bit，因此协议最多支持 4 个 Resource Plane。
// 具体工程可以只启用其中一部分；本模型默认只启用 RP0。
static constexpr unsigned MAX_RESOURCE_PLANES     = 4;
static constexpr unsigned DEFAULT_RESOURCE_PLANES = 1;

// ============================================================
//  链路参数（用于 FIFO/credit 容量反推，以及 testbench 的速率节流）
// ============================================================
// 默认按 16 条通道、每通道 24GT/s 计算聚合裸带宽。
// 裸链路字节速率（B/ns）：16 lane × 24 Gbps ÷ 8 = 48 GB/s = 48 B/ns
// 参数和 NRZ/PAM4 位数统一定义在 link_config.h；上式是默认 NRZ 配置。
// 一个 256B Flit 在链路上占用的时间（ns）：256 / 48 ≈ 5.333ns
static constexpr double FLIT_PERIOD_NS     = FLIT_TOTAL_BYTES / LINK_BYTES_PER_NS;

// credit回传的排队、编码与发送等待按三个帧周期计入容量预算。
static constexpr double CREDIT_RETURN_OVERHEAD_FLITS = 3.0;
static constexpr double CREDIT_LOOP_NS =
    LINK_TAT_NS + CREDIT_RETURN_OVERHEAD_FLITS * FLIT_PERIOD_NS;

// 深度反推的固定余量（条），用于吸收流水线级数与仲裁抖动
static constexpr unsigned FIFO_MARGIN_ENTRIES = 4;

// ============================================================
//  全局日志开关
// ============================================================
// 功能仿真需要逐条打印消息/Flit 便于人工核对；性能仿真会跑几万个周期，
// 打印会淹没输出也拖慢仿真，因此由 testbench 统一关闭。
inline bool g_aou_verbose = true;

enum class MsgType : uint8_t {
    Misc          = 0x0,   // 流控 / 接口管理（Misc）
    WriteReq      = 0x1,   // 写请求（对应 AXI AW 通道）
    ReadReq       = 0x2,   // 读请求（对应 AXI AR 通道）
    WriteData     = 0x3,   // 写数据（对应 AXI W 通道，含 strobe）
    ReadData      = 0x4,   // 读数据（对应 AXI R 通道）
    WriteResp     = 0x5,   // 写响应（对应 AXI B 通道）
    WriteDataFull = 0x6,   // 写数据（全 strobe，省掉 WSTRB 字段，效率更高）
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

// Misc 消息的子操作码（MISCOP，3bit）
static constexpr uint8_t MISCOP_CRDT_GRANT = 0b100;  // 专用 credit 授予
static constexpr uint8_t MISCOP_ACTIVATION = 0b000;  // 接口激活（本模型未实现）

// ============================================================
//  数据长度选项（DLENGTH，WriteData / ReadData 消息专用）
// ============================================================
enum class DataLength : uint8_t {
    B256  = 0,   // 256-bit  = 32B  数据
    B512  = 1,   // 512-bit  = 64B  数据
    B1024 = 2    // 1024-bit = 128B 数据
};

// DLENGTH → 单条消息承载的应用数据字节数
constexpr int dlength_to_bytes(DataLength dl) {
    switch (dl) {
        case DataLength::B256:  return 32;
        case DataLength::B512:  return 64;
        case DataLength::B1024: return 128;
        default:                return 32;
    }
}

static constexpr int WREQ_GRANULES   = 3;
static constexpr int RREQ_GRANULES   = 3;
static constexpr int WRESP_GRANULES  = 1;

// Misc 消息长度随 MISCOP 变化
static constexpr int MISC_ACTIVATION_GRANULES = 1;
static constexpr int MISC_CRDTGRANT_GRANULES  = 2;

// 带字节选通的写数据消息，按总线宽度确定粒度数。
constexpr int wdata_granules(DataLength dl) {
    switch (dl) {
        case DataLength::B256:  return 8;
        case DataLength::B512:  return 15;
        case DataLength::B1024: return 30;
        default:                return 8;
    }
}

// 全字节有效的写数据消息，省去选通位图。
constexpr int wdatafull_granules(DataLength dl) {
    switch (dl) {
        case DataLength::B256:  return 7;
        case DataLength::B512:  return 14;
        case DataLength::B1024: return 27;
        default:                return 7;
    }
}

constexpr int rdata_granules(DataLength dl) {
    switch (dl) {
        case DataLength::B256:  return 8;
        case DataLength::B512:  return 14;
        case DataLength::B1024: return 27;
        default:                return 8;
    }
}

constexpr MsgType    msg_type_of(uint8_t b0)    { return static_cast<MsgType>((b0 >> 4) & 0xF); }
constexpr uint8_t    msg_rp_of(uint8_t b0)      { return static_cast<uint8_t>((b0 >> 2) & 0x3); }
constexpr DataLength msg_dlength_of(uint8_t b0) { return static_cast<DataLength>(b0 & 0x3); }
constexpr uint8_t    misc_op_of(uint8_t b0)     { return static_cast<uint8_t>((b0 >> 1) & 0x7); }

// 由消息首字节推出整条消息的粒度数；无法识别时返回 0（调用方按错误处理）
constexpr int message_granules_from_header(uint8_t b0) {
    // 数据类 DLENGTH=3 是保留编码，不能落入粒度表的默认 256b 分支。
    if ((msg_type_of(b0) == MsgType::WriteData ||
         msg_type_of(b0) == MsgType::WriteDataFull ||
         msg_type_of(b0) == MsgType::ReadData) && (b0 & 3) == 3) return 0;
    switch (msg_type_of(b0)) {
        case MsgType::WriteReq:      return WREQ_GRANULES;
        case MsgType::ReadReq:       return RREQ_GRANULES;
        case MsgType::WriteResp:     return WRESP_GRANULES;
        case MsgType::WriteData:     return wdata_granules(msg_dlength_of(b0));
        case MsgType::WriteDataFull: return wdatafull_granules(msg_dlength_of(b0));
        case MsgType::ReadData:      return rdata_granules(msg_dlength_of(b0));
        case MsgType::Misc:
            return (misc_op_of(b0) == MISCOP_CRDT_GRANT) ? MISC_CRDTGRANT_GRANULES
                 : (misc_op_of(b0) == MISCOP_ACTIVATION) ? MISC_ACTIVATION_GRANULES : 0;
        default:                     return 0;
    }
}

// ============================================================
//  本端 AXI 数据位宽对应的 DLENGTH（编译期常量）
//
//  真正的位宽定义在 axi_if.h（AXI_DATA_WIDTH）。这里直接从同一个宏求值，
//  是为了让 aou_types.h 不反向依赖 axi_if.h —— 下面的 FIFO/credit 深度
//  必须知道一条 ReadData 到底占几个 granule，而这取决于数据位宽：
//      256b → 8 granule    512b → 14 granule    1024b → 27 granule
//  msg_builder.h 中的 AXI_DLENGTH 直接取用本常量，并用 static_assert
//  保证两处对位宽的理解一致。
// ============================================================
#ifndef AXI_DATA_WIDTH_CFG
#define AXI_DATA_WIDTH_CFG 256
#endif
static constexpr DataLength CFG_DLENGTH =
    (AXI_DATA_WIDTH_CFG == 256)  ? DataLength::B256  :
    (AXI_DATA_WIDTH_CFG == 512)  ? DataLength::B512  : DataLength::B1024;

// 本配置下三类数据消息各占几个 granule（供深度反推与 testbench 统计使用）
static constexpr int CFG_RDATA_GRANULES     = rdata_granules(CFG_DLENGTH);
static constexpr int CFG_WDATA_GRANULES     = wdata_granules(CFG_DLENGTH);
static constexpr int CFG_WDATAFULL_GRANULES = wdatafull_granules(CFG_DLENGTH);
// 一个 beat 承载的应用数据字节数（32 / 64 / 128）
static constexpr int CFG_DATA_BYTES         = dlength_to_bytes(CFG_DLENGTH);

// 一条 N granule 的消息在链路上实际占用的裸字节数
constexpr double message_wire_bytes(int granules) {
    return granules * GRANULE_BYTES *
           static_cast<double>(FLIT_TOTAL_BYTES) / PAYLOAD_BYTES;
}

// credit环路内的消息数预算：商取整数后加一，整除时也保留一条余量。
constexpr unsigned messages_in_flight(int granules) {
    return static_cast<unsigned>(
               CREDIT_LOOP_NS * LINK_BYTES_PER_NS / message_wire_bytes(granules)) + 1u;
}

// credit 环路折算成时钟周期数（testbench 用 2ns 时钟 → 56/2 = 28 拍）
static constexpr double MODEL_CLK_PERIOD_NS = 2.0;
static constexpr unsigned CREDIT_LOOP_CYCLES =
    static_cast<unsigned>(CREDIT_LOOP_NS / MODEL_CLK_PERIOD_NS);

// ---- 接收侧（决定我们发给对端的初始 credit）----
// ReadData：受链路灌入速率约束，深度必须随数据位宽变化。
// 环路内的在飞字节数 = CREDIT_LOOP_NS × 48B/ns = 56 × 48 = 2688 B
//   256b （ 8 granule/条）：商取整数 63，+1+4 = 68 条
//   512b （14 granule/条）：商取整数 36，+1+4 = 41 条
//   1024b（27 granule/条）：商取整数 18，+1+4 = 23 条
// 三者对应的"在飞字节数"是一样的（≈2688B），只是每条消息更大、条数更少。
// messages_in_flight() 包含加一的余量，FIFO 另加四条固定余量。
static constexpr unsigned RX_RDATA_FIFO_DEPTH_PER_RP =
    messages_in_flight(CFG_RDATA_GRANULES) + FIFO_MARGIN_ENTRIES;

// WriteResp：源头是本端发出的写请求，速率上限是 AXI AW 的 1 条/拍，
//   而不是链路带宽。因此按 credit 环路的周期数定深度：28 + 4 = 32 条
static constexpr unsigned RX_WRESP_FIFO_DEPTH_PER_RP =
    CREDIT_LOOP_CYCLES + FIFO_MARGIN_ENTRIES;

// credit 以 granule 计：容量（条） × 每条消息的 granule 数
static constexpr unsigned RX_RDATA_CREDITS_PER_RP =
    RX_RDATA_FIFO_DEPTH_PER_RP * CFG_RDATA_GRANULES;  // 随位宽变化：8/14/27
static constexpr unsigned RX_WRESP_CREDITS_PER_RP =
    RX_WRESP_FIFO_DEPTH_PER_RP * WRESP_GRANULES;    // WriteResp   = 1 granule/条

// ---- 发送侧（只是抹平 AXI 突发与打包节奏的缓冲，不参与 credit 协商）----
// 深度取"一个 flit 能装下的最多消息条数"量级即可：
//   48 granule ÷ 3 granule(请求) = 16 条；÷ 7 granule(WriteDataFull) ≈ 7 条，
//   这里给写数据留两个 flit 的余量。
static constexpr unsigned TX_REQ_FIFO_DEPTH_PER_RP   = 16;
static constexpr unsigned TX_WDATA_FIFO_DEPTH_PER_RP = 16;

class BitWriter {
public:
    explicit BitWriter(uint8_t* dst) : dst_(dst) {}

    // 写入宽度为 width 的字段：value 的 bit(width-1) 先出，落在当前最高位
    void put(uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit_) {
            if ((value >> (width - 1 - i)) & 1ULL)
                dst_[bit_ >> 3] |= static_cast<uint8_t>(1u << (7 - (bit_ & 7)));
        }
    }
    // 跳过 width 个保留位（RsvdZero，保持为 0）
    void skip(unsigned width) { bit_ += width; }

    // 按给定顺序写入一段字节（要求字节对齐）
    void put_bytes(const uint8_t* src, unsigned n) {
        sc_assert((bit_ & 7) == 0);
        std::memcpy(dst_ + (bit_ >> 3), src, n);
        bit_ += n * 8;
    }

    // 数据数组的低下标表示低字节通道；编码时从最高下标开始写入。
    void put_bytes_msb_first(const uint8_t* src, unsigned n) {
        sc_assert((bit_ & 7) == 0);
        uint8_t* d = dst_ + (bit_ >> 3);
        for (unsigned i = 0; i < n; ++i) d[i] = src[n - 1 - i];
        bit_ += n * 8;
    }

    unsigned bits() const  { return bit_; }
    unsigned bytes() const { return (bit_ + 7) / 8; }

private:
    uint8_t* dst_;
    unsigned bit_ = 0;
};

class BitReader {
public:
    explicit BitReader(const uint8_t* src) : src_(src) {}

    uint64_t get(unsigned width) {
        uint64_t v = 0;
        for (unsigned i = 0; i < width; ++i, ++bit_) {
            v <<= 1;
            if (src_[bit_ >> 3] & (1u << (7 - (bit_ & 7)))) v |= 1ULL;
        }
        return v;
    }
    void skip(unsigned width) { bit_ += width; }

    void get_bytes(uint8_t* dst, unsigned n) {
        sc_assert((bit_ & 7) == 0);
        std::memcpy(dst, src_ + (bit_ >> 3), n);
        bit_ += n * 8;
    }

    // 与 put_bytes_msb_first 对称：线上第一个字节还原到 dst[n-1]
    void get_bytes_msb_first(uint8_t* dst, unsigned n) {
        sc_assert((bit_ & 7) == 0);
        const uint8_t* s = src_ + (bit_ >> 3);
        for (unsigned i = 0; i < n; ++i) dst[n - 1 - i] = s[i];
        bit_ += n * 8;
    }

    unsigned bits() const { return bit_; }

private:
    const uint8_t* src_;
    unsigned bit_ = 0;
};

// ============================================================
//  桥接消息 消息结构体
//  采用"字节数组 + 元信息"的表示方式，方便后续直接填入 Flit 载荷
// ============================================================

// 最长消息为 WriteData1024 = 30 granule = 150B；这里按完整 48-granule
// payload 预留空间，便于 Unpacker 对异常输入做有界拷贝以及后续扩展。
static constexpr int MSG_MAX_BYTES = GRANULE_COUNT * GRANULE_BYTES;

struct AouMessage {
    MsgType  type     = MsgType::Misc;     // 消息类型
    uint8_t  rp       = 0;                 // Resource Plane，合法范围由顶层 RP_COUNT 决定
    int      granules = 0;                 // 该消息占用的粒度数
    int      byte_len = 0;                 // 有效字节数（<= granules × 5）
    uint8_t  data[MSG_MAX_BYTES] = {};     // 消息载荷（按粒度对齐填充）

    // 来自 AXI 的原始事务 ID，用于日志追踪（不放入 Flit，仅模型内部使用）
    uint32_t axi_id   = 0;
    uint64_t axi_addr = 0;
};

// SystemC sc_signal<AouMessage> 要求：相等比较、输出操作符
inline bool operator==(const AouMessage& a, const AouMessage& b) {
    return a.type == b.type && a.rp == b.rp && a.granules == b.granules &&
           a.byte_len == b.byte_len && a.axi_id == b.axi_id &&
           a.axi_addr == b.axi_addr &&
           std::memcmp(a.data, b.data, MSG_MAX_BYTES) == 0;
}
inline bool operator!=(const AouMessage& a, const AouMessage& b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const AouMessage& m) {
    os << "[AouMsg type=" << msgtype_to_str(m.type)
       << " rp=" << (int)m.rp << " granules=" << m.granules
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
}

// ============================================================
//  UCIe Flit 结构体
//  对应 UCIe Latency-Optimized 256B Flit Format 6
// ============================================================
struct AouFlit {
    // ---- Protocol Header（10B）----
    // FDId[1:0]：Flit 目的 ID，单桥场景固定为 0
    uint8_t  fdid        = 0;
    // MsgStart[47:0]：位图，标记 48 个 granule 中各自是否有新消息起始。
    // 注意语义：bit[i]=1 表示"granule i 是一条新消息的第一个粒度"。
    // 若一条消息从上一个 Flit 续传过来，它占用的粒度对应 bit 为 0，
    // 且续传部分必须从 G0 开始。bit0=0 也可能只是 G0 为空，需结合接收 carry 判断。
    uint64_t msg_start   = 0;
    // MsgCredit[15:0]：随业务 flit 捎带回传的 credit，由 CreditManager 生成
    uint16_t msg_credit  = 0;

    uint8_t  payload[PAYLOAD_BYTES] = {};

    // ---- 模型辅助字段（不对应 Flit 实际 bit）----
    int      used_granules = 0;   // 本 flit 已使用的粒度数
    bool     valid         = false; // 该 flit 是否包含有效数据

    /**
     * @brief 把一条消息的一个片段追加写入载荷（支持跨 Flit 续传）
     *
     * @param msg             待写入的消息
     * @param granule_offset  该消息已经写入前面 Flit 的粒度数；0 表示消息起始
     * @return 本次实际写入的粒度数（0 表示当前 Flit 已满）
     *
     * 只有 granule_offset == 0 时才置 MsgStart 位——这正是接收侧
     * 区分"新消息"与"续传片段"的唯一依据。
     */
    int pack_fragment(const AouMessage& msg, int granule_offset) {
        if (msg.granules <= 0 || granule_offset < 0 || granule_offset >= msg.granules) return 0;
        int remain_msg  = msg.granules - granule_offset;
        int remain_flit = GRANULE_COUNT - used_granules;
        int take = std::min(remain_msg, remain_flit);
        if (take <= 0) return 0;

        if (granule_offset == 0) msg_start |= (1ULL << used_granules);

        int dst = used_granules * GRANULE_BYTES;
        int src = granule_offset * GRANULE_BYTES;
        for (int i = 0; i < take * GRANULE_BYTES; ++i) {
            int s = src + i;
            payload[dst + i] = (s < msg.byte_len) ? msg.data[s] : 0;
        }
        used_granules += take;
        valid = true;
        return take;
    }

    // 便捷接口：要求整条消息一次性放得下（用于 CrdtGrant 等短消息）
    // 返回：追加成功 true；剩余空间不足则不做任何修改并返回 false
    bool pack_message(const AouMessage& msg) {
        if (msg.granules <= 0 || msg.byte_len < 0 ||
            msg.byte_len > msg.granules * GRANULE_BYTES ||
            msg.byte_len > MSG_MAX_BYTES ||
            used_granules + msg.granules > GRANULE_COUNT) return false;
        return pack_fragment(msg, 0) == msg.granules;
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
    // sc_signal 依赖 operator== 判断值是否更新。这里必须比较完整的协议头和
    // 有效 payload，否则两个布局相同、数据不同的连续 flit 可能不会触发更新。
    if (a.valid != b.valid || a.flit.fdid != b.flit.fdid ||
        a.flit.msg_start != b.flit.msg_start ||
        a.flit.msg_credit != b.flit.msg_credit ||
        a.flit.used_granules != b.flit.used_granules ||
        a.flit.valid != b.flit.valid) {
        return false;
    }
    return std::memcmp(a.flit.payload, b.flit.payload, PAYLOAD_BYTES) == 0;
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
    sc_trace(tf, ft.flit.msg_credit,    name + ".msg_credit");
    sc_trace(tf, ft.flit.fdid,          name + ".fdid");
}
}
