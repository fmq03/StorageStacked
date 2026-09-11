/**
 * 从桥内部消息恢复AXI字段。数据长度编码必须与本地总线宽度一致。
 */
#pragma once

#include "aou_types.h"
#include "axi_if.h"
#include "msg_builder.h"   // 复用 AXI_DLENGTH 与 unpack_strobe

class MsgDecoder {
public:

    static bool decode_write_resp(const AouMessage& msg, BChannel& b) {
        if (msg.type != MsgType::WriteResp || msg.byte_len < WRESP_GRANULES * GRANULE_BYTES)
            return false;
        b = BChannel{};

        BitReader br(msg.data);
        br.skip(4);
        br.skip(2);                                          // RP（调用方已从消息头拿到）
        br.skip(2);
        b.user = static_cast<uint16_t>(br.get(16));
        b.id   = static_cast<uint16_t>(br.get(10));
        b.resp = static_cast<uint8_t>(br.get(2));
        br.skip(4);
        sc_assert(br.bits() == 40);
        return true;
    }

    static bool decode_read_data(const AouMessage& msg, RChannel& r) {
        // 5B 消息头 + AXI_DATA_BYTES 数据是解析所需的最小长度
        if (msg.type != MsgType::ReadData || msg.byte_len < 5 + AXI_DATA_BYTES)
            return false;
        r = RChannel{};

        BitReader br(msg.data);
        br.skip(4);
        br.skip(2);
        DataLength dl = static_cast<DataLength>(br.get(2));
        r.user = static_cast<uint16_t>(br.get(16));
        r.id   = static_cast<uint16_t>(br.get(10));
        r.resp = static_cast<uint8_t>(br.get(2));
        r.last = (br.get(1) != 0);
        br.skip(3);

        // DLENGTH 必须与本端 AXI 数据位宽一致，否则无法映射成一个 R beat。
        // 两端须使用一致的编译配置，宽度不匹配时返回解码失败。
        if (dl != AXI_DLENGTH) return false;
        br.get_bytes_msb_first(r.data, AXI_DATA_BYTES);      // RDATA（最高位字节在前）
        return true;
    }

    static bool decode_req(const AouMessage& msg, AxChannel& ax) {
        if ((msg.type != MsgType::WriteReq && msg.type != MsgType::ReadReq) ||
            msg.byte_len < WREQ_GRANULES * GRANULE_BYTES)
            return false;
        ax = AxChannel{};

        BitReader br(msg.data);
        br.skip(4);
        br.skip(2);
        br.skip(1);
        ax.lock  = static_cast<uint8_t>(br.get(1));
        ax.user  = static_cast<uint16_t>(br.get(16));
        ax.id    = static_cast<uint16_t>(br.get(10));
        ax.size  = static_cast<uint8_t>(br.get(3));
        ax.prot  = static_cast<uint8_t>(br.get(3));
        ax.len   = static_cast<uint8_t>(br.get(8));
        ax.cache = static_cast<uint8_t>(br.get(4));
        ax.qos   = static_cast<uint8_t>(br.get(4));
        ax.addr  = br.get(64);                               // AxADDR（大端）
        ax.burst = 1;                                        // 本模型只支持 INCR
        sc_assert(br.bits() == 120);
        return true;
    }

    // ---------------------------------------------------------
    //  WriteData / WriteDataFull → W beat
    //  WriteDataFull 没有 WSTRB 字段，解码后 strb 全部置为有效。
    // ---------------------------------------------------------
    static bool decode_write_data(const AouMessage& msg, WChannel& w) {
        bool full = (msg.type == MsgType::WriteDataFull);
        if (msg.type != MsgType::WriteData && !full) return false;
        if (msg.byte_len < 3 + AXI_DATA_BYTES) return false;
        w = WChannel{};

        BitReader br(msg.data);
        br.skip(4);
        br.skip(2);
        DataLength dl = static_cast<DataLength>(br.get(2));
        w.user = static_cast<uint16_t>(br.get(16));
        if (dl != AXI_DLENGTH) return false;
        br.get_bytes_msb_first(w.data, AXI_DATA_BYTES);      // WDATA（最高位字节在前）

        if (full) {
            std::memset(w.strb, 0xFF, AXI_STRB_WIDTH);       // 全 strobe 有效
        } else {
            uint8_t packed[AXI_STRB_BYTES] = {};
            br.get_bytes_msb_first(packed, AXI_STRB_BYTES);  // WSTRB 位图
            MsgBuilder::unpack_strobe(packed, w.strb);
        }
        // 写数据消息不携带 WLAST，突发边界须由 AWLEN 在接收侧重建，
        // 因此这里保持 w.last = false，由调用方按 AWLEN 计数后自行置位。
        return true;
    }
};
