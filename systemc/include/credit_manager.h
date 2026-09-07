/**
 * @file credit_manager.h
 * @brief AoU per-message-type、per-RP credit 计数与编码工具
 *
 * AoU v0.8 规定，一个 credit 表示接收端为“某一消息类型的一个 5B granule”
 * 预留了空间。credit 因而不是 FIFO entry 数，也不能在不同消息类型或不同 RP
 * 之间借用。本文件把协议规则集中在 CreditManager 中，避免 Packer、Unpacker
 * 和 AXI 通道线程分别维护一套容易失配的计数逻辑。
 */

#pragma once

#include "aou_types.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <ostream>

// AoU 需要独立流控的五种消息资源；Misc 按规范不消耗 credit。
enum class CreditKind : uint8_t {
    WriteReq  = 0,
    ReadReq   = 1,
    WriteData = 2,
    ReadData  = 3,
    WriteResp = 4,
    Count     = 5
};

static constexpr unsigned CREDIT_KIND_COUNT = 5;
static constexpr unsigned CREDIT_COUNTER_MAX = 4095;

inline CreditKind msgtype_to_credit_kind(MsgType type) {
    switch (type) {
        case MsgType::WriteReq:      return CreditKind::WriteReq;
        case MsgType::ReadReq:       return CreditKind::ReadReq;
        case MsgType::WriteData:
        case MsgType::WriteDataFull: return CreditKind::WriteData;
        case MsgType::ReadData:      return CreditKind::ReadData;
        case MsgType::WriteResp:     return CreditKind::WriteResp;
        default:                     return CreditKind::Count;
    }
}

inline const char* credit_kind_to_str(CreditKind kind) {
    switch (kind) {
        case CreditKind::WriteReq:  return "WREQ";
        case CreditKind::ReadReq:   return "RREQ";
        case CreditKind::WriteData: return "WDATA";
        case CreditKind::ReadData:  return "RDATA";
        case CreditKind::WriteResp: return "WRESP";
        default:                    return "INVALID";
    }
}

// Unpacker 把对端发来的 credit 转成该结构，经 sc_fifo 送给 Packer。
struct CreditUpdate {
    uint8_t    rp       = 0;
    CreditKind kind     = CreditKind::WriteReq;
    unsigned   granules = 0;
};

// AXI R/B 真正完成握手后，接收缓冲被释放，通过该结构归还 credit。
using CreditReturn = CreditUpdate;

inline std::ostream& operator<<(std::ostream& os, const CreditUpdate& update) {
    return os << "[Credit rp=" << (int)update.rp
              << " type=" << credit_kind_to_str(update.kind)
              << " granules=" << update.granules << "]";
}

using CreditMatrix = std::array<
    std::array<unsigned, CREDIT_KIND_COUNT>, MAX_RESOURCE_PLANES>;

inline unsigned credit_kind_index(CreditKind kind) {
    return static_cast<unsigned>(kind);
}

// Table 17：3bit *CRED 编码不是二进制数，而是离散的 grant 数量。
inline unsigned decode_credit_encoding(uint8_t encoding) {
    static constexpr unsigned VALUES[8] = {0, 1, 4, 8, 16, 32, 64, 128};
    return VALUES[encoding & 0x7];
}

// 选择不超过 pending 的最大合法编码。pending=2/3 时每次只能发 1，
// 剩余 credit 会留到后续 header 或 CrdtGrant 中继续发送。
inline uint8_t encode_credit_amount(unsigned pending, unsigned max_encoding = 7) {
    for (int enc = static_cast<int>(max_encoding); enc >= 0; --enc) {
        if (decode_credit_encoding(static_cast<uint8_t>(enc)) <= pending)
            return static_cast<uint8_t>(enc);
    }
    return 0;
}

/**
 * CreditManager 不是独立 SC_MODULE，而是 FlitPacker 的状态组件：
 *   - tx_available：对端已授予、尚未被本端消息消耗的 credit；
 *   - rx_capacity：本端接收 FIFO 可向对端承诺的初始容量；
 *   - rx_pending_return：R/B 被 AXI 消费后，等待回填给对端的 credit。
 */
class CreditManager {
public:
    // 通用构造：直接给出本端各 [RP][类型] 的接收容量（granule 数）。
    // 桥接单元只接收 R/B，而对端（存储控制器一侧）只接收 WREQ/RREQ/WDATA，
    // 用同一个类描述两侧，避免 testbench 里再写一套容易失配的 credit 逻辑。
    CreditManager(unsigned rp_count, const CreditMatrix& rx_capacity)
        : rp_count_(rp_count), rx_capacity_(rx_capacity) {
        assert(rp_count_ >= 1 && rp_count_ <= MAX_RESOURCE_PLANES);
        clear_matrix(tx_available_);
        clear_matrix(rx_pending_return_);
        for (unsigned rp = rp_count_; rp < MAX_RESOURCE_PLANES; ++rp)
            rx_capacity_[rp].fill(0);
    }

    // initiator 侧桥的便捷构造：只有 ReadData / WriteResp 两类接收资源。
    CreditManager(unsigned rp_count,
                  unsigned rdata_capacity_per_rp,
                  unsigned wresp_capacity_per_rp)
        : CreditManager(rp_count,
                        make_capacity(rp_count, rdata_capacity_per_rp,
                                      wresp_capacity_per_rp)) {}

    void reset() {
        clear_matrix(tx_available_);
        clear_matrix(rx_pending_return_);
        next_header_rp_ = 0;
    }

    /**
     * @brief 复位后把全部接收容量放进"待归还"队列，交由常规 credit 发放路径分批公布
     *
     * 【为什么不能用一条 CrdtGrant 一次性公布】
     * credit 字段是 3bit 离散编码，单个字段一次最多表示 128 个 granule。
     * 1024b 配置下 RDATA 容量是 18 条 × 27 granule = 486 granule，若只发一条
     * CrdtGrant，对端只会拿到 128，"在飞字节数"仅 682B ≈ 14ns 链路时间，
     * 覆盖不住 40ns 的 TAT —— 读带宽会被 credit 往返卡死（约 17GB/s），
     * 而这跟协议效率毫无关系，纯粹是初始公布方式的问题。
     * 因此改为放进 pending，由后续若干个 CrdtGrant / MsgCredit 累加发满。
     */
    void publish_initial_capacity() {
        rx_pending_return_ = rx_capacity_;
    }

    unsigned rp_count() const { return rp_count_; }

    void add_tx_credit(const CreditUpdate& update) {
        if (!valid(update)) return;
        unsigned& counter = tx_available_[update.rp][credit_kind_index(update.kind)];
        counter = std::min(CREDIT_COUNTER_MAX, counter + update.granules);
    }

    bool can_consume(uint8_t rp, MsgType type, unsigned granules) const {
        CreditKind kind = msgtype_to_credit_kind(type);
        if (rp >= rp_count_ || kind == CreditKind::Count) return false;
        return tx_available_[rp][credit_kind_index(kind)] >= granules;
    }

    void consume(uint8_t rp, MsgType type, unsigned granules) {
        CreditKind kind = msgtype_to_credit_kind(type);
        assert(rp < rp_count_ && kind != CreditKind::Count);
        unsigned& counter = tx_available_[rp][credit_kind_index(kind)];
        assert(counter >= granules);
        counter -= granules;
    }

    void return_rx_credit(const CreditReturn& returned) {
        if (!valid(returned)) return;
        unsigned idx = credit_kind_index(returned.kind);
        unsigned& pending = rx_pending_return_[returned.rp][idx];
        // 上限使用该 RP/类型实际公布的接收容量，避免错误的重复归还无限累积。
        pending = std::min(rx_capacity_[returned.rp][idx],
                           pending + returned.granules);
    }

    bool has_pending_returns() const {
        for (unsigned rp = 0; rp < rp_count_; ++rp)
            for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k)
                if (rx_pending_return_[rp][k] != 0) return true;
        return false;
    }

    const CreditMatrix& initial_capacity() const { return rx_capacity_; }

    // 为普通数据 flit 生成 MsgCredit。一个 header 只能为一个 RP 发放 credit，
    // 因此在活跃 RP 之间轮转，避免高编号 RP 长期得不到回填。
    uint16_t take_header_grant() {
        for (unsigned checked = 0; checked < rp_count_; ++checked) {
            unsigned rp = (next_header_rp_ + checked) % rp_count_;
            bool any = false;
            for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k)
                any |= rx_pending_return_[rp][k] != 0;
            if (!any) continue;

            uint16_t field = static_cast<uint16_t>(rp << 14);
            field |= take_encoded(rp, CreditKind::WriteReq,  0, 3);
            field |= take_encoded(rp, CreditKind::ReadReq,   3, 3);
            field |= take_encoded(rp, CreditKind::WriteData, 6, 3);
            field |= take_encoded(rp, CreditKind::ReadData,  9, 3);
            field |= take_encoded(rp, CreditKind::WriteResp,12, 2);
            next_header_rp_ = (rp + 1) % rp_count_;
            return field;
        }
        return 0;
    }

    // 生成 dedicated CrdtGrant 使用的矩阵，并从 pending 中扣除已经编码的数量。
    CreditMatrix take_misc_grants() {
        CreditMatrix grants{};
        for (unsigned rp = 0; rp < rp_count_; ++rp) {
            for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k) {
                unsigned max_enc = (k == credit_kind_index(CreditKind::WriteResp)) ? 3 : 7;
                uint8_t enc = encode_credit_amount(rx_pending_return_[rp][k], max_enc);
                grants[rp][k] = decode_credit_encoding(enc);
                rx_pending_return_[rp][k] -= grants[rp][k];
            }
        }
        return grants;
    }

private:
    unsigned rp_count_ = 1;
    unsigned next_header_rp_ = 0;
    CreditMatrix tx_available_{};
    CreditMatrix rx_capacity_{};
    CreditMatrix rx_pending_return_{};

    static void clear_matrix(CreditMatrix& matrix) {
        for (auto& per_rp : matrix) per_rp.fill(0);
    }

    static CreditMatrix make_capacity(unsigned rp_count,
                                      unsigned rdata_per_rp,
                                      unsigned wresp_per_rp) {
        CreditMatrix matrix{};
        for (auto& per_rp : matrix) per_rp.fill(0);
        for (unsigned rp = 0; rp < rp_count; ++rp) {
            matrix[rp][credit_kind_index(CreditKind::ReadData)]  = rdata_per_rp;
            matrix[rp][credit_kind_index(CreditKind::WriteResp)] = wresp_per_rp;
        }
        return matrix;
    }

    bool valid(const CreditUpdate& update) const {
        return update.rp < rp_count_ && update.kind != CreditKind::Count &&
               update.granules != 0;
    }

    uint16_t take_encoded(unsigned rp, CreditKind kind,
                          unsigned shift, unsigned width) {
        unsigned idx = credit_kind_index(kind);
        unsigned max_enc = (1u << width) - 1u;
        uint8_t enc = encode_credit_amount(rx_pending_return_[rp][idx], max_enc);
        rx_pending_return_[rp][idx] -= decode_credit_encoding(enc);
        return static_cast<uint16_t>(enc) << shift;
    }
};

// -----------------------------------------------------------------------------
// CrdtGrant 消息编解码
// -----------------------------------------------------------------------------
// 布局基线：《AoU 规范 v0.8》Table 18（字段与宽度）+ Figure 21（字节级排布）。
// 80bit = 10B = 2 granule，按 MSB-first 自左向右依次是：
//     MSGTYPE   4    = 'b0000（Misc）
//     MISCOP    3    = 'b100 （CrdtGrant）
//     WREQCRED0..3   4 × 3bit
//     RREQCRED0..3   4 × 3bit
//     WDATACRED0..3  4 × 3bit
//     RDATACRED0..3  4 × 3bit
//     WRESPCRED0..3  4 × 2bit
//     RsvdZero  17
//   合计 4 + 3 + 48 + 8 + 17 = 80bit
//
// 【历史修正】v1 照抄参考 RTL 的 st_misc_grantcredit_packet，在最前面多放了
// 1bit Rsvd、尾部只留 16bit，导致每个 credit 字段整体偏移 1bit。规范里没有这
// 个前导保留位（Figure 21：byte0 = MSGTYPE[7:4] | MISCOP[3:1] | WREQCRED0 的
// 最高位）。规范 §11 明确要求 CrdtGrant 的格式与操作码编码 "shall remain
// exactly as defined"，即使自定义 Profile 也不得改动，因此这是硬互操作项：
// 偏 1bit 时本模型收发自洽、测试全过，接上真实对端却完全对不上。

inline AouMessage build_crdt_grant_message(const CreditMatrix& grants,
                                           unsigned rp_count) {
    AouMessage msg;
    msg.type = MsgType::Misc;
    msg.rp = 0;  // CrdtGrant 自身包含全部 RP 的字段，不使用普通消息的 RP 字段。
    msg.granules = MISC_CRDTGRANT_GRANULES;   // 2 granule
    msg.byte_len = MISC_CRDTGRANT_GRANULES * GRANULE_BYTES;  // 10B

    BitWriter bw(msg.data);
    bw.put(static_cast<unsigned>(MsgType::Misc), 4);       // MSGTYPE = 'b0000
    bw.put(MISCOP_CRDT_GRANT, 3);                          // MISCOP  = 'b100

    // 按消息类型分组，每组依次放 RP0～RP3（Table 18 的字段顺序）
    for (CreditKind kind : {CreditKind::WriteReq, CreditKind::ReadReq,
                            CreditKind::WriteData, CreditKind::ReadData}) {
        for (unsigned rp = 0; rp < MAX_RESOURCE_PLANES; ++rp) {
            unsigned amount = (rp < rp_count) ? grants[rp][credit_kind_index(kind)] : 0;
            bw.put(encode_credit_amount(amount), 3);
        }
    }
    // WriteResp 字段只有 2bit，最大编码值 3（对应 8 个 credit）
    for (unsigned rp = 0; rp < MAX_RESOURCE_PLANES; ++rp) {
        unsigned amount = (rp < rp_count)
            ? grants[rp][credit_kind_index(CreditKind::WriteResp)] : 0;
        bw.put(encode_credit_amount(amount, 3), 2);
    }
    bw.skip(17);                                           // RsvdZero（Table 18）
    assert(bw.bits() == 80);
    return msg;
}

inline bool decode_crdt_grant_message(const AouMessage& msg,
                                      unsigned rp_count,
                                      CreditMatrix& grants) {
    for (auto& per_rp : grants) per_rp.fill(0);
    if (msg.type != MsgType::Misc ||
        msg.byte_len < MISC_CRDTGRANT_GRANULES * GRANULE_BYTES) return false;

    BitReader br(msg.data);
    unsigned msgtype = static_cast<unsigned>(br.get(4));   // MSGTYPE
    unsigned opcode  = static_cast<unsigned>(br.get(3));   // MISCOP
    if (msgtype != static_cast<unsigned>(MsgType::Misc) || opcode != MISCOP_CRDT_GRANT)
        return false;

    for (CreditKind kind : {CreditKind::WriteReq, CreditKind::ReadReq,
                            CreditKind::WriteData, CreditKind::ReadData}) {
        for (unsigned rp = 0; rp < MAX_RESOURCE_PLANES; ++rp) {
            unsigned amount = decode_credit_encoding(static_cast<uint8_t>(br.get(3)));
            if (rp < rp_count) grants[rp][credit_kind_index(kind)] = amount;
        }
    }
    for (unsigned rp = 0; rp < MAX_RESOURCE_PLANES; ++rp) {
        unsigned amount = decode_credit_encoding(static_cast<uint8_t>(br.get(2)));
        if (rp < rp_count)
            grants[rp][credit_kind_index(CreditKind::WriteResp)] = amount;
    }
    br.skip(17);                                           // RsvdZero
    return br.bits() == 80;
}

// -----------------------------------------------------------------------------
// Protocol Header 中 MsgCredit[15:0] 的解码
// -----------------------------------------------------------------------------
// 布局基线：《AoU 规范 v0.8》Table 16（MsgCredit Encoding），从 LSB 起：
//     bit  2:0   WReqCred    (3)
//     bit  5:3   RReqCred    (3)
//     bit  8:6   WDataCred   (3)
//     bit 11:9   RDataCred   (3)
//     bit 13:12  WRespCred   (2)
//     bit 15:14  RP          (2)   —— 一个 header 只能为一个 RP 发放 credit
// 拆成最多五次更新交给 emit，因而既可写 sc_fifo，也可在 testbench 中直接累计检查。
template <typename Emit>
inline void decode_header_credits(uint16_t field, unsigned rp_count, Emit emit) {
    uint8_t rp = static_cast<uint8_t>((field >> 14) & 0x3);
    if (rp >= rp_count) return;
    const CreditKind kinds[CREDIT_KIND_COUNT] = {
        CreditKind::WriteReq, CreditKind::ReadReq, CreditKind::WriteData,
        CreditKind::ReadData, CreditKind::WriteResp
    };
    const unsigned shifts[CREDIT_KIND_COUNT] = {0, 3, 6, 9, 12};
    const unsigned masks[CREDIT_KIND_COUNT]  = {7, 7, 7, 7, 3};
    for (unsigned i = 0; i < CREDIT_KIND_COUNT; ++i) {
        unsigned amount = decode_credit_encoding(
            static_cast<uint8_t>((field >> shifts[i]) & masks[i]));
        if (amount != 0) emit(CreditUpdate{rp, kinds[i], amount});
    }
}
