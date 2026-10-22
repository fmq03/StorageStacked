/**
 * 消息黄金字节向量自检。固定期望值不经过被测编码器生成，逐字节比较编码结果。
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
        dump("期望", exp.data(), static_cast<unsigned>(exp.size()));
        dump("模型", got, static_cast<unsigned>(exp.size()));
        for (size_t i = 0; i < exp.size(); ++i)
            if (got[i] != exp[i]) {
                std::printf("      首个不一致：byte%zu 期望=0x%02X 模型=0x%02X"
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

static void test_write_req() {
    std::printf("\n--- 用例1 WriteReq（写请求字段）---\n");
    AxChannel aw{};
    aw.lock = 1;  aw.user = 0xA55A;  aw.id = 0x2C3;
    aw.size = 0b101;  aw.prot = 0b011;  aw.len = 0x0F;
    aw.cache = 0xA;   aw.qos = 0x6;
    aw.addr = 0x0123456789ABCDEFull;

    AouMessage m = MsgBuilder::build_write_req(aw,  2);
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

static void test_read_req() {
    std::printf("\n--- 用例2 ReadReq（读请求字段）---\n");
    AxChannel ar{};
    ar.lock = 0;  ar.user = 0x5AA5;  ar.id = 0x0FC;
    ar.size = 0b110;  ar.prot = 0b001;  ar.len = 0x3F;
    ar.cache = 0x5;   ar.qos = 0x9;
    ar.addr = 0xFEDCBA9876543210ull;

    AouMessage m = MsgBuilder::build_read_req(ar,  1);
    expect_bytes("ReadReq 线上字节", m.data,
                 {0x24, 0x5A, 0xA5, 0x3F, 0x31, 0x3F, 0x59,
                  0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});

    AxChannel back{};
    expect_true("ReadReq 可解码", MsgDecoder::decode_req(m, back));
    expect_eq("  ARADDR", back.addr, ar.addr);
    expect_eq("  ARID",   back.id,   ar.id);
    expect_eq("  FLEX(ARUSER)", back.user, ar.user);
}

static void test_write_resp() {
    std::printf("\n--- 用例3 WriteResp（写响应字段）---\n");
    BChannel b{};
    b.id = 0x155;  b.resp = 0b10;  b.user = 0x1234;

    AouMessage m = MsgBuilder::build_write_resp(b,  1);
    expect_eq("WriteResp granule 数", m.granules, WRESP_GRANULES);
    expect_bytes("WriteResp 线上字节", m.data, {0x54, 0x12, 0x34, 0x55, 0x60});

    BChannel back{};
    expect_true("WriteResp 可解码", MsgDecoder::decode_write_resp(m, back));
    expect_eq("  BID",   back.id,   b.id);
    expect_eq("  BRESP", back.resp, b.resp);
    expect_eq("  FLEX(BUSER)", back.user, b.user);
}

static void test_read_data() {
    std::printf("\n--- 用例4 ReadData%d（读数据字段）---\n", AXI_DATA_WIDTH);
    RChannel r{};
    r.id = 0x2AA;  r.resp = 0b01;  r.last = true;  r.user = 0xBEEF;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) r.data[i] = pattern_byte(i);

    AouMessage m = MsgBuilder::build_read_data(r,  3);
    expect_eq("ReadData granule 数", m.granules, CFG_RDATA_GRANULES);

    std::vector<uint8_t> exp;
    exp.push_back(static_cast<uint8_t>(0x4C + static_cast<int>(AXI_DLENGTH)));
    exp.push_back(0xBE); exp.push_back(0xEF);
    exp.push_back(0xAA); exp.push_back(0x98);
    for (int i = 0; i < AXI_DATA_BYTES; ++i)          // 最高位字节在前
        exp.push_back(pattern_byte(AXI_DATA_BYTES - 1 - i));
    exp.resize(static_cast<size_t>(CFG_RDATA_GRANULES) * GRANULE_BYTES, 0x00);
    expect_bytes("ReadData 线上字节", m.data, exp);

    RChannel back{};
    expect_true("ReadData 可解码", MsgDecoder::decode_read_data(m, back));
    expect_eq("  RID",   back.id,   r.id);
    expect_eq("  RRESP", back.resp, r.resp);
    expect_eq("  RLAST", back.last, r.last);
    expect_eq("  FLEX(RUSER)", back.user, r.user);
    expect_true("  RDATA 逐字节还原", std::memcmp(back.data, r.data, AXI_DATA_BYTES) == 0);
}

static void test_write_data_full() {
    std::printf("\n--- 用例5 WriteDataFull%d（全字节有效写数据）---\n", AXI_DATA_WIDTH);
    WChannel w{};
    w.user = 0x0F1E;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) { w.data[i] = pattern_byte(i); w.strb[i] = 0xFF; }
    expect_true("全 strobe 判定为 WriteDataFull", MsgBuilder::is_full_strobe(w));

    AouMessage m = MsgBuilder::build_write_data_full(w,  0);
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

static void test_write_data() {
    std::printf("\n--- 用例6 WriteData%d（带字节选通写数据）---\n", AXI_DATA_WIDTH);
    WChannel w{};
    w.user = 0xC3D4;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) {
        w.data[i] = pattern_byte(i);
        w.strb[i] = strobe_bit(i) ? 0xFF : 0x00;
    }
    expect_true("部分 strobe 不判为 Full", !MsgBuilder::is_full_strobe(w));

    AouMessage m = MsgBuilder::build_write_data(w,  2);
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

static void test_crdt_grant() {
    std::printf("\n--- 用例7 CrdtGrant（接收credit）---\n");
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

    AouMessage m = build_crdt_grant_message(g,  2);
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

static void test_field_saturation() {
    std::printf("\n--- 用例9 字段满值边界 ---\n");
    AxChannel ax{};
    ax.user = 0xFFFF; ax.id = 0x3FF; ax.len = 0xFF; ax.size = 0x7;
    ax.prot = 0x7; ax.cache = 0xF; ax.qos = 0xF; ax.lock = 1;
    ax.addr = 0xFFFFFFFFFFFFFFFFull;
    AouMessage m = MsgBuilder::build_write_req(ax, 3);
    AxChannel back{};
    expect_true("满值 WriteReq 可解码", MsgDecoder::decode_req(m, back));
    expect_eq("  FLEX 完整 16bit", back.user, 0xFFFFu);
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

static void test_reserved_zero() {
    std::printf("\n--- 用例10 RsvdZero 保持为 0 ---\n");
    AxChannel ax{};
    ax.user = 0xFFFF; ax.id = 0x3FF; ax.len = 0xFF; ax.size = 0x7;
    ax.prot = 0x7; ax.cache = 0xF; ax.qos = 0xF; ax.lock = 1;
    ax.addr = 0xFFFFFFFFFFFFFFFFull;
    AouMessage m = MsgBuilder::build_write_req(ax, 3);

    expect_eq("WriteReq byte0.bit1 (RsvdZero)", (m.data[0] >> 1) & 1u, 0u);

    RChannel r{};
    r.user = 0xFFFF; r.id = 0x3FF; r.resp = 3; r.last = true;
    for (int i = 0; i < AXI_DATA_BYTES; ++i) r.data[i] = 0xFF;
    AouMessage rm = MsgBuilder::build_read_data(r, 3);

    expect_eq("ReadData byte4.bit2..0 (RsvdZero)", rm.data[4] & 0x7u, 0u);
    // 尾部 Zero-Padding
    int tail = CFG_RDATA_GRANULES * GRANULE_BYTES - (5 + AXI_DATA_BYTES);
    bool tail_zero = true;
    for (int i = 0; i < tail; ++i)
        if (rm.data[5 + AXI_DATA_BYTES + i] != 0) tail_zero = false;
    expect_true("ReadData 尾部 Zero-Padding 全 0", tail_zero);

    BChannel b{};  b.user = 0xFFFF;  b.id = 0x3FF;  b.resp = 3;
    AouMessage bm = MsgBuilder::build_write_resp(b, 3);

    expect_eq("WriteResp byte0.bit1..0 (RsvdZero)", bm.data[0] & 0x3u, 0u);
    expect_eq("WriteResp byte4.bit3..0 (RsvdZero)", bm.data[4] & 0xFu, 0u);
}

int sc_main(int, char*[]) {
    sc_report_handler::set_actions(SC_WARNING, SC_DO_NOTHING);

    std::printf("================ 消息格式黄金向量对拍 ================\n");
    std::printf("  逐字节核对消息编码与独立期望向量\n");
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
    std::printf("  %s\n", g_fail == 0 ? "GOLDEN VECTORS MATCH"
                                      : "GOLDEN VECTOR MISMATCH —— 线上格式与期望不符");
    std::printf("==============================================================\n");
    return g_fail == 0 ? 0 : 1;
}
