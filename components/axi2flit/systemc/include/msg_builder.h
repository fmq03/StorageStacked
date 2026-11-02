/**
 * 将AXI请求、数据和响应编码为桥内部消息。字段顺序由各构建函数的写入顺序确定。
 */
#pragma once

#include "aou_types.h"
#include "axi_if.h"

// AXI 数据位宽 → 桥接消息 DLENGTH 编码。两者必须一一对应：
// 一个 AXI beat 打成一条 WriteData/ReadData 消息。
// 映射本体定义在 aou_types.h（CFG_DLENGTH），那里的 FIFO/credit 深度也要用；
// 这里做一次交叉校验，防止两个头文件对位宽的理解走偏。
static constexpr DataLength AXI_DLENGTH = CFG_DLENGTH;
static_assert(dlength_to_bytes(AXI_DLENGTH) == AXI_DATA_BYTES,
              "AXI_DATA_WIDTH 与 DLENGTH 编码不一致");

class MsgBuilder {
public:

    static AouMessage build_write_req(const AxChannel& aw, uint8_t rp) {
        return build_req(aw, rp, MsgType::WriteReq, WREQ_GRANULES);
    }

    static AouMessage build_read_req(const AxChannel& ar, uint8_t rp) {
        return build_req(ar, rp, MsgType::ReadReq, RREQ_GRANULES);
    }

    static AouMessage build_write_data(const WChannel& w, uint8_t rp,
                                       DataLength dl = AXI_DLENGTH) {
        AouMessage m;
        m.type     = MsgType::WriteData;
        m.rp       = rp & 0x3;
        m.granules = wdata_granules(dl);
        m.byte_len = m.granules * GRANULE_BYTES;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);
        bw.put(m.rp, 2);
        bw.put(static_cast<uint8_t>(dl), 2);
        bw.put(w.user & AXI_USER_MASK, 16);
        bw.put_bytes_msb_first(w.data, AXI_DATA_BYTES);

        uint8_t packed_strb[AXI_STRB_BYTES] = {};
        pack_strobe(w.strb, packed_strb);
        bw.put_bytes_msb_first(packed_strb, AXI_STRB_BYTES);
        // 余下 RsvdZero 保持为 0
        return m;
    }

    static AouMessage build_write_data_full(const WChannel& w, uint8_t rp,
                                            DataLength dl = AXI_DLENGTH) {
        AouMessage m;
        m.type     = MsgType::WriteDataFull;
        m.rp       = rp & 0x3;
        m.granules = wdatafull_granules(dl);
        m.byte_len = m.granules * GRANULE_BYTES;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);
        bw.put(m.rp, 2);
        bw.put(static_cast<uint8_t>(dl), 2);
        bw.put(w.user & AXI_USER_MASK, 16);
        bw.put_bytes_msb_first(w.data, AXI_DATA_BYTES);
        return m;
    }

    static AouMessage build_read_data(const RChannel& r, uint8_t rp,
                                      DataLength dl = AXI_DLENGTH) {
        AouMessage m;
        m.type     = MsgType::ReadData;
        m.rp       = rp & 0x3;
        m.granules = rdata_granules(dl);
        m.byte_len = m.granules * GRANULE_BYTES;
        m.axi_id   = r.id & AXI_ID_MASK;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);
        bw.put(m.rp, 2);
        bw.put(static_cast<uint8_t>(dl), 2);
        bw.put(r.user & AXI_USER_MASK, 16);
        bw.put(r.id & AXI_ID_MASK, 10);
        bw.put(r.resp & 0x3, 2);
        bw.put(r.last ? 1 : 0, 1);
        bw.skip(3);
        bw.put_bytes_msb_first(r.data, AXI_DATA_BYTES);
        return m;
    }

    static AouMessage build_write_resp(const BChannel& b, uint8_t rp) {
        AouMessage m;
        m.type     = MsgType::WriteResp;
        m.rp       = rp & 0x3;
        m.granules = WRESP_GRANULES;
        m.byte_len = m.granules * GRANULE_BYTES;
        m.axi_id   = b.id & AXI_ID_MASK;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);
        bw.put(m.rp, 2);
        bw.skip(2);
        bw.put(b.user & AXI_USER_MASK, 16);
        bw.put(b.id & AXI_ID_MASK, 10);
        bw.put(b.resp & 0x3, 2);
        bw.skip(4);
        return m;
    }

    // ========================================================
    //  判断一个 W beat 是否为"全 strobe"，决定用 WriteData 还是 WriteDataFull
    // ========================================================
    static bool is_full_strobe(const WChannel& w) {

        for (int i = 0; i < AXI_STRB_WIDTH; ++i) {
            if (w.strb[i] == 0) return false;
        }
        return true;
    }

    // 把"每字节一个元素"的 strobe 数组压缩成线上的位图（每数据字节 1bit）：
    // packed[i] 的 bit j 对应数据字节 i*8 + j。
    static void pack_strobe(const uint8_t* strb, uint8_t* packed) {
        for (int i = 0; i < AXI_STRB_WIDTH; ++i)
            if (strb[i]) packed[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }

    // 线上位图 → 每字节一个元素（MsgDecoder 与 testbench 解码侧使用）
    static void unpack_strobe(const uint8_t* packed, uint8_t* strb) {
        for (int i = 0; i < AXI_STRB_WIDTH; ++i)
            strb[i] = (packed[i >> 3] >> (i & 7)) & 1u ? 0xFF : 0x00;
    }

private:

    static AouMessage build_req(const AxChannel& ax, uint8_t rp,
                                MsgType type, int granules) {
        AouMessage m;
        m.type     = type;
        m.rp       = rp & 0x3;
        m.granules = granules;
        m.byte_len = granules * GRANULE_BYTES;
        m.axi_id   = ax.id & AXI_ID_MASK;
        m.axi_addr = ax.addr;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(type), 4);
        bw.put(m.rp, 2);
        bw.skip(1);
        bw.put(ax.lock & 0x1, 1);
        bw.put(ax.user & AXI_USER_MASK, 16);
        bw.put(ax.id & AXI_ID_MASK, 10);
        bw.put(ax.size & 0x7, 3);
        bw.put(ax.prot & 0x7, 3);
        bw.put(ax.len, 8);
        bw.put(ax.cache & 0xF, 4);
        bw.put(ax.qos & 0xF, 4);
        bw.put(ax.addr, 64);                          // AxADDR（大端落到 byte7..14）
        sc_assert(bw.bits() == 120);
        return m;
    }
};
