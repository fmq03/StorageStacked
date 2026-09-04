/**
 * @file msg_builder.h
 * @brief AXI 通道信号 → AoU 消息转换器
 *
 * 负责将 AXI 五通道中的各类信号字段，按照 AoU Spec §5 规定的
 * 消息字段布局（Basic Profile），序列化为定长 granule 对齐的字节流。
 *
 * 每种消息的格式（以 WriteReq 为例，4 granule = 20B）：
 *   Byte 0    [7:4]=MSGTYPE [3:2]=RP [1:0]=RsvdZero
 *   Byte 1    [7]=AWLOCK   [6:0]=RsvdZero
 *   Byte 2-3  FLEX[15:0]（= AWUSER[15:0]）
 *   Byte 4    AWID[7:0]
 *   Byte 5    [7:5]=AWSIZE [4:2]=AWPROT [1:0]=RsvdZero
 *   Byte 6    AWLEN[7:0]
 *   Byte 7    [7:4]=AWCACHE [3:0]=AWQOS
 *   Byte 8-15 AWADDR[63:0]（小端序）
 *   Byte 16-19 RsvdZero 填充（凑整到 4 granule = 20B）
 *
 * 注意：字段排布为简化建模，以方便阅读为优先，不与真实 Bit 位置完全对应。
 * 后续 RTL 阶段应以 AoU Spec Table 2/3/4 等字段位置为准。
 */

#pragma once

#include "aou_types.h"
#include "axi_if.h"

// ============================================================
//  消息构造工具类（纯静态方法）
// ============================================================
class MsgBuilder {
public:

    // ----------------------------------------------------------
    //  构造 WriteReq 消息（来自 AXI AW 通道）
    //  占 4 granule = 20B
    // ----------------------------------------------------------
    static AouMessage build_write_req(const AxChannel& aw, uint8_t rp = 0) {
        AouMessage msg;
        msg.type     = MsgType::WriteReq;
        msg.granules = REQ_GRANULES;
        msg.byte_len = REQ_GRANULES * GRANULE_BYTES;  // 20B
        msg.axi_id   = aw.id;
        msg.axi_addr = aw.addr;

        uint8_t* d = msg.data;
        d[0] = ((uint8_t)MsgType::WriteReq << 4) | (rp << 2);  // MSGTYPE[7:4] RP[3:2]
        d[1] = (aw.lock & 0x1) << 7;                            // AWLOCK[7]
        d[2] = (uint8_t)(aw.user & 0xFF);                       // FLEX[7:0] = USER[7:0]
        d[3] = (uint8_t)(aw.user >> 8);                         // FLEX[15:8] = USER[15:8]
        d[4] = aw.id;                                            // AWID[7:0]
        d[5] = (aw.size & 0x7) << 5 | (aw.prot & 0x7) << 2;    // AWSIZE[7:5] AWPROT[4:2]
        d[6] = aw.len;                                           // AWLEN[7:0]
        d[7] = (aw.cache & 0xF) << 4 | (aw.qos & 0xF);          // AWCACHE[7:4] AWQOS[3:0]
        // AWADDR（64-bit 小端序）
        for (int i = 0; i < 8; ++i)
            d[8 + i] = (uint8_t)(aw.addr >> (8 * i));
        // Byte 16-19：RsvdZero 填充
        return msg;
    }

    // ----------------------------------------------------------
    //  构造 ReadReq 消息（来自 AXI AR 通道）
    //  占 4 granule = 20B，字段与 WriteReq 对称
    // ----------------------------------------------------------
    static AouMessage build_read_req(const AxChannel& ar, uint8_t rp = 0) {
        AouMessage msg;
        msg.type     = MsgType::ReadReq;
        msg.granules = REQ_GRANULES;
        msg.byte_len = REQ_GRANULES * GRANULE_BYTES;
        msg.axi_id   = ar.id;
        msg.axi_addr = ar.addr;

        uint8_t* d = msg.data;
        d[0] = ((uint8_t)MsgType::ReadReq << 4) | (rp << 2);
        d[1] = (ar.lock & 0x1) << 7;
        d[2] = (uint8_t)(ar.user & 0xFF);
        d[3] = (uint8_t)(ar.user >> 8);
        d[4] = ar.id;
        d[5] = (ar.size & 0x7) << 5 | (ar.prot & 0x7) << 2;
        d[6] = ar.len;
        d[7] = (ar.cache & 0xF) << 4 | (ar.qos & 0xF);
        for (int i = 0; i < 8; ++i)
            d[8 + i] = (uint8_t)(ar.addr >> (8 * i));
        return msg;
    }

    // ----------------------------------------------------------
    //  构造 WriteData 消息（来自 AXI W 通道，含 strobe）
    //  数据宽度固定为 256b，占 8 granule = 40B
    //    Byte 0   MSGTYPE/RP
    //    Byte 1   WLAST[7] | RsvdZero
    //    Byte 2-3 FLEX（WUSER）
    //    Byte 4-35 WDATA（256b = 32B）
    //    Byte 36-39 WSTRB（32bit = 4B，每位控制 1B 数据）
    //  共 40B = 8 granule
    // ----------------------------------------------------------
    static AouMessage build_write_data(const WChannel& w, uint8_t rp = 0) {
        AouMessage msg;
        msg.type     = MsgType::WriteData;
        msg.granules = dlength_to_granules(DataLength::B256);  // 8 granule
        msg.byte_len = msg.granules * GRANULE_BYTES;           // 40B
        msg.axi_id   = 0;  // WriteData 不携带 ID，由 WriteReq 顺序匹配

        uint8_t* d = msg.data;
        d[0] = ((uint8_t)MsgType::WriteData << 4) | (rp << 2);
        d[1] = (w.last ? 0x80 : 0x00);       // WLAST[7]
        d[2] = (uint8_t)(w.user & 0xFF);      // FLEX[7:0]
        d[3] = (uint8_t)(w.user >> 8);        // FLEX[15:8]

        // WDATA：32B
        for (int i = 0; i < AXI_STRB_WIDTH; ++i)
            d[4 + i] = w.data[i];

        // WSTRB：4B（32b strobe，每位对应 1B 数据）
        // 将 AXI 的 32B strobe 合并成 4B 位图
        for (int i = 0; i < 4; ++i) {
            uint8_t sb = 0;
            for (int j = 0; j < 8; ++j)
                sb |= (w.strb[i * 8 + j] ? (1 << j) : 0);
            d[36 + i] = sb;
        }
        // Byte 40-39：本消息正好 40B，无填充
        return msg;
    }

    // ----------------------------------------------------------
    //  构造 WriteDataFull 消息（无 strobe，全字节有效）
    //  比 WriteData 少 4B strobe，但粒度对齐仍为 8 granule
    //  在全写场景（strobe 全 1）中优先使用，避免无谓字段
    // ----------------------------------------------------------
    static AouMessage build_write_data_full(const WChannel& w, uint8_t rp = 0) {
        AouMessage msg;
        msg.type     = MsgType::WriteDataFull;
        msg.granules = dlength_to_granules(DataLength::B256);
        msg.byte_len = msg.granules * GRANULE_BYTES;  // 40B

        uint8_t* d = msg.data;
        d[0] = ((uint8_t)MsgType::WriteDataFull << 4) | (rp << 2);
        d[1] = (w.last ? 0x80 : 0x00);
        d[2] = (uint8_t)(w.user & 0xFF);
        d[3] = (uint8_t)(w.user >> 8);
        for (int i = 0; i < AXI_STRB_WIDTH; ++i)
            d[4 + i] = w.data[i];
        // Byte 36-39：RsvdZero（strobe 字段省略，填 0）
        return msg;
    }

    // ----------------------------------------------------------
    //  构造 WriteResp 消息（来自存储器 / NoC 返回，非发送方向）
    //  此处仅供参考，单向模型（AXI→Flit）不会用到
    //  占 2 granule = 10B
    // ----------------------------------------------------------
    static AouMessage build_write_resp(uint8_t id, uint8_t resp, uint8_t rp = 0) {
        AouMessage msg;
        msg.type     = MsgType::WriteResp;
        msg.granules = WRESP_GRANULES;
        msg.byte_len = WRESP_GRANULES * GRANULE_BYTES;  // 10B
        msg.axi_id   = id;

        uint8_t* d = msg.data;
        d[0] = ((uint8_t)MsgType::WriteResp << 4) | (rp << 2);
        d[1] = id;                   // BID
        d[2] = resp & 0x3;           // BRESP[1:0]
        d[3] = 0;                    // FLEX（USER）低8位
        d[4] = 0;                    // FLEX（USER）高8位
        // Byte 5-9：RsvdZero
        return msg;
    }

    // ----------------------------------------------------------
    //  判断 W 通道的写数据是否全字节有效（strobe 全 1）
    //  用于决定使用 WriteData 还是 WriteDataFull
    // ----------------------------------------------------------
    static bool is_full_strobe(const WChannel& w) {
        for (int i = 0; i < AXI_STRB_WIDTH; ++i)
            if (w.strb[i] != 0xFF) return false;
        return true;
    }
};
