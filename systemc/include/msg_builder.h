/**
 * @file msg_builder.h
 * @brief AXI beat → AoU 消息的序列化工具
 *
 * 【位域布局的唯一基线：AoU 规范 v0.8 PDF】
 * 各消息的字段集合、字段宽度与线上位置，逐条对齐
 *   doc/AXI over UCIe Protocol Specification v0.8.pdf
 *   §5.3~§5.6 的 Table 2~Table 14（字段与宽度）
 *   §5.8      的 Figure 6~Figure 18（字节/比特级排布）
 *
 * 规范 §5.8 规定线上格式遵循 PCIe 约定：字节序号递增、每字节内 bit7→bit0、
 * 字段自左向右 MSB-first。aou_types.h 的 BitWriter 正是按这个约定实现的，
 * 所以下面每个 build_* 里 put() 的先后顺序 = 规范图里字段从左到右的顺序，
 * 可以拿着 PDF 逐行核对。
 *
 * 参考 RTL（reference/tt-oca-harness-aou）只用于交叉验证 granule 常量；它在
 * PROF/PROFEXTLEN→FLEX 这一项上停留在 v0.8 之前的版本，其 st_*_packet_tmp
 * 的字段划分不再作为布局依据。
 *
 * 【FLEX[15:0]】
 * v0.8 把旧的 PROF[11:0] + PROFEXTLEN[3:0] 合并重定义为 FLEX[15:0]，并规定
 * Basic Profile 下 FLEX 即 xUSER。因此 AXI_USER_WIDTH = 16，且 FLEX 在线上
 * 是一个完整的 16bit 大端字段（byte1 = FLEX[15:8]，byte2 = FLEX[7:0]）。
 *
 * 【数据字段的字节序】
 * 规范把 WDATA/RDATA 的最高位字节排在最前（Figure 8：g0 尾部是 WDATA[255:240]，
 * g6 是 WDATA[39:0]）。模型里 data[0] 是最低有效字节，因此用
 * put_bytes_msb_first() 倒序写入。WSTRB 同理（Figure 8 的 g7 = WSTRB[31:0]）。
 * 地址也一样：put(addr,64) 天然得到 byte7 = AWADDR[63:56] 的大端排布。
 *
 * 每条消息的长度都严格等于 aou_types.h 中的粒度常量：
 *   WriteReq/ReadReq   3 granule = 15B     （Table 2 / Table 3）
 *   WriteResp          1 granule =  5B     （Table 13）
 *                       256b      512b      1024b
 *   WriteData          8 gran    15 gran    30 gran   （Table 4~6）
 *   WriteDataFull      7 gran    14 gran    27 gran   （Table 7~9）
 *   ReadData           8 gran    14 gran    27 gran   （Table 10~12）
 *
 * 【数据宽度自适应】
 * WriteData / ReadData 的默认 DLENGTH 取 AXI_DLENGTH，由编译期常量
 * AXI_DATA_WIDTH 推出，因此把 -DAXI_DATA_WIDTH_CFG 从 256 改成 1024 后，
 * 消息长度、数据字节数、WSTRB 字节数会全部自动跟随，不需要改这里的代码。
 */

#pragma once

#include "aou_types.h"
#include "axi_if.h"

// AXI 数据位宽 → AoU DLENGTH 编码。两者必须一一对应：
// 一个 AXI beat 打成一条 WriteData/ReadData 消息。
// 映射本体定义在 aou_types.h（CFG_DLENGTH），那里的 FIFO/credit 深度也要用；
// 这里做一次交叉校验，防止两个头文件对位宽的理解走偏。
static constexpr DataLength AXI_DLENGTH = CFG_DLENGTH;
static_assert(dlength_to_bytes(AXI_DLENGTH) == AXI_DATA_BYTES,
              "AXI_DATA_WIDTH 与 DLENGTH 编码不一致");

class MsgBuilder {
public:
    // ========================================================
    //  WriteReq（对应 AXI AW 通道）—— 3 granule / 15B / 120bit
    //  规范 Table 2 + Figure 6，字段自左向右：
    //    byte0   MSGTYPE[3:0] | RP[1:0] | RsvdZero(1) | AWLOCK(1)
    //    byte1-2 FLEX[15:0]
    //    byte3-4 AWID[9:0] | AWSIZE[2:0] | AWPROT[2:0]
    //    byte5   AWLEN[7:0]
    //    byte6   AWCACHE[3:0] | AWQOS[3:0]
    //    byte7-14 AWADDR[63:0]（大端）
    // ========================================================
    static AouMessage build_write_req(const AxChannel& aw, uint8_t rp) {
        return build_req(aw, rp, MsgType::WriteReq, WREQ_GRANULES);
    }

    // ReadReq（对应 AXI AR 通道）—— 3 granule，布局与 WriteReq 逐字段同构
    // （Table 3 + Figure 7，只有 MSGTYPE 与字段名不同）
    static AouMessage build_read_req(const AxChannel& ar, uint8_t rp) {
        return build_req(ar, rp, MsgType::ReadReq, RREQ_GRANULES);
    }

    // ========================================================
    //  WriteData（带 WSTRB）—— 规范 Table 4~6 + Figure 8~10
    //  DLENGTH=256b 时 8 granule / 40B / 320bit：
    //    byte0    MSGTYPE[3:0] | RP[1:0] | DLENGTH[1:0]
    //    byte1-2  FLEX[15:0]
    //    byte3..  WDATA（32/64/128B，最高位字节在前）
    //    随后     WSTRB（4/8/16B，WSTRB[31] 在最前）
    //    末尾     RsvdZero：256b 补 1B、512b 补 0B、1024b 补 3B
    //
    //  【注意】AoU 的 WriteData 消息里没有 WLAST 字段。burst 边界由
    //  WriteReq 中的 AWLEN 在接收侧重建。本模型为发起端，不解析回来的
    //  WriteData，故暂不需要重建逻辑。
    // ========================================================
    static AouMessage build_write_data(const WChannel& w, uint8_t rp,
                                       DataLength dl = AXI_DLENGTH) {
        AouMessage m;
        m.type     = MsgType::WriteData;
        m.rp       = rp & 0x3;
        m.granules = wdata_granules(dl);
        m.byte_len = m.granules * GRANULE_BYTES;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);      // MSGTYPE = 'b0011
        bw.put(m.rp, 2);                              // RP
        bw.put(static_cast<uint8_t>(dl), 2);          // DLENGTH
        bw.put(w.user & AXI_USER_MASK, 16);           // FLEX[15:0] = WUSER
        bw.put_bytes_msb_first(w.data, AXI_DATA_BYTES);   // WDATA：32/64/128B
        // WSTRB：模型里每个数据字节一个数组元素，线上是每字节 1bit。
        // pack_strobe 产出的 packed[k] 的 bit j 对应数据字节 8k+j，
        // 而规范把 WSTRB 的最高位（对应最后一个数据字节）排在最前，
        // 因此同样倒序写入。
        uint8_t packed_strb[AXI_STRB_BYTES] = {};
        pack_strobe(w.strb, packed_strb);
        bw.put_bytes_msb_first(packed_strb, AXI_STRB_BYTES);
        // 余下 RsvdZero 保持为 0
        return m;
    }

    // ========================================================
    //  WriteDataFull（全 strobe，无 WSTRB）—— Table 7~9 + Figure 11~13
    //  布局 = WriteData 去掉 WSTRB；256b 时 7 granule / 35B / 280bit
    //  这是本桥拿到高带宽利用率的关键：省下 5B/40B = 12.5% 的开销
    // ========================================================
    static AouMessage build_write_data_full(const WChannel& w, uint8_t rp,
                                            DataLength dl = AXI_DLENGTH) {
        AouMessage m;
        m.type     = MsgType::WriteDataFull;
        m.rp       = rp & 0x3;
        m.granules = wdatafull_granules(dl);
        m.byte_len = m.granules * GRANULE_BYTES;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);      // MSGTYPE = 'b0110
        bw.put(m.rp, 2);                              // RP
        bw.put(static_cast<uint8_t>(dl), 2);          // DLENGTH
        bw.put(w.user & AXI_USER_MASK, 16);           // FLEX[15:0]
        bw.put_bytes_msb_first(w.data, AXI_DATA_BYTES);   // WDATA
        return m;
    }

    // ========================================================
    //  ReadData（对应 AXI R 通道）—— Table 10~12 + Figure 14~16
    //  256b 时 8 granule / 40B / 320bit：
    //    byte0    MSGTYPE[3:0] | RP[1:0] | DLENGTH[1:0]
    //    byte1-2  FLEX[15:0]
    //    byte3-4  RID[9:0] | RRESP[1:0] | RLAST(1) | RsvdZero(3)
    //    byte5..  RDATA（最高位字节在前）
    //    末尾     RsvdZero：256b 补 3B、512b 补 1B、1024b 补 2B
    // ========================================================
    static AouMessage build_read_data(const RChannel& r, uint8_t rp,
                                      DataLength dl = AXI_DLENGTH) {
        AouMessage m;
        m.type     = MsgType::ReadData;
        m.rp       = rp & 0x3;
        m.granules = rdata_granules(dl);
        m.byte_len = m.granules * GRANULE_BYTES;
        m.axi_id   = r.id & AXI_ID_MASK;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);      // MSGTYPE = 'b0100
        bw.put(m.rp, 2);                              // RP
        bw.put(static_cast<uint8_t>(dl), 2);          // DLENGTH
        bw.put(r.user & AXI_USER_MASK, 16);           // FLEX[15:0] = RUSER
        bw.put(r.id & AXI_ID_MASK, 10);               // RID[9:0]
        bw.put(r.resp & 0x3, 2);                      // RRESP
        bw.put(r.last ? 1 : 0, 1);                    // RLAST
        bw.skip(3);                                   // RsvdZero
        bw.put_bytes_msb_first(r.data, AXI_DATA_BYTES);   // RDATA
        return m;
    }

    // ========================================================
    //  WriteResp（对应 AXI B 通道）—— Table 13 + Figure 17
    //    byte0    MSGTYPE[3:0] | RP[1:0] | RsvdZero(2)
    //    byte1-2  FLEX[15:0]
    //    byte3-4  BID[9:0] | BRESP[1:0] | RsvdZero(4)
    //  合计 1 granule / 5B / 40bit
    // ========================================================
    static AouMessage build_write_resp(const BChannel& b, uint8_t rp) {
        AouMessage m;
        m.type     = MsgType::WriteResp;
        m.rp       = rp & 0x3;
        m.granules = WRESP_GRANULES;
        m.byte_len = m.granules * GRANULE_BYTES;
        m.axi_id   = b.id & AXI_ID_MASK;

        BitWriter bw(m.data);
        bw.put(static_cast<uint8_t>(m.type), 4);      // MSGTYPE = 'b0101
        bw.put(m.rp, 2);                              // RP
        bw.skip(2);                                   // RsvdZero
        bw.put(b.user & AXI_USER_MASK, 16);           // FLEX[15:0] = BUSER
        bw.put(b.id & AXI_ID_MASK, 10);               // BID[9:0]
        bw.put(b.resp & 0x3, 2);                      // BRESP
        bw.skip(4);                                   // RsvdZero
        return m;
    }

    // ========================================================
    //  判断一个 W beat 是否为"全 strobe"，决定用 WriteData 还是 WriteDataFull
    // ========================================================
    static bool is_full_strobe(const WChannel& w) {
        // strb[] 是"每个数据字节一个元素"，必须全部检查（原实现只查了前
        // AXI_STRB_WIDTH/8 个，会把部分屏蔽的 beat 误判成全 strobe）。
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
    // AW / AR 共用的请求消息打包（Table 2/3 与 Figure 6/7 逐字段同构，只差 MSGTYPE）
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
        bw.put(static_cast<uint8_t>(type), 4);        // MSGTYPE
        bw.put(m.rp, 2);                              // RP
        bw.skip(1);                                   // RsvdZero
        bw.put(ax.lock & 0x1, 1);                     // AxLOCK
        bw.put(ax.user & AXI_USER_MASK, 16);          // FLEX[15:0] = AxUSER
        bw.put(ax.id & AXI_ID_MASK, 10);              // AxID[9:0]
        bw.put(ax.size & 0x7, 3);                     // AxSIZE
        bw.put(ax.prot & 0x7, 3);                     // AxPROT
        bw.put(ax.len, 8);                            // AxLEN
        bw.put(ax.cache & 0xF, 4);                    // AxCACHE
        bw.put(ax.qos & 0xF, 4);                      // AxQOS
        bw.put(ax.addr, 64);                          // AxADDR（大端落到 byte7..14）
        sc_assert(bw.bits() == 120);
        return m;
    }
};
