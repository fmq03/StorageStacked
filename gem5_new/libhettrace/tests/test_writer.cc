// libhettrace 自测。无外部依赖，g++ 直接编译运行。
//
//   make test-writer （在仓库根目录）
//   或  g++ -std=c++17 -I../include test_writer.cc -o t && ./t

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hettrace/addrmap.h"
#include "hettrace/record.h"
#include "hettrace/writer.h"

using namespace hettrace;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                         msg);                                              \
            ++g_failed;                                                     \
        }                                                                   \
    } while (0)

static std::string g_dir;

static std::vector<Record> ReadBin(const std::string& path, FileHeader* out_h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return {};
    FileHeader h;
    if (std::fread(&h, sizeof(h), 1, f) != 1) {
        std::fclose(f);
        return {};
    }
    if (out_h != nullptr) *out_h = h;
    std::vector<Record> recs;
    Record r;
    while (std::fread(&r, sizeof(r), 1, f) == 1) recs.push_back(r);
    std::fclose(f);
    return recs;
}

// ---------------------------------------------------------------------------

static void TestDisabledWhenNoEnv() {
    unsetenv("HETTRACE_DIR");
    TraceWriter w;
    bool ok = w.Open(kSrcHost, "host", kLevelPostLlc, kClockPeriodTicks_host);
    CHECK(!ok, "HETTRACE_DIR 未设置时 Open 应返回 false");
    CHECK(!w.is_open(), "未设置时应保持关闭");
    // 关闭状态下 Emit 必须是安全的 no-op
    w.Emit(0, kSharedBufferBase, 64, kRead, 0);
    AxiTxn txn;
    txn.addr = kSharedBufferBase;
    txn.bytes = 16;
    txn.axi_size = 4;
    CHECK(!w.BeginRead(0, txn), "关闭状态下 BeginRead 应是安静的 no-op");
    CHECK(!w.CompleteRead(1, txn, kRespOkay),
          "关闭状态下 CompleteRead 应是安静的 no-op");
    CHECK(w.stats().emitted == 0, "关闭状态不应产生记录");
}

static void TestHeaderAndRoundTrip() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "bin", 1);
    unsetenv("HETTRACE_FILTER");

    {
        TraceWriter w;
        CHECK(w.Open(kSrcVortex, "vortex", kLevelPostLlc,
                     kClockPeriodTicks_vortex, /*axi_data_bytes=*/64),
              "Open 应成功");
        w.Emit(1000, kVortexBarBase, 64, kRead, 7);
        w.Emit(2000, kVortexBarBase + 64, 64, kWrite, 7);
        w.Emit(3000, kVortexVramBase, 32, kRead, 9);
    }  // 析构应 Flush + 写 meta

    FileHeader h;
    std::vector<Record> recs = ReadBin(g_dir + "/vortex.hettrace", &h);

    CHECK(std::memcmp(h.magic, kMagic, 8) == 0, "magic 应匹配");
    CHECK(h.version == kFormatVersion, "version 应匹配");
    CHECK(h.record_size == sizeof(Record), "record_size 应为 56");
    CHECK(h.ticks_per_second == kTicksPerSecond, "ticks_per_second 应匹配");
    CHECK(h.clock_period_ticks == kClockPeriodTicks_vortex,
          "clock_period_ticks 应匹配");
    CHECK(h.src_id == kSrcVortex, "src_id 应匹配");
    CHECK(h.level == kLevelPostLlc, "level 应匹配");
    CHECK((h.flags & kHdrFilteredDram) != 0, "默认应为 dram 过滤");
    CHECK(h.axi_data_bytes == 64, "axi_data_bytes 应记录调用方声明的 64B");
    CHECK(h.axi_addr_bits == kMapAddrBits, "axi_addr_bits 应为地址图宽度");
    CHECK(std::strcmp(h.name, "vortex") == 0, "name 应匹配");

    CHECK(recs.size() == 3, "应有 3 条记录");
    if (recs.size() == 3) {
        CHECK(recs[0].tick == 1000 && recs[0].addr == kVortexBarBase &&
                  recs[0].size == 64 && recs[0].op == kRead &&
                  recs[0].ctx == 7 && recs[0].seq == 0,
              "记录 0 字段应往返一致");
        CHECK(recs[1].op == kWrite && recs[1].seq == 1, "记录 1 应为写且 seq=1");
        CHECK(recs[2].addr == kVortexVramBase && recs[2].seq == 2,
              "记录 2 应往返一致");
        CHECK(recs[0].src_id == kSrcVortex, "src_id 应写入每条记录");
    }
}

// Emit() 是"每次访问一条记录"这一旧语义在 v2 格式里的投影。它必须只写数据
// 通道 —— 设备 tap 观测不到独立的地址通道与响应通道，替它们造 AW/B 记录就是
// 把推测写成观测。同时条数必须与旧格式逐条对齐，否则任何跨版本的回归对比
// 都失去基准。
static void TestDataChannelProjection() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "bin", 1);
    unsetenv("HETTRACE_FILTER");

    {
        TraceWriter w;
        w.Open(kSrcHost, "host_proj", kLevelPostLlc, kClockPeriodTicks_host,
               /*axi_data_bytes=*/64);
        w.Emit(10, kSharedBufferBase, 64, kRead, 5);
        w.Emit(20, kSharedBufferBase + 64, 8, kWrite, 5);
    }

    std::vector<Record> recs = ReadBin(g_dir + "/host_proj.hettrace", nullptr);
    CHECK(recs.size() == 2, "Emit 每次应恰好一条记录");
    if (recs.size() != 2) return;

    for (const Record& r : recs) {
        CHECK(IsDataChan(r.chan), "Emit 只应写数据通道");
        CHECK((r.flags & kFlagLast) != 0, "单拍访问应带 LAST");
        CHECK(r.axi_len == 0, "单拍访问 AxLEN 应为 0");
        CHECK(r.burst == kBurstIncr, "默认应为 INCR");
        CHECK(r.resp == kRespOkay, "默认响应应为 OKAY");
    }
    CHECK(recs[0].chan == kChanR && recs[0].op == kRead, "读应落在 R 通道");
    CHECK(recs[0].strb == 0, "读通道 STRB 恒为 0");
    CHECK(recs[0].axi_size == 6, "64 字节 => AxSIZE=6");
    CHECK(recs[1].chan == kChanW && recs[1].op == kWrite, "写应落在 W 通道");
    CHECK(recs[1].strb == 0xffull, "8 字节写应使能 8 条 lane");
    CHECK(recs[1].axi_size == 3, "8 字节 => AxSIZE=3");
    CHECK(recs[0].txn != recs[1].txn, "两次独立访问应是两笔事务");
}

// 全通道级：monitor 用。读事务的 R 必须在响应时刻写出，否则 trace 里读延迟
// 会整体消失 —— 这正是本项目要喂给 DRAM 模拟器的那个量。
static void TestFullChannelTransactions() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "bin", 1);
    unsetenv("HETTRACE_FILTER");

    Stats st;
    {
        TraceWriter w;
        w.Open(kSrcHost, "mon", kLevelInterconnect, kClockPeriodTicks_host,
               /*axi_data_bytes=*/16, kMapAddrBits, /*synth=*/true);

        AxiTxn wt;
        wt.addr     = kSharedBufferBase;
        wt.bytes    = 64;
        wt.ctx      = 3;
        wt.txn      = w.NextTxn();
        wt.axi_id   = 9;
        wt.axi_size = 4;  // 16 B/拍 => 4 拍
        CHECK(wt.axi_len() == 3, "64 字节 / 16 字节每拍 => AxLEN=3");
        const uint64_t strb[4] = {0xffffull, 0xffffull, 0xffffull, 0x00ffull};
        w.BeginWrite(1000, wt, strb);
        w.CompleteWrite(1500, wt, kRespOkay);

        AxiTxn rt;
        rt.addr     = kSharedBufferBase + 0x100;
        rt.bytes    = 32;
        rt.ctx      = 3;
        rt.txn      = w.NextTxn();
        rt.axi_id   = 10;
        rt.axi_size = 4;  // 2 拍
        w.BeginRead(2000, rt);
        w.CompleteRead(2700, rt, kRespSlvErr);

        w.Close();
        st = w.stats();
    }

    // 写: AW + 4×W + B = 6; 读: AR + 2×R = 3
    CHECK(st.emitted == 9, "两笔事务应写出 9 条记录");
    CHECK(st.transactions == 2, "AW+AR 应计为 2 笔事务");
    CHECK(st.data_records == 6, "数据通道应有 6 条（4 拍写 + 2 拍读）");
    CHECK(st.bytes == 96, "数据字节应为 4×16 + 2×16");

    std::vector<Record> recs = ReadBin(g_dir + "/mon.hettrace", nullptr);
    CHECK(recs.size() == 9, "应写出 9 条");
    if (recs.size() != 9) return;

    CHECK(recs[0].chan == kChanAw, "第 0 条应为 AW");
    CHECK(recs[0].size == 64, "AW 记整笔字节数");
    CHECK(recs[0].axi_id == 9, "AW 应带 AXI ID");
    CHECK(recs[0].strb == 0, "地址通道 STRB 恒为 0");
    for (int i = 1; i <= 4; ++i) {
        CHECK(recs[i].chan == kChanW, "第 1..4 条应为 W");
        CHECK(recs[i].addr == kSharedBufferBase + (i - 1) * 16,
              "W 地址应按拍递增");
        CHECK(recs[i].txn == recs[0].txn, "同一事务的 txn 应一致");
    }
    CHECK(recs[4].strb == 0x00ffull, "末拍应保留调用方给的部分 STRB");
    CHECK((recs[4].flags & kFlagLast) != 0, "末拍应带 WLAST");
    CHECK((recs[3].flags & kFlagLast) == 0, "非末拍不应带 WLAST");
    CHECK(recs[5].chan == kChanB, "第 5 条应为 B");
    CHECK(recs[5].tick == 1500, "B 应记在响应时刻");
    CHECK(recs[5].size == 0, "B 通道不搬字节");

    CHECK(recs[6].chan == kChanAr, "第 6 条应为 AR");
    CHECK(recs[6].tick == 2000, "AR 应记在请求时刻");
    CHECK(recs[7].chan == kChanR && recs[7].tick == 2700,
          "R 必须记在响应时刻，否则读延迟消失");
    CHECK(recs[8].resp == kRespSlvErr, "错误响应应逐拍带到 R 上");
    CHECK(recs[6].txn != recs[0].txn, "读写应是两笔不同事务");

    for (const Record& r : recs) {
        CHECK((r.flags & kFlagSynth) != 0, "synth=true 的源每条都应带 Synth");
    }
}

static void TestFullChannelBurstKindsAndRejects() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "bin", 1);
    unsetenv("HETTRACE_FILTER");

    TraceWriter w;
    CHECK(w.Open(kSrcHost, "burst_kinds", kLevelInterconnect,
                 kClockPeriodTicks_host, /*axi_data_bytes=*/16),
          "burst 类型测试应能打开 writer");

    AxiTxn fixed;
    fixed.addr = kSharedBufferBase;
    fixed.bytes = 32;
    fixed.txn = w.NextTxn();
    fixed.axi_size = 4;
    fixed.burst = kBurstFixed;
    CHECK(w.BeginRead(100, fixed), "合法 FIXED 应被接受");
    CHECK(w.CompleteRead(200, fixed, kRespOkay), "合法 FIXED 应能完成");

    AxiTxn wrap;
    wrap.addr = kSharedBufferBase + 48;
    wrap.bytes = 64;
    wrap.txn = w.NextTxn();
    wrap.axi_size = 4;
    wrap.burst = kBurstWrap;
    CHECK(w.BeginRead(300, wrap), "合法 WRAP 应被接受");
    CHECK(w.CompleteRead(400, wrap, kRespOkay), "合法 WRAP 应能完成");

    AxiTxn narrow;
    narrow.addr = kSharedBufferBase + 8;
    narrow.bytes = 4;
    narrow.txn = w.NextTxn();
    narrow.axi_size = 2;
    narrow.burst = kBurstIncr;
    CHECK(w.BeginWrite(450, narrow), "合法窄写应被接受");
    CHECK(w.CompleteWrite(460, narrow, kRespOkay), "合法窄写应能完成");

    AxiTxn partial = fixed;
    partial.addr = kSharedBufferBase + 0x100;
    partial.bytes = 20;
    partial.txn = w.NextTxn();
    CHECK(!w.BeginRead(500, partial), "非整拍事务必须被 writer 拒绝");

    AxiTxn unaligned = fixed;
    unaligned.addr = kSharedBufferBase + 3;
    unaligned.txn = w.NextTxn();
    CHECK(!w.BeginRead(600, unaligned), "unaligned 事务必须被 writer 拒绝");
    w.Close();

    std::vector<Record> recs = ReadBin(g_dir + "/burst_kinds.hettrace", nullptr);
    // FIXED: AR+2R；WRAP: AR+4R；窄写: AW+W+B。被拒事务不能留下半条记录。
    CHECK(recs.size() == 11, "三笔合法事务应产生 11 条记录");
    if (recs.size() != 11) return;
    CHECK(recs[1].addr == fixed.addr && recs[2].addr == fixed.addr,
          "FIXED 的每个 R 拍地址必须保持不变");
    const uint64_t wrap_expected[4] = {
        kSharedBufferBase + 48,
        kSharedBufferBase,
        kSharedBufferBase + 16,
        kSharedBufferBase + 32,
    };
    for (int i = 0; i < 4; ++i) {
        CHECK(recs[4 + i].addr == wrap_expected[i],
              "WRAP 的 R 拍地址必须在 wrap boundary 内回绕");
    }
    CHECK(recs[9].chan == kChanW && recs[9].addr == narrow.addr,
          "窄写的数据拍位置应正确");
    CHECK(recs[9].strb == 0x0f00,
          "4B 窄写位于 16B bus lane 8..11 时默认 WSTRB 应为 0x0f00");
}

static void TestDramFilter() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "bin", 1);
    unsetenv("HETTRACE_FILTER");  // 默认 dram

    TraceWriter w;
    w.Open(kSrcCoralnpu, "npu_filt", kLevelAxiMaster,
           kClockPeriodTicks_coralnpu);
    w.Emit(10, kSharedBufferBase, 16, kRead, 0);   // DRAM 窗口内 -> 保留
    w.Emit(20, kNpuTcmAddr, 4, kRead, 0);          // TCM -> 过滤
    w.Emit(30, kVortexCpBase, 4, kWrite, 0);       // CP 寄存器 -> 过滤
    w.Emit(40, kNpuMailboxBase, 16, kWrite, 0);    // mailbox 在 DRAM 窗口外 -> 过滤
    w.Emit(50, kNpuWorkBase, 16, kWrite, 0);       // DRAM 窗口内 -> 保留
    w.Emit(60, 0xdeadbeef, 4, kRead, 0);           // 未映射且被过滤
    w.Close();

    CHECK(w.stats().emitted == 2, "dram 过滤后应剩 2 条");
    CHECK(w.stats().filtered == 4, "应过滤掉 4 条");
    CHECK(w.stats().unmapped == 1,
          "未映射地址即使被过滤也应留在 meta 统计中");
}

static void TestFilterAll() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FILTER", "all", 1);

    TraceWriter w;
    w.Open(kSrcCoralnpu, "npu_all", kLevelAxiMaster, kClockPeriodTicks_coralnpu);
    w.Emit(10, kSharedBufferBase, 16, kRead, 0);
    w.Emit(20, kNpuTcmAddr, 4, kRead, 0);
    w.Emit(30, 0xdeadbeef, 4, kWrite, 0);  // 未映射
    w.Close();
    unsetenv("HETTRACE_FILTER");

    CHECK(w.stats().emitted == 3, "filter=all 应全部保留");
    CHECK(w.stats().filtered == 0, "filter=all 不应过滤");
    CHECK(w.stats().unmapped == 1, "应计出 1 条未映射");

    FileHeader h;
    std::vector<Record> recs = ReadBin(g_dir + "/npu_all.hettrace", &h);
    CHECK((h.flags & kHdrFilteredDram) == 0, "filter=all 时头部不应置过滤位");
    CHECK(recs.size() == 3, "应写出 3 条");
    if (recs.size() == 3) {
        CHECK((recs[2].flags & kFlagUnmapped) != 0,
              "未映射记录应带 kFlagUnmapped");
    }
}

static void TestNonMonotonicDetected() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    TraceWriter w;
    w.Open(kSrcHost, "host_mono", kLevelPostLlc, kClockPeriodTicks_host);
    w.Emit(100, kSharedBufferBase, 8, kRead, 0);
    w.Emit(90, kSharedBufferBase, 8, kRead, 0);   // 回退
    w.Emit(200, kSharedBufferBase, 8, kRead, 0);
    w.Close();
    CHECK(w.stats().non_monotonic == 1, "应检出 1 次 tick 回退");
    CHECK(w.stats().first_tick == 100, "first_tick 应为 100");
    CHECK(w.stats().last_tick == 200, "last_tick 应为 200");
}

// AXI burst 展开是最容易出错的一处：hw_primitives.h 里每拍回调都带同一个
// 首地址，不展开就会得到 N 条相同地址的记录。
static void TestBurstExpansion() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    TraceWriter w;
    w.Open(kSrcCoralnpu, "npu_burst", kLevelAxiMaster,
           kClockPeriodTicks_coralnpu);
    // axi_len=3 => 4 拍; axi_size=4 => 每拍 16 字节
    w.EmitBurst(500, kSharedBufferBase, /*axi_len=*/3, /*axi_size=*/4, kRead, 2);
    w.Close();

    CHECK(w.stats().emitted == 4, "len=3 应展开为 4 拍");
    CHECK(w.stats().bytes == 64, "4 拍 × 16 字节 = 64");
    CHECK(w.stats().data_records == 4, "burst 展开后全是数据通道记录");
    CHECK(w.stats().transactions == 0,
          "EmitBurst 不写地址通道，事务数应为 0");

    std::vector<Record> recs = ReadBin(g_dir + "/npu_burst.hettrace", nullptr);
    CHECK(recs.size() == 4, "应写出 4 条");
    if (recs.size() == 4) {
        for (uint32_t i = 0; i < 4; ++i) {
            CHECK(recs[i].addr == kSharedBufferBase + i * 16,
                  "burst 地址应按拍递增");
            CHECK(recs[i].size == 16, "每拍 16 字节");
            CHECK(recs[i].chan == kChanR, "读 burst 应全落在 R 通道");
            CHECK(recs[i].axi_len == 3, "每拍都应带整笔的 AxLEN");
            CHECK(recs[i].txn == recs[0].txn, "同一 burst 的 txn 应一致");
        }
        CHECK((recs[0].flags & kFlagBurstBeat) == 0, "首拍不应带 BurstBeat");
        CHECK((recs[1].flags & kFlagBurstBeat) != 0, "非首拍应带 BurstBeat");
        CHECK((recs[3].flags & kFlagLast) != 0, "末拍应带 RLAST");
        CHECK((recs[2].flags & kFlagLast) == 0, "非末拍不应带 RLAST");
    }
}

static void TestTextFormat() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "text", 1);
    {
        TraceWriter w;
        w.Open(kSrcHost, "host_txt", kLevelPostLlc, kClockPeriodTicks_host,
               /*axi_data_bytes=*/64);
        w.Emit(4242, kSharedBufferBase + 0x100, 64, kWrite, 3);
    }
    setenv("HETTRACE_FORMAT", "bin", 1);

    FILE* f = std::fopen((g_dir + "/host_txt.hettrace.txt").c_str(), "r");
    CHECK(f != nullptr, "文本 trace 文件应存在");
    if (f == nullptr) return;
    char line[512];
    std::string body;
    int ncomment = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#') {
            ++ncomment;
            continue;
        }
        body = line;
    }
    std::fclose(f);
    CHECK(ncomment == 6, "文本头应为 6 行注释");
    CHECK(body.find("4242") != std::string::npos, "应含 tick");
    CHECK(body.find(" W W ") != std::string::npos, "写应记为 chan=W op=W");
    CHECK(body.find("INCR") != std::string::npos, "应含 burst 类型");
    CHECK(body.find("OKAY") != std::string::npos, "应含响应码");
    CHECK(body.find("0x90000100") != std::string::npos, "应含十六进制地址");
}

static void TestMetaSidecar() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FORMAT", "bin", 1);
    {
        TraceWriter w;
        w.Open(kSrcVortex, "vortex_meta", kLevelPostLlc,
               kClockPeriodTicks_vortex, /*axi_data_bytes=*/64);
        for (int i = 0; i < 10; ++i) {
            w.Emit(1000 + i, kSharedBufferBase + i * 64, 64, kRead, 1);
        }
    }
    FILE* f = std::fopen((g_dir + "/vortex_meta.hettrace.meta.json").c_str(), "r");
    CHECK(f != nullptr, "meta 侧车文件应存在");
    if (f == nullptr) return;
    std::string all;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f) != nullptr) all += buf;
    std::fclose(f);
    CHECK(all.find("\"emitted\": 10") != std::string::npos,
          "meta 应记录 emitted=10");
    CHECK(all.find("\"data_records\": 10") != std::string::npos,
          "meta 应记录 data_records=10");
    CHECK(all.find("\"bytes\": 640") != std::string::npos,
          "meta 应记录 bytes=640");
    CHECK(all.find("\"axi_data_bytes\": 64") != std::string::npos,
          "meta 应记录总线宽度");
    CHECK(all.find("\"name\": \"vortex_meta\"") != std::string::npos,
          "meta 应含源名");
}

// 缓冲区必须能正确跨越多次 flush —— 这是长跑 trace 的常见丢数据点。
static void TestFlushAcrossBuffer() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_BUFSZ", "8", 1);
    const int N = 100;
    {
        TraceWriter w;
        w.Open(kSrcHost, "host_flush", kLevelPostLlc, kClockPeriodTicks_host);
        for (int i = 0; i < N; ++i) {
            w.Emit(i, kSharedBufferBase + i * 8, 8, kRead, 0);
        }
    }
    unsetenv("HETTRACE_BUFSZ");

    std::vector<Record> recs = ReadBin(g_dir + "/host_flush.hettrace", nullptr);
    CHECK(recs.size() == static_cast<size_t>(N), "跨 flush 不应丢记录");
    bool seq_ok = true;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (recs[i].seq != i) seq_ok = false;
    }
    CHECK(seq_ok, "seq 应连续无洞");
}

static void TestRuntimeTimebase() {
    setenv("HETTRACE_DIR", g_dir.c_str(), 1);
    setenv("HETTRACE_FILTER", "all", 1);
    for (const char* format : {"bin", "text"}) {
        setenv("HETTRACE_FORMAT", format, 1);
        const std::string name = std::string("host_fs_") + format;
        TraceWriter w;
        CHECK(w.Open(kSrcHost, name.c_str(), kLevelInterconnect,
                     500000, 16, 64, true, 1000000000000000ull),
              "1fs timebase writer should open");
        w.Emit(2000000, kSharedBufferBase, 8, kRead, 0);
        w.Close();
        const std::string path = g_dir + "/" + name + ".hettrace" +
                                 (std::strcmp(format, "text") == 0 ? ".txt" : "");
        if (std::strcmp(format, "bin") == 0) {
            FileHeader header{};
            const auto records = ReadBin(path, &header);
            CHECK(header.ticks_per_second == 1000000000000000ull,
                  "binary header must describe fs rather than ps");
            CHECK(header.clock_period_ticks == 500000,
                  "binary header must retain clock period in fs");
            CHECK(records.size() == 1 && records[0].tick == 2000000,
                  "record timestamps must not be rescaled");
        }
        for (const auto& file : {path, path + ".meta.json"}) {
            if (file == path && std::strcmp(format, "bin") == 0) continue;
            FILE* f = std::fopen(file.c_str(), "r");
            CHECK(f != nullptr, "trace/metadata file must exist");
            if (!f) continue;
            std::string contents;
            char line[512];
            while (std::fgets(line, sizeof(line), f)) contents += line;
            std::fclose(f);
            CHECK(contents.find("1000000000000000") != std::string::npos,
                  "text and metadata must describe the same fs timebase");
        }
    }
    setenv("HETTRACE_FORMAT", "bin", 1);
    TraceWriter legacy;
    CHECK(legacy.Open(kSrcHost, "host_default_timebase", kLevelInterconnect,
                      kClockPeriodTicks_host), "default writer should open");
    legacy.Close();
    FileHeader header{};
    ReadBin(g_dir + "/host_default_timebase.hettrace", &header);
    CHECK(header.ticks_per_second == kTicksPerSecond,
          "existing callers must retain their ps default");
}

static void TestAddrMapSanity() {
    // shared_buffer 是 host↔NPU 交接区；Vortex 只能经 4 GiB 以上 BAR
    // 与 host 交接，当前不存在三方以同一物理地址直连共享的区域。
    CHECK(IsDram(kSharedBufferBase), "shared_buffer 应在 DRAM 窗口内");
    CHECK(kSharedBufferBase + kSharedBufferSize <= (1ull << kNpuAddrBits),
          "shared_buffer 必须在 NPU 32 位地址范围内");
    CHECK(kVortexBarBase >= (1ull << kNpuAddrBits),
          "vortex_bar 应位于 NPU 地址范围之外");
    // CoralNPU 只有 32 位地址；所有 DRAM 区域必须在 4GiB 以内。
    // 注意这里用 kNpuAddrBits 而不是整张图的宽度：vortex_bar 在 4GiB 之上，
    // 它是合法的（host 与 Vortex 够得到），只是 NPU 够不到。
    for (size_t i = 0; i < kNumRegions; ++i) {
        if (!kRegions[i].is_dram) continue;
        CHECK(kRegions[i].base + kRegions[i].size <= (1ull << kNpuAddrBits),
              "DRAM 区域必须落在 CoralNPU 的 32 位可寻址范围内");
    }
    // 过滤窗口。IsTraced 比 IsDram 宽一个 BAR —— 少了它，host 与 Vortex 经 BAR
    // 交换的字节会在默认过滤器下被无声丢掉。
    CHECK(IsTraced(kSharedBufferBase), "shared_buffer 应过得了 dram 过滤器");
    CHECK(IsTraced(kVortexBarBase), "vortex_bar 应过得了 dram 过滤器");
    CHECK(IsTraced(kVortexBarBase + kVortexBarSize - 1),
          "vortex_bar 末端也应在窗口内");
    CHECK(!IsDram(kVortexBarBase),
          "vortex_bar 不在 CoralNPU 的 DDR 判定窗口内");
    CHECK(!IsTraced(kVortexCpBase), "CP 寄存器不是内存流量，不该进 trace");
    CHECK(!IsTraced(kNpuTcmAddr), "NPU 的 TCM 命中不该进 trace");
    CHECK(std::strcmp(RegionOf(kVortexBarBase), "vortex_bar") == 0,
          "RegionOf 应返回 vortex_bar");
    CHECK(std::strcmp(RegionOf(kSharedBufferBase), "shared_buffer") == 0,
          "RegionOf 应返回 shared_buffer");
    CHECK(RegionOf(0xdeadbeef) == nullptr, "未映射地址应返回 nullptr");
}

int main() {
    // 外部指定 HETTRACE_DIR 时用它 —— tools/tests 的互操作用例靠这个拿到产物，
    // 交叉验证 Python 侧能否原样读出 C++ 写的记录。
    char tmpl[] = "/tmp/hettrace_test_XXXXXX";
    const char* env_dir = std::getenv("HETTRACE_DIR");
    if (env_dir != nullptr && env_dir[0] != '\0') {
        g_dir = env_dir;
    } else {
        const char* d = mkdtemp(tmpl);
        if (d == nullptr) {
            std::fprintf(stderr, "mkdtemp 失败\n");
            return 1;
        }
        g_dir = d;
    }

    TestDisabledWhenNoEnv();
    TestHeaderAndRoundTrip();
    TestDataChannelProjection();
    TestFullChannelTransactions();
    TestFullChannelBurstKindsAndRejects();
    TestDramFilter();
    TestFilterAll();
    TestNonMonotonicDetected();
    TestBurstExpansion();
    TestTextFormat();
    TestMetaSidecar();
    TestFlushAcrossBuffer();
    TestAddrMapSanity();
    TestRuntimeTimebase();

    std::printf("libhettrace: %d 项检查, %d 项失败\n", g_checks, g_failed);
    if (g_failed == 0) {
        std::printf("产物保留在 %s\n", g_dir.c_str());
    }
    return g_failed == 0 ? 0 : 1;
}
