/**
 * 固定字节映射、跨帧解析与非法输入自检。期望字节独立于被测编解码器。
 */
#include "aou_wire.h"
#include "aou_format6.h"
#include "aou_stream_decoder.h"
#include "axi_contract.h"
#include "msg_builder.h"
#include "credit_manager.h"
#include "simple_mem_if.h"
#include <iostream>
#include <random>

static unsigned passed = 0, failed = 0;
static void check(bool ok, const char* what) {
    if (ok) ++passed;
    else { ++failed; std::cerr << "[FAIL] " << what << '\n'; }
}
template<class F> static void rejects(F f, const char* what) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, what);
}
static bool same_message(const AouMessage& a, const AouMessage& b) {
    return a.type == b.type && a.granules == b.granules &&
        std::equal(a.data, a.data + a.granules * GRANULE_BYTES, b.data);
}

static void header_golden() {
    AouFlit f;
    f.fdid = 2; f.msg_start = 0xFEDCBA987654ULL; f.msg_credit = 0x1234;
    for (unsigned i = 0; i < PAYLOAD_BYTES; ++i) f.payload[i] = (i * 29 + 7) & 255;
    const std::array<uint8_t, 10> ph{0x42,0x65,0x70,0x98,0x34,0x12,0xA0,0xCB,0xD0,0xFE};
    AouWireFlit expected{};
    std::copy(ph.begin(), ph.end(), expected.begin());
    for (unsigned i = 0; i < 240; ++i) expected[10 + i] = (i * 29 + 7) & 255;
    check(serialize_aou(f) == expected, "250B 完整独立黄金向量");
    const auto decoded = deserialize_aou(expected);
    check(decoded.fdid == 2 && decoded.msg_start == 0xFEDCBA987654ULL &&
          decoded.msg_credit == 0x1234 && decoded.used_granules == -1 &&
          std::equal(f.payload, f.payload + 240, decoded.payload), "黄金字节直接解码");
    f.used_granules = 48; f.valid = false;
    check(serialize_aou(f) == expected, "辅助字段绝不影响线格式");

    const unsigned byte[48] = {
        0,0,0,0, 1,1,1,1,1,1,1,1, 2,2,2,2, 3,3,3,3,3,3,3,3,
        6,6,6,6, 7,7,7,7,7,7,7,7, 8,8,8,8, 9,9,9,9,9,9,9,9};
    const unsigned bit[12] = {4,5,6,7,0,1,2,3,4,5,6,7};
    for (unsigned i = 0; i < 48; ++i) {
        AouFlit one; one.msg_start = 1ULL << i;
        AouWireFlit exp{}; exp[byte[i]] = 1U << bit[i % 12];
        check(serialize_aou(one) == exp, "MsgStart 逐位 walking-one");
        check(deserialize_aou(exp).msg_start == one.msg_start, "MsgStart 逐位解码");
    }
    for (unsigned i = 0; i < 16; ++i) {
        AouFlit one; one.msg_credit = 1U << i;
        AouWireFlit exp{}; exp[4 + i / 8] = 1U << (i % 8);
        check(serialize_aou(one) == exp && deserialize_aou(exp).msg_credit == one.msg_credit,
              "MsgCredit 逐位编码解码");
    }
    for (unsigned i = 0; i < 4; ++i) {
        AouFlit one; one.fdid = i;
        AouWireFlit exp{}; exp[0] = i;
        check(serialize_aou(one) == exp && deserialize_aou(exp).fdid == i, "四个 FDId");
    }
    for (unsigned b : {0U, 2U, 6U, 8U}) {
        for (unsigned bitno = b == 0 ? 2 : 0; bitno < 4; ++bitno) {
            AouWireFlit bad{}; bad[b] = 1U << bitno;
            rejects([&] { deserialize_aou(bad); }, "PH 非零保留位拒绝");
        }
    }
    f.fdid = 4; rejects([&] { serialize_aou(f); }, "FDId 越界拒绝");
    f.fdid = 0; f.msg_start = 1ULL << 48;
    rejects([&] { serialize_aou(f); }, "MsgStart 越界拒绝");
}

static void physical_mapping() {
    const unsigned phpos[] = {62,63,64,65,128,129,190,191,192,193};
    // 每个逻辑字节单独打标，核对全部 256B（包含 FH/CRC 零值），能检出遗漏、
    // 重复映射、覆盖 CRC 和两端一起错的对称映射问题。
    for (unsigned i = 0; i < 250; ++i) {
        aou_format6::Payload p{}; p[i] = 0xA5;
        aou_format6::Frame expected{};
        const unsigned pos = i < 10 ? phpos[i] : 2 + ((i - 10) / 60) * 64 + (i - 10) % 60;
        expected[pos] = 0xA5;
        check(aou_format6::scatter(p) == expected, "Format 6 全字节独立位置检查");
        check(aou_format6::gather(expected) == p, "Format 6 独立物理字节收集");
    }
    const auto frame = aou_format6::scatter({}, {0x12,0x34}, {0x56,0x78,0x9A,0xBC});
    check(frame[0] == 0x12 && frame[1] == 0x34 && frame[126] == 0x56 &&
          frame[127] == 0x78 && frame[254] == 0x9A && frame[255] == 0xBC,
          "FH 与两组 CRC 的独立槽位");
}

static std::vector<AouMessage> sample_messages() {
    AxChannel ax; ax.id = 0x312; ax.addr = 0xABCDEF0123400000ULL; ax.user = 0xBA98;
    WChannel w; w.last = true;
    RChannel r; r.id = 0x312; r.last = true; r.user = 0xCDEF;
    for (unsigned i = 0; i < AXI_DATA_BYTES; ++i) {
        w.data[i] = (i * 17 + 3) & 255; w.strb[i] = i % 3 != 0;
        r.data[i] = (i * 23 + 9) & 255;
    }
    BChannel b; b.id = 0x123; b.user = 0x4567; b.resp = 2;
    CreditMatrix credits{};
    credits[0][credit_kind_index(CreditKind::ReadData)] = 128;
    return {MsgBuilder::build_write_req(ax, 0), MsgBuilder::build_read_req(ax, 0),
            MsgBuilder::build_write_data(w, 0), MsgBuilder::build_write_data_full(w, 0),
            MsgBuilder::build_read_data(r, 0), MsgBuilder::build_write_resp(b, 0),
            build_crdt_grant_message(credits, 1)};
}

static void stream_boundaries() {
    const auto msgs = sample_messages();
    for (const auto& msg : msgs) for (int start = 0; start < 48; ++start) {
        AouStreamDecoder d;
        AouFlit first; first.used_granules = start;
        const int take = first.pack_fragment(msg, 0);
        // 空粒度可以非零，解析不得误把旧数据当成消息。
        std::fill_n(first.payload, start * 5, 0xEE);
        auto got = d.consume(deserialize_aou(serialize_aou(first)));
        check(got.used_granules == unsigned(take), "统计忽略前部空粒度");
        if (take < msg.granules) {
            check(got.messages.empty() && d.carry_active(), "尾部截断不提前交付");
            AouFlit second; second.pack_fragment(msg, take);
            got = d.consume(deserialize_aou(serialize_aou(second)));
            check(got.used_granules == unsigned(msg.granules - take), "续传尾片统计");
        }
        check(got.messages.size() == 1 && same_message(got.messages[0], msg) &&
              !d.carry_active(), "全部起点的消息内容完整重组");
    }
    AouStreamDecoder d;
    AouFlit empty; empty.msg_credit = 0x1234;
    std::fill_n(empty.payload, 240, 0xFF);
    auto got = d.consume(deserialize_aou(serialize_aou(empty)));
    check(got.messages.empty() && got.used_granules == 0, "无 carry 的纯 credit/空 Flit");
    for (uint8_t bad : {uint8_t(0xF0), uint8_t(0x43), uint8_t(0x33), uint8_t(0x63), uint8_t(0x0E)}) {
        AouFlit f; f.msg_start = 1; f.payload[0] = bad;
        rejects([&] { d.consume(f); }, "非法消息/保留长度拒绝");
    }
    AouFlit overlap; overlap.pack_message(msgs[0]); overlap.msg_start |= 2;
    rejects([&] { d.consume(overlap); }, "新消息起点重叠拒绝");
    AouFlit tail; tail.used_granules = 47; tail.pack_fragment(msgs[0], 0);
    d.consume(tail);
    rejects([&] { d.consume(overlap); }, "起点与续传重叠拒绝");
    AouFlit continuation; continuation.pack_fragment(msgs[0], 1);
    got = d.consume(continuation);
    check(got.messages.size() == 1 && same_message(got.messages[0], msgs[0]),
          "拒绝坏包不污染续传状态");
    d.consume(tail); d.reset();
    check(!d.carry_active() && d.consume(empty).messages.empty(), "复位清除未完成续传");

    std::mt19937 rng(0xA0250);
    std::vector<AouMessage> expected, observed;
    AouFlit current;
    for (unsigned i = 0; i < 4096; ++i) {
        // 连续压力只混合六类业务消息，避免随机生成超过半 Flit Misc 数量上限。
        const auto msg = msgs[rng() % (msgs.size() - 1)];
        expected.push_back(msg);
        int done = 0;
        while (done < msg.granules) {
            done += current.pack_fragment(msg, done);
            if (current.remaining_granules() == 0) {
                auto result = d.consume(deserialize_aou(serialize_aou(current)));
                observed.insert(observed.end(), result.messages.begin(), result.messages.end());
                current.clear();
            }
        }
        if ((rng() % 7) == 0 && current.used_granules) {
            auto result = d.consume(deserialize_aou(serialize_aou(current)));
            observed.insert(observed.end(), result.messages.begin(), result.messages.end());
            current.clear();
        }
    }
    if (current.used_granules) {
        auto result = d.consume(deserialize_aou(serialize_aou(current)));
        observed.insert(observed.end(), result.messages.begin(), result.messages.end());
    }
    check(observed.size() == expected.size() &&
          std::equal(expected.begin(), expected.end(), observed.begin(), same_message) &&
          !d.carry_active(), "固定种子 4096 条混合消息逐字节顺序守恒");
}

static void axi_negative() {
    AxChannel ax;
    check(axi_request_error(ax) == nullptr, "合法对齐 INCR");
    ax.burst = 0; check(axi_request_error(ax) != nullptr, "FIXED 拒绝");
    ax.burst = 2; check(axi_request_error(ax) != nullptr, "WRAP 拒绝");
    ax.burst = 1; ax.size = AXI_SIZE_CODE + 1;
    check(axi_request_error(ax) != nullptr, "超位宽 SIZE 拒绝");
    ax.size = AXI_SIZE_CODE; ax.addr = 1;
    check(axi_request_error(ax) != nullptr, "非对齐拒绝");
    ax.addr = 4096 - AXI_DATA_BYTES; ax.len = 1;
    check(axi_request_error(ax) != nullptr, "跨 4KB 拒绝");
    ax.len = 0; check(axi_request_error(ax) == nullptr, "恰好到 4KB 边界允许");
    ax.addr = 0; ax.size = 0; ax.len = 255;
    check(axi_request_error(ax) == nullptr, "窄传输 256 beat 边界允许");
    check(axi_wlast_matches(1, true) && axi_wlast_matches(2, false) &&
          !axi_wlast_matches(1, false) && !axi_wlast_matches(2, true) &&
          !axi_wlast_matches(0, true), "WLAST 正常/提前/缺失/无路由");
}

int sc_main(int, char**) {
    // 实例化存储请求和响应 FIFO，检查公共事务类型的读写。
    sc_fifo<SimpleMemRequest> requests("memory_requests", 1);
    sc_fifo<SimpleMemResponse> responses("memory_responses", 1);
    header_golden(); physical_mapping(); stream_boundaries(); axi_negative();
    std::cout << "WIRE " << AXI_DATA_WIDTH << "b: " << passed << " PASS / " << failed << " FAIL\n";
    return failed ? 1 : 0;
}
