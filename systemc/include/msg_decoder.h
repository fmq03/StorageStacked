/**
 * @file msg_decoder.h
 * @brief AoU 消息 → AXI beat 的反序列化工具
 *
 * 与 MsgBuilder 严格对称：字段顺序、宽度、比特序一一对应。
 * 位域布局的唯一基线是《AoU 规范 v0.8》Table 2~14 / Figure 6~18，
 * 详见 msg_builder.h 的文件头注释。
 *
 * 本文件提供两类接口：
 *   - 桥接单元自身需要的  ReadData / WriteResp 解码；
 *   - testbench 远端模型需要的 WriteReq / ReadReq / WriteData 解码
 *     （用于在 TB 侧重建请求、按 AxLEN 生成响应、校验写数据正确性）。
 */

#pragma once

#include "aou_types.h"
#include "axi_if.h"
#include "msg_builder.h"   // 复用 AXI_DLENGTH 与 unpack_strobe

class MsgDecoder {
public:
    // ---------------------------------------------------------
    //  WriteResp → B beat（1 granule = 5B，Table 13 / Figure 17）
    // ---------------------------------------------------------
    static bool decode_write_resp(const AouMessage& msg, BChannel& b) {
        if (msg.type != MsgType::WriteResp || msg.byte_len < WRESP_GRANULES * GRANULE_BYTES)
            return false;
        b = BChannel{};

        BitReader br(msg.data);
        br.skip(4);                                          // MSGTYPE
        br.skip(2);                                          // RP（调用方已从消息头拿到）
        br.skip(2);                                          // RsvdZero
        b.user = static_cast<uint16_t>(br.get(16));          // FLEX[15:0] = BUSER
        b.id   = static_cast<uint16_t>(br.get(10));          // BID[9:0]
        b.resp = static_cast<uint8_t>(br.get(2));            // BRESP
        br.skip(4);                                          // RsvdZero
        sc_assert(br.bits() == 40);
        return true;
    }

    // ---------------------------------------------------------
    //  ReadData → R beat（Table 10~12 / Figure 14~16）
    // ---------------------------------------------------------
    static bool decode_read_data(const AouMessage& msg, RChannel& r) {
        // 5B 消息头 + AXI_DATA_BYTES 数据是解析所需的最小长度
        if (msg.type != MsgType::ReadData || msg.byte_len < 5 + AXI_DATA_BYTES)
            return false;
        r = RChannel{};

        BitReader br(msg.data);
        br.skip(4);                                          // MSGTYPE
        br.skip(2);                                          // RP
        DataLength dl = static_cast<DataLength>(br.get(2));   // DLENGTH
        r.user = static_cast<uint16_t>(br.get(16));          // FLEX[15:0] = RUSER
        r.id   = static_cast<uint16_t>(br.get(10));          // RID[9:0]
        r.resp = static_cast<uint8_t>(br.get(2));            // RRESP
        r.last = (br.get(1) != 0);                           // RLAST
        br.skip(3);                                          // RsvdZero

        // DLENGTH 必须与本端 AXI 数据位宽一致，否则无法映射成一个 R beat。
        // 真实系统里这由链路两端的 Activation/CSR 协商保证（本模型未实现协商，
        // 直接按不匹配处理为解码失败，避免静默错位）。
        if (dl != AXI_DLENGTH) return false;
        br.get_bytes_msb_first(r.data, AXI_DATA_BYTES);      // RDATA（最高位字节在前）
        return true;
    }

    // ---------------------------------------------------------
    //  WriteReq / ReadReq → AW/AR beat（3 granule = 15B，Table 2/3 + Figure 6/7）
    //  两者字段完全同构，只差 MSGTYPE，因此共用一个解码函数。
    //  主要供 testbench 的远端 AoU 模型使用：需要 ID/LEN 才能生成响应。
    // ---------------------------------------------------------
    static bool decode_req(const AouMessage& msg, AxChannel& ax) {
        if ((msg.type != MsgType::WriteReq && msg.type != MsgType::ReadReq) ||
            msg.byte_len < WREQ_GRANULES * GRANULE_BYTES)
            return false;
        ax = AxChannel{};

        BitReader br(msg.data);
        br.skip(4);                                          // MSGTYPE
        br.skip(2);                                          // RP
        br.skip(1);                                          // RsvdZero
        ax.lock  = static_cast<uint8_t>(br.get(1));          // AxLOCK
        ax.user  = static_cast<uint16_t>(br.get(16));        // FLEX[15:0] = AxUSER
        ax.id    = static_cast<uint16_t>(br.get(10));        // AxID[9:0]
        ax.size  = static_cast<uint8_t>(br.get(3));          // AxSIZE
        ax.prot  = static_cast<uint8_t>(br.get(3));          // AxPROT
        ax.len   = static_cast<uint8_t>(br.get(8));          // AxLEN
        ax.cache = static_cast<uint8_t>(br.get(4));          // AxCACHE
        ax.qos   = static_cast<uint8_t>(br.get(4));          // AxQOS
        ax.addr  = br.get(64);                               // AxADDR（大端）
        ax.burst = 1;                                        // AoU 只支持 INCR
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
        br.skip(4);                                          // MSGTYPE
        br.skip(2);                                          // RP
        DataLength dl = static_cast<DataLength>(br.get(2));   // DLENGTH
        w.user = static_cast<uint16_t>(br.get(16));          // FLEX[15:0] = WUSER
        if (dl != AXI_DLENGTH) return false;
        br.get_bytes_msb_first(w.data, AXI_DATA_BYTES);      // WDATA（最高位字节在前）

        if (full) {
            std::memset(w.strb, 0xFF, AXI_STRB_WIDTH);       // 全 strobe 有效
        } else {
            uint8_t packed[AXI_STRB_BYTES] = {};
            br.get_bytes_msb_first(packed, AXI_STRB_BYTES);  // WSTRB 位图
            MsgBuilder::unpack_strobe(packed, w.strb);
        }
        // AoU 的 WriteData 里没有 WLAST，burst 边界须由 AWLEN 在接收侧重建，
        // 因此这里保持 w.last = false，由调用方按 AWLEN 计数后自行置位。
        return true;
    }
};
