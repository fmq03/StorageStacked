/**
 * @file tb_golden_vectors.cpp
 * @brief AoU 线上格式的"黄金字节向量"对拍（规范一致性测试）
 *
 * 【这个测试解决什么问题】
 * 功能 TB 里的远端模型 RemoteAouModel 复用了 DUT 的 MsgBuilder/MsgDecoder，
 * 所以只要打包和解包用的是同一套（哪怕是错的）位域约定，测试就一定通过。
 * 第一版模型的 CrdtGrant 在最前面多了 1bit 保留位，全部 TC 依然全绿，正是
 * 因为收发两端一起错。
 *
 * 本文件不比较"发出去再收回来是否一致"，而是把 MsgBuilder 产生的字节流与
 * 直接从《AXI over UCIe Protocol Specification v0.8》的表/图手工展开的字节
 * 数组逐字节比较。任何位域挪动、字节序翻转、保留位增减都会立即失败。
 *
 * 【黄金值从哪来】
 * 每个用例上面都写清了规范出处（Table/Figure 编号）与完整的比特串推导，
 * 比特串按规范 §5.8 的 PCIe 约定串接：
 *     Bytes count up / Bits count down / MSB-first, left to right
 * 复核方式：拿 PDF 对着注释里的比特串读一遍，再核对十六进制字面量。
 *
 * 【运行】
 *     make golden            # 默认位宽
 *     make golden-all        # 256 / 512 / 1024 三种位宽
 */

#include <systemc.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aou_types.h"
#include "axi_if.h"
#include "msg_builder.h"
#include "msg_decoder.h"
#include "credit_manager.h"

// ============================================================
//  检查框架
// ============================================================
static int g_fail = 0;
static int g_pass = 0;

static void dump(const char* tag, const uint8_t* p, unsigned n) {
    std::printf("      %-8s:", tag);
    for (unsigned i = 0; i < n; ++i) {
        if (i && i % 16 == 0) std::printf("\n               ");
        std::printf(" %02X", p[i]);
    }
    std::printf("\n");
}

// 逐字节比较，失败时把两条字节流都打出来，方便直接定位是哪一位挪了
static void expect_bytes(const char* name, const uint8_t* got,
                         const std::vector<uint8_t>& exp) {
    bool ok = std::memcmp(got, exp.data(), exp.size()) == 0;
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s（%zu B 逐字节一致）\n", name, exp.size());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name);
        dump("规范", exp.data(), static_cast<unsigned>(exp.size()));
        dump("模型", got, static_cast<unsigned>(exp.size()));
        for (size_t i = 0; i < exp.size(); ++i)
            if (got[i] != exp[i]) {
                std::printf("      首个不一致：byte%zu 规范=0x%02X 模型=0x%02X"
                            "（异或 0x%02X）\n", i, exp[i], got[i],
                            static_cast<uint8_t>(exp[i] ^ got[i]));
                break;
            }
    }
}

static void expect_eq(const char* name, uint64_t got, uint64_t exp) {
    if (got == exp) { ++g_pass; std::printf("  [PASS] %s = %llu\n", name,
                                            static_cast<unsigned long long>(exp)); }
    else { ++g_fail; std::printf("  [FAIL] %s：期望 %llu，实得 %llu\n", name,
                                 static_cast<unsigned long long>(exp),
                                 static_cast<unsigned long long>(got)); }
}

static void expect_true(const char* name, bool cond) {
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", name); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", name); }
}

// ============================================================
//  测试激励的数据/选通图样
//
//  刻意不用"整条 beat 同一个字节值"：那种图样下即使把数据整体倒序也看不出
//  任何差别，正好掩盖字节序错误。这里用递增图样，任何一次错位/翻转都会露馅。
// ============================================================
static uint8_t pattern_byte(int i) { return static_cast<uint8_t>(0x10 + i); }
// 选通图样：每 3 个字节屏蔽 1 个，产生非平凡的 WSTRB 位图
static bool    strobe_bit(int i)   { return (i % 3) != 0; }

// ============================================================
//  用例 1：WriteReq —— 规范 Table 2 + Figure 6
//
//  取值：MSGTYPE='b0001  RP='b10  RsvdZero=0  AWLOCK=1
//        FLEX=0xA55A     AWID=0x2C3('b1011000011)
//        AWSIZE='b101    AWPROT='b011
//        AWLEN=0x0F      AWCACHE='b1010  AWQOS='b0110
//        AWADDR=0x0123456789ABCDEF
//
//  按 Figure 6 自左向右串接（120bit = 3 granule）：
//    byte0    0001 10 0 1                         -> 0x19
//    byte1-2  FLEX[15:8]=0xA5  FLEX[7:0]=0x5A     -> 0xA5 0x5A
//    byte3-4  1011000011 101 011                  -> 0xB0 0xEB
//    byte5    AWLEN                               -> 0x0F
//    byte6    AWCACHE=1010  AWQOS=0110            -> 0xA6
//    byte7-14 AWADDR 大端                         -> 01 23 45 67 89 AB CD EF
// ============================================================
static void test_write_req() {
    std::printf("\n--- 用例1 WriteReq（Table 2 / Figure 6）---\n");
    AxChannel aw{};
    aw.lock = 1;  aw.user = 0xA55A;  aw.id = 0x2C3;
    aw.size = 0b101;  aw.prot = 0b011;  aw.len = 0x0F;
    aw.cache = 0xA;   aw.qos = 0x6;
    aw.addr = 0x0123456789ABCDEFull;

    AouMessage m = MsgBuilder::build_write_req(aw, /*rp=*/2);
    expect_eq("WriteReq granule 数", m.granules, WREQ_GRANULES);
    expect_bytes("WriteReq 线上字节", m.data,
                 {0x19, 0xA5, 0x5A, 0xB0, 0xEB, 0x0F, 0xA6,
                  0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});

    // 反向：解码回来必须一字不差
    AxChannel back{};
    expect_true("WriteReq 可解码", MsgDecoder::decode_req(m, back));
    expect_eq("  AWADDR", back.addr, aw.addr);
    expect_eq("  AWID",   back.id,   aw.id);
    expect_eq("  FLEX(AWUSER)", back.user, aw.user);
    expect_eq("  AWLEN",  back.len,  aw.len);
    expect_eq("  AWSIZE", back.size, aw.size);
    expect_eq("  AWPROT", back.prot, aw.prot);
    expect_eq("  AWCACHE", back.cache, aw.cache);
    expect_eq("  AWQOS",  back.qos,  aw.qos);
    expect_eq("  AWLOCK", back.lock, aw.lock);
}

// ============================================================
//  用例 2：ReadReq —— 规范 Table 3 + Figure 7（与 WriteReq 同构）
//
//  取值：MSGTYPE='b0010  RP='b01  ARLOCK=0  FLEX=0x5AA5
//        ARID=0x0FC      ARSIZE='b110  ARPROT='b001
//        ARLEN=0x3F      ARCACHE='b0101  ARQOS='b1001
//        ARADDR=0xFEDCBA9876543210
//    byte0    0010 01 0 0                         -> 0x24
//    byte3-4  0011111100 110 001                  -> 0x3F 0x31
//    byte6    ARCACHE=0101 ARQOS=1001             -> 0x59
// ============================================================
static void test_read_req() {
    std::printf("\n--- 用例2 ReadReq（Table 3 / Figure 7）---\n");
    AxChannel ar{};
    ar.lock = 0;  ar.user = 0x5AA5;  ar.id = 0x0FC;
    ar.size = 0b110;  ar.prot = 0b001;  ar.len = 0x3F;
    ar.cache = 0x5;   ar.qos = 0x9;
    ar.addr = 0xFEDCBA9876543210ull;

    AouMessage m = MsgBuilder::build_read_req(ar, /*rp=*/1);
    expect_bytes("ReadReq 线上字节", m.data,
                 {0x24, 0x5A, 0xA5, 0x3F, 0x31, 0x3F, 0x59,
                  0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});

    AxChannel back{};
    expect_true("ReadReq 可解码", MsgDecoder::decode_req(m, back));
    expect_eq("  ARADDR", back.addr, ar.addr);
    expect_eq("  ARID",   back.id,   ar.id);
    expect_eq("  FLEX(ARUSER)", back.user, ar.user);
}

// ============================================================
//  用例 3：WriteResp —— 规范 Table 13 + Figure 17
//
//  取值：MSGTYPE='b0101 RP='b01 RsvdZero=00 FLEX=0x1234
//        BID=0x155('b0101010101)  BRESP='b10  RsvdZero(4)=0000
//    byte0    0101 01 00                          -> 0x54
//    byte1-2  0x12 0x34
//    byte3-4  0101010101 10 0000                  -> 0x55 0x60
// ============================================================
static void test_write_resp() {
    std::printf("\n--- 用例3 WriteResp（Table 13 / Figure 17）---\n");
    BChannel b{};
    b.id = 0x155;  b.resp = 0b10;  b.user = 0x1234;

    AouMessage m = MsgBuilder::build_write_resp(b, /*rp=*/1);
    expect_eq("WriteResp granule 数", m.granules, WRESP_GRANULES);
    expect_bytes("WriteResp 线上字节", m.data, {0x54, 0x12, 0x34, 0x55, 0x60});

    BChannel back{};
    expect_true("WriteResp 可解码", MsgDecoder::decode_write_resp(m, back));
    expect_eq("  BID",   back.id,   b.id);
    expect_eq("  BRESP", back.resp, b.resp);
    expect_eq("  FLEX(BUSER)", back.user, b.user);
}

// ============================================================
//  用例 4：ReadData —— 规范 Table 10~12 + Figure 14~16
//
//  取值：MSGTYPE='b0100 RP='b11 DLENGTH=本配置
//        FLEX=0xBEEF  RID=0x2AA('b1010101010) RRESP='b01 RLAST=1 RsvdZero(3)=000
//    byte0    0100 11 DL      -> 256b:0x4C  512b:0x4D  1024b:0x4E
//    byte1-2  0xBE 0xEF
//    byte3-4  1010101010 01 1 000                 -> 0xAA 0x98
//    byte5..  RDATA：规范把最高位字节排在最前（Figure 14 的 g1 = RDATA[255:216]），
//             模型里 data[0] 是最低有效字节，因此线上 byte5 = data[N-1]。
//    末尾     RsvdZero：256b 3B / 512b 1B / 1024b 2B，必须为 0
// ============================================================
static void test_read_data() {
    std::printf("\n--- 用例4 ReadData%d（Table 10~12 / Figure 14~16）---\n", AXI_DATA_WIDTH);
    RChannel r{};
    r.id = 0x2AA;  r.resp = 0b01;  r.last = true;  r.user = 0xBEEF;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) r.data[i] = pattern_byte(i);

    AouMessage m = MsgBuilder::build_read_data(r, /*rp=*/3);
    expect_eq("ReadData granule 数", m.granules, CFG_RDATA_GRANULES);

    std::vector<uint8_t> exp;
    exp.push_back(static_cast<uint8_t>(0x4C + static_cast<int>(AXI_DLENGTH)));
    exp.push_back(0xBE); exp.push_back(0xEF);
    exp.push_back(0xAA); exp.push_back(0x98);
    for (int i = 0; i < AXI_DATA_BYTES; ++i)          // 最高位字节在前
        exp.push_back(pattern_byte(AXI_DATA_BYTES - 1 - i));
    exp.resize(static_cast<size_t>(CFG_RDATA_GRANULES) * GRANULE_BYTES, 0x00);  // RsvdZero
    expect_bytes("ReadData 线上字节", m.data, exp);

    RChannel back{};
    expect_true("ReadData 可解码", MsgDecoder::decode_read_data(m, back));
    expect_eq("  RID",   back.id,   r.id);
    expect_eq("  RRESP", back.resp, r.resp);
    expect_eq("  RLAST", back.last, r.last);
    expect_eq("  FLEX(RUSER)", back.user, r.user);
    expect_true("  RDATA 逐字节还原", std::memcmp(back.data, r.data, AXI_DATA_BYTES) == 0);
}

// ============================================================
//  用例 5：WriteDataFull —— 规范 Table 7~9 + Figure 11~13
//
//  取值：MSGTYPE='b0110 RP='b00 DLENGTH=本配置 FLEX=0x0F1E
//    byte0    0110 00 DL      -> 256b:0x60  512b:0x61  1024b:0x62
//    byte1-2  0x0F 0x1E
//    byte3..  WDATA，最高位字节在前
//    末尾     RsvdZero：256b 0B / 512b 3B / 1024b 4B
// ============================================================
static void test_write_data_full() {
    std::printf("\n--- 用例5 WriteDataFull%d（Table 7~9 / Figure 11~13）---\n", AXI_DATA_WIDTH);
    WChannel w{};
    w.user = 0x0F1E;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) { w.data[i] = pattern_byte(i); w.strb[i] = 0xFF; }
    expect_true("全 strobe 判定为 WriteDataFull", MsgBuilder::is_full_strobe(w));

    AouMessage m = MsgBuilder::build_write_data_full(w, /*rp=*/0);
    expect_eq("WriteDataFull granule 数", m.granules, CFG_WDATAFULL_GRANULES);

    std::vector<uint8_t> exp;
    exp.push_back(static_cast<uint8_t>(0x60 + static_cast<int>(AXI_DLENGTH)));
    exp.push_back(0x0F); exp.push_back(0x1E);
    for (int i = 0; i < AXI_DATA_BYTES; ++i)
        exp.push_back(pattern_byte(AXI_DATA_BYTES - 1 - i));
    exp.resize(static_cast<size_t>(CFG_WDATAFULL_GRANULES) * GRANULE_BYTES, 0x00);
    expect_bytes("WriteDataFull 线上字节", m.data, exp);

    WChannel back{};
    expect_true("WriteDataFull 可解码", MsgDecoder::decode_write_data(m, back));
    expect_eq("  FLEX(WUSER)", back.user, w.user);
    expect_true("  WDATA 逐字节还原", std::memcmp(back.data, w.data, AXI_DATA_BYTES) == 0);
}

// ============================================================
//  用例 6：WriteData（带 WSTRB）—— 规范 Table 4~6 + Figure 8~10
//
//  取值：MSGTYPE='b0011 RP='b10 DLENGTH=本配置 FLEX=0xC3D4
//    byte0    0011 10 DL      -> 256b:0x38  512b:0x39  1024b:0x3A
//    byte1-2  0xC3 0xD4
//    byte3..  WDATA，最高位字节在前
//    随后     WSTRB[N-1:0]：同样最高位在前。WSTRB[i] 对应数据字节 i，
//             所以线上最后一个 WSTRB 字节的 bit0 = WSTRB[0]。
//    末尾     RsvdZero：256b 1B / 512b 0B / 1024b 3B
// ============================================================
static void test_write_data() {
    std::printf("\n--- 用例6 WriteData%d（Table 4~6 / Figure 8~10）---\n", AXI_DATA_WIDTH);
    WChannel w{};
    w.user = 0xC3D4;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) {
        w.data[i] = pattern_byte(i);
        w.strb[i] = strobe_bit(i) ? 0xFF : 0x00;
    }
    expect_true("部分 strobe 不判为 Full", !MsgBuilder::is_full_strobe(w));

    AouMessage m = MsgBuilder::build_write_data(w, /*rp=*/2);
    expect_eq("WriteData granule 数", m.granules, CFG_WDATA_GRANULES);

    std::vector<uint8_t> exp;
    exp.push_back(static_cast<uint8_t>(0x38 + static_cast<int>(AXI_DLENGTH)));
    exp.push_back(0xC3); exp.push_back(0xD4);
    for (int i = 0; i < AXI_DATA_BYTES; ++i)
        exp.push_back(pattern_byte(AXI_DATA_BYTES - 1 - i));
    // WSTRB 位图：先按"数据字节序"压成 M 个字节，再整体倒序放上线
    uint8_t packed[AXI_STRB_BYTES] = {};
    for (int i = 0; i < AXI_STRB_WIDTH; ++i)
        if (strobe_bit(i)) packed[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    for (int k = 0; k < AXI_STRB_BYTES; ++k) exp.push_back(packed[AXI_STRB_BYTES - 1 - k]);
    exp.resize(static_cast<size_t>(CFG_WDATA_GRANULES) * GRANULE_BYTES, 0x00);
    expect_bytes("WriteData 线上字节", m.data, exp);

    WChannel back{};
    expect_true("WriteData 可解码", MsgDecoder::decode_write_data(m, back));
    expect_true("  WDATA 逐字节还原", std::memcmp(back.data, w.data, AXI_DATA_BYTES) == 0);
    bool strb_ok = true;
    for (int i = 0; i < AXI_STRB_WIDTH; ++i)
        if ((back.strb[i] != 0) != strobe_bit(i)) strb_ok = false;
    expect_true("  WSTRB 逐位还原", strb_ok);
}

// ============================================================
//  用例 7：CrdtGrant —— 规范 Table 18 + Figure 21
//
//  取值（rp_count = 2，RP2/RP3 按规范置 0）：
//        RP0: WREQ=4  RREQ=8  WDATA=16  RDATA=32  WRESP=1
//        RP1: WREQ=1  RREQ=0  WDATA=128 RDATA=64  WRESP=4
//  Table 17 编码：0->000 1->001 4->010 8->011 16->100 32->101 64->110 128->111
//
//  比特串（80bit）：
//    MSGTYPE  0000
//    MISCOP   100
//    WREQ     010 001 000 000
//    RREQ     011 000 000 000
//    WDATA    100 111 000 000
//    RDATA    101 110 000 000
//    WRESP    01  10  00  00
//    RsvdZero 17 个 0
//  -> 08 88 0C 01 38 17 00 C0 00 00
//
//  注意 byte0 = 0x08：高 4 位 MSGTYPE=0，bit3..1 = MISCOP='b100，
//  bit0 已经是 WREQCRED0 的最高位（=0）。规范里没有前导保留位。
// ============================================================
static void test_crdt_grant() {
    std::printf("\n--- 用例7 CrdtGrant（Table 17/18 / Figure 21）---\n");
    CreditMatrix g{};
    for (auto& per_rp : g) per_rp.fill(0);
    g[0][credit_kind_index(CreditKind::WriteReq)]  = 4;
    g[0][credit_kind_index(CreditKind::ReadReq)]   = 8;
    g[0][credit_kind_index(CreditKind::WriteData)] = 16;
    g[0][credit_kind_index(CreditKind::ReadData)]  = 32;
    g[0][credit_kind_index(CreditKind::WriteResp)] = 1;
    g[1][credit_kind_index(CreditKind::WriteReq)]  = 1;
    g[1][credit_kind_index(CreditKind::ReadReq)]   = 0;
    g[1][credit_kind_index(CreditKind::WriteData)] = 128;
    g[1][credit_kind_index(CreditKind::ReadData)]  = 64;
    g[1][credit_kind_index(CreditKind::WriteResp)] = 4;

    AouMessage m = build_crdt_grant_message(g, /*rp_count=*/2);
    expect_eq("CrdtGrant granule 数", m.granules, MISC_CRDTGRANT_GRANULES);
    expect_bytes("CrdtGrant 线上字节", m.data,
                 {0x08, 0x88, 0x0C, 0x01, 0x38, 0x17, 0x00, 0xC0, 0x00, 0x00});

    CreditMatrix back{};
    expect_true("CrdtGrant 可解码", decode_crdt_grant_message(m, 2, back));
    bool same = true;
    for (unsigned rp = 0; rp < 2; ++rp)
        for (size_t k = 0; k < back[rp].size(); ++k)
            if (back[rp][k] != g[rp][k]) same = false;
    expect_true("  credit 矩阵逐项还原", same);

    // Table 17：WRESPCRED 只有 2bit，最多只能表达 8 个 credit。
    // 编码器必须"取不超过 pending 的最大合法值"，而不是溢出或饱和成非法编码。
    expect_eq("WRESPCRED 编码上限（24 -> 一次只发 8）",
              decode_credit_encoding(encode_credit_amount(24, 3)), 8);
    expect_eq("WREQCRED 编码上限（200 -> 一次只发 128）",
              decode_credit_encoding(encode_credit_amount(200)), 128);
    expect_eq("非 2 的幂（例如 5）向下取到合法编码 4",
              decode_credit_encoding(encode_credit_amount(5)), 4);
}

// ============================================================
//  用例 8：首字节自描述长度
//
//  跨 Flit 续传时，接收端只能靠消息首字节判断整条消息有多长（后面的
//  MsgStart 位已经不在本 Flit 里了）。这里验证 byte0 的解析函数与
//  各 build_* 实际产生的 granule 数一致。
// ============================================================
static void test_header_self_describing() {
    std::printf("\n--- 用例8 首字节自描述长度（§4 Flit 打包）---\n");
    AxChannel ax{};  WChannel w{};  RChannel r{};  BChannel b{};
    for (int i = 0; i < AXI_DATA_BYTES; ++i) { w.data[i] = 0; w.strb[i] = 0xFF; }

    struct { const char* name; AouMessage m; int gran; } cases[] = {
        {"WriteReq",      MsgBuilder::build_write_req(ax, 0),       WREQ_GRANULES},
        {"ReadReq",       MsgBuilder::build_read_req(ax, 0),        RREQ_GRANULES},
        {"WriteResp",     MsgBuilder::build_write_resp(b, 0),       WRESP_GRANULES},
        {"ReadData",      MsgBuilder::build_read_data(r, 0),        CFG_RDATA_GRANULES},
        {"WriteDataFull", MsgBuilder::build_write_data_full(w, 0),  CFG_WDATAFULL_GRANULES},
        {"WriteData",     MsgBuilder::build_write_data(w, 0),       CFG_WDATA_GRANULES},
    };
    for (auto& c : cases) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s 首字节 0x%02X 推出长度",
                      c.name, c.m.data[0]);
        expect_eq(label, message_granules_from_header(c.m.data[0]), c.gran);
    }
    CreditMatrix g{};  for (auto& p : g) p.fill(0);
    AouMessage cg = build_crdt_grant_message(g, 1);
    expect_eq("CrdtGrant 首字节推出长度",
              message_granules_from_header(cg.data[0]), MISC_CRDTGRANT_GRANULES);
}

// ============================================================
//  用例 9：字段边界（满值不串位）
//
//  所有字段同时取最大值时，各字段不能互相溢出到邻位。
//  这是 v0.8 把 PROF[11:0]+PROFEXTLEN 合并成 FLEX[15:0] 之后必须复核的一项：
//  旧实现最多只能传 12bit USER，0xFFFF 会被截断成 0x0FFF。
// ============================================================
static void test_field_saturation() {
    std::printf("\n--- 用例9 字段满值边界 ---\n");
    AxChannel ax{};
    ax.user = 0xFFFF; ax.id = 0x3FF; ax.len = 0xFF; ax.size = 0x7;
    ax.prot = 0x7; ax.cache = 0xF; ax.qos = 0xF; ax.lock = 1;
    ax.addr = 0xFFFFFFFFFFFFFFFFull;
    AouMessage m = MsgBuilder::build_write_req(ax, 3);
    AxChannel back{};
    expect_true("满值 WriteReq 可解码", MsgDecoder::decode_req(m, back));
    expect_eq("  FLEX 满 16bit（旧实现只有 12bit）", back.user, 0xFFFFu);
    expect_eq("  AxID 满 10bit",  back.id,   0x3FFu);
    expect_eq("  AxADDR 满 64bit", back.addr, 0xFFFFFFFFFFFFFFFFull);
    expect_eq("  AxLEN 满 8bit",  back.len,  0xFFu);

    RChannel r{};
    r.user = 0xFFFF; r.id = 0x3FF; r.resp = 3; r.last = true;
    AouMessage rm = MsgBuilder::build_read_data(r, 3);
    RChannel rback{};
    expect_true("满值 ReadData 可解码", MsgDecoder::decode_read_data(rm, rback));
    expect_eq("  FLEX 满 16bit", rback.user, 0xFFFFu);
    expect_eq("  RID 满 10bit",  rback.id,   0x3FFu);
    expect_eq("  RRESP 满 2bit", rback.resp, 3u);
}

// ============================================================
//  用例 10：RsvdZero 必须真的是 0
//
//  规范对每个 RsvdZero 字段都写明 "Zero-Padding"。这里把所有字段拉满，
//  再检查保留位仍然为 0 —— 保证没有字段越界写进保留区。
// ============================================================
static void test_reserved_zero() {
    std::printf("\n--- 用例10 RsvdZero 保持为 0 ---\n");
    AxChannel ax{};
    ax.user = 0xFFFF; ax.id = 0x3FF; ax.len = 0xFF; ax.size = 0x7;
    ax.prot = 0x7; ax.cache = 0xF; ax.qos = 0xF; ax.lock = 1;
    ax.addr = 0xFFFFFFFFFFFFFFFFull;
    AouMessage m = MsgBuilder::build_write_req(ax, 3);
    // Figure 6：byte0 的 bit1 是 RsvdZero
    expect_eq("WriteReq byte0.bit1 (RsvdZero)", (m.data[0] >> 1) & 1u, 0u);

    RChannel r{};
    r.user = 0xFFFF; r.id = 0x3FF; r.resp = 3; r.last = true;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) r.data[i] = 0xFF;
    AouMessage rm = MsgBuilder::build_read_data(r, 3);
    // Figure 14：byte4 的 bit2..0 是 RsvdZero
    expect_eq("ReadData byte4.bit2..0 (RsvdZero)", rm.data[4] & 0x7u, 0u);
    // 尾部 Zero-Padding
    int tail = CFG_RDATA_GRANULES * GRANULE_BYTES - (5 + AXI_DATA_BYTES);
    bool tail_zero = true;
    for (int i = 0; i < tail; ++i)
        if (rm.data[5 + AXI_DATA_BYTES + i] != 0) tail_zero = false;
    expect_true("ReadData 尾部 Zero-Padding 全 0", tail_zero);

    BChannel b{};  b.user = 0xFFFF;  b.id = 0x3FF;  b.resp = 3;
    AouMessage bm = MsgBuilder::build_write_resp(b, 3);
    // Figure 17：byte0 的 bit1..0 与 byte4 的 bit3..0 是 RsvdZero
    expect_eq("WriteResp byte0.bit1..0 (RsvdZero)", bm.data[0] & 0x3u, 0u);
    expect_eq("WriteResp byte4.bit3..0 (RsvdZero)", bm.data[4] & 0xFu, 0u);
}

// ============================================================
int sc_main(int, char*[]) {
    sc_report_handler::set_actions(SC_WARNING, SC_DO_NOTHING);

    std::printf("================ AoU v0.8 线上格式黄金向量对拍 ================\n");
    std::printf("  基线：doc/AXI over UCIe Protocol Specification v0.8.pdf\n");
    std::printf("        §5.8 约定：Bytes count up / Bits count down / MSB-first\n");
    std::printf("  本次配置：AXI_DATA_WIDTH = %d bit（DLENGTH='b%d%d）\n",
                AXI_DATA_WIDTH,
                (static_cast<int>(AXI_DLENGTH) >> 1) & 1, static_cast<int>(AXI_DLENGTH) & 1);
    std::printf("==============================================================\n");

    test_write_req();
    test_read_req();
    test_write_resp();
    test_read_data();
    test_write_data_full();
    test_write_data();
    test_crdt_grant();
    test_header_self_describing();
    test_field_saturation();
    test_reserved_zero();

    std::printf("\n==============================================================\n");
    std::printf("  检查项 %d 通过 / %d 失败\n", g_pass, g_fail);
    std::printf("  %s\n", g_fail == 0 ? "GOLDEN VECTORS MATCH SPEC v0.8"
                                      : "GOLDEN VECTOR MISMATCH —— 线上格式与规范不符");
    std::printf("==============================================================\n");
    return g_fail == 0 ? 0 : 1;
}
