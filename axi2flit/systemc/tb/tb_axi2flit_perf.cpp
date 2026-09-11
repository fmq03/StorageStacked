/**
 * 桥性能基线测试。采用速率节流、飞行延迟和模式数据响应器，不实例化完整链路。
 * 分别测量持续读写吞吐、桥内延迟、载荷效率及credit环路时间的影响。
 */
#include <systemc.h>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include "axi2flit.h"
#include "tb_common.h"

static constexpr double   CLK_PERIOD_NS = MODEL_CLK_PERIOD_NS;
static constexpr unsigned PERF_RP_COUNT = 1;    // 性能场景只用 RP0，避免仲裁噪声

// 突发长度（beat 数）。AXI 的 AxLEN = BURST_BEATS-1。
static constexpr unsigned BURST_BEATS = 16;
// 主控最大未完成突发数：给足够深度让链路而不是主控成为瓶颈
static constexpr unsigned MAX_OUTSTANDING_BURSTS = 64;
// 测量窗口：先跑 WARMUP 拍让流水线和 credit 进入稳态，再统计 MEASURE 拍
static constexpr unsigned WARMUP_CYCLES  = 600;
static constexpr unsigned MEASURE_CYCLES = 2000;
// 空载延迟采样次数
static constexpr unsigned LATENCY_SAMPLES = 20;

// ============================================================
//  性能门限（按位宽）
// ============================================================
/**
 * 门限取"实测值留一点余量"，目的是抓回归而不是刷指标：
 *   - 256b/512b 是 AXI 侧受限（16 / 32 GB/s 就是天花板），门限贴着上限给；
 *   - 1024b 是链路侧受限，门限贴着 桥接消息 协议效率上限给；
 *   - 口径B 门限只在链路受限（1024b）时才有意义 —— 位宽不够时 Flit 装不满，
 *     口径A/B 低是物理事实，不是实现缺陷，对它们设门限只会误报。
 *   - 延迟门限用"桥内 TX + RX"，这是本交付物真正拥有的部分；链路飞行时间
 *     和存储器件时延都是可加的独立项，不该混进来考核。
 */
struct PerfThresholds {
    double read_gbps_min;
    double write_gbps_min;
    double effB_min;          // 口径B 下限，<=0 表示本位宽不考核
    double lat_tx_max_ns;     // 桥内发送侧延迟上限
    double lat_rx_max_ns;     // 桥内接收侧延迟上限
};

#if   AXI_DATA_WIDTH_CFG == 1024
static constexpr PerfThresholds PERF_LIMITS{42.0, 41.5, 0.93, 12.0, 8.0};
#elif AXI_DATA_WIDTH_CFG == 512
static constexpr PerfThresholds PERF_LIMITS{31.5, 31.5, 0.0,  12.0, 8.0};
#else
static constexpr PerfThresholds PERF_LIMITS{15.8, 15.8, 0.0,  12.0, 8.0};
#endif

// 宏与 constexpr 必须一致：万一以后有人改了其中一个，这里立刻编译失败，
// 而不是像上面那样悄悄退回 256b 门限。
static_assert(AXI_DATA_WIDTH_CFG == AXI_DATA_WIDTH,
              "AXI_DATA_WIDTH_CFG 与 AXI_DATA_WIDTH 不一致，性能门限会选错位宽");

static int perf_errors = 0;

// 门限检查：打印一行结论，不达标就计一个错误
static bool check_limit(const char* name, double got, double limit,
                        const char* unit, bool lower_bound = true) {
    bool ok = lower_bound ? (got >= limit) : (got <= limit);
    std::cout << "    " << (ok ? "[PASS] " : "[FAIL] ") << std::left
              << std::setw(30) << name << std::right << std::fixed
              << std::setprecision(2) << got << " " << unit
              << (lower_bound ? "  >= " : "  <= ") << limit << " " << unit
              << std::endl;
    if (!ok) ++perf_errors;
    return ok;
}

static inline double now_ns() {
    return sc_time_stamp() / sc_time(1, SC_NS);
}

SC_MODULE(PerfTb) {
    sc_in<bool> clk;
    sc_in<bool> rst_n;

    sc_out<bool>      aw_valid;
    sc_in<bool>       aw_ready;
    sc_out<AxChannel> aw_ch;
    sc_out<bool>      w_valid;
    sc_in<bool>       w_ready;
    sc_out<WChannel>  w_ch;
    sc_out<bool>      ar_valid;
    sc_in<bool>       ar_ready;
    sc_out<AxChannel> ar_ch;

    sc_in<bool>      b_valid;
    sc_out<bool>     b_ready;
    sc_in<BChannel>  b_ch;
    sc_in<bool>      r_valid;
    sc_out<bool>     r_ready;
    sc_in<RChannel>  r_ch;

    sc_in<FlitTransfer>  flit_out;
    sc_out<bool>         flit_ready;
    sc_out<FlitTransfer> flit_in;
    sc_in<bool>          flit_in_ready;

    // ---- 链路对端与速率节流 ----
    RemoteAouModel remote{PERF_RP_COUNT};

    FlitScanner rx_scanner_{PERF_RP_COUNT};
    LinkPacer      tx_pacer;      // DUT → 对端（速率）
    LinkPacer      rx_pacer;      // 对端 → DUT（速率）
    // 链路飞行时间（延迟）。速率与延迟是两件独立的事，见 tb_common.h 的说明。
    // 默认 0：PERF-1~4 量的是桥接单元自身；PERF-5 会把它设成真实 TAT 对应的值。
    FlitDelayLine<TimedFlit> tx_flight_;   // DUT 发出 → 对端收到
    FlitDelayLine<AouFlit>   rx_flight_;   // 对端发出 → DUT 收到

    /**
     * 配置链路单向飞行时间。往返 = 2 × (序列化 5.333ns + 飞行 one_way_ns)。
     * 改配置时两个方向的在飞队列都要清空，否则旧配置下的包会带着旧到达时刻
     * 留在队列里，污染下一个场景。
     */
    void set_link_one_way_ns(double one_way_ns) {
        tx_flight_.configure(one_way_ns);
        rx_flight_.configure(one_way_ns);
    }

    // ---- 链路计数 ----
    unsigned long tx_flits_ = 0, tx_granules_ = 0;
    unsigned long rx_flits_ = 0, rx_granules_ = 0;

    unsigned long tx_empty_flits_ = 0, rx_empty_flits_ = 0;

    // ---- AXI 计数 ----
    unsigned long r_beats_ = 0, w_beats_ = 0, b_beats_ = 0;
    unsigned long r_data_errors_ = 0;

    // ---- 激励控制（由 ctrl_thread 设置，激励线程只读）----
    unsigned long ar_todo_ = 0;     // 还要发多少个读突发
    unsigned long wr_todo_ = 0;     // 还要发多少个写突发
    uint8_t       burst_len_ = 0;
    unsigned long aw_issued_ = 0;   // 已发出的 AW 个数（只用于生成地址/ID）

    std::deque<std::pair<uint16_t, unsigned>> w_pending_;   // {id, beat 数}

    /*
     * AxVALID 已经拉高、但还没等到 AxREADY 的那一拍，事务尚未计入
     * outstanding_*，排空判据看不见它。切换测试用例时如果正好卡在这里，
     * 这条迟到的 AW 会带着上一个用例的 AWLEN 闯进下一个用例。
     */
    bool ar_pending_ = false;
    bool aw_pending_ = false;
    unsigned      outstanding_reads_ = 0;
    unsigned      outstanding_writes_ = 0;

    // ---- 延迟测量用的时间戳 ----
    double t_ar_issue_ = -1.0;      // AR 握手
    double t_w_last_   = -1.0;      // 最后一个 W beat 握手
    double t_tx_rreq_  = -1.0;      // 载有 ReadReq 的 Flit 被链路取走
    double t_tx_wdata_ = -1.0;      // 载有最后一个 WriteData 的 Flit 被链路取走
    double t_rx_data_  = -1.0;      // 载有响应的入站 Flit 被 DUT 接收
    double t_r_beat_   = -1.0;      // R beat 在 AXI 上握手
    double t_b_beat_   = -1.0;      // B beat 在 AXI 上握手

    LatencyStat lat_read_total_, lat_read_tx_, lat_read_rx_;
    LatencyStat lat_write_total_, lat_write_tx_, lat_write_rx_;
    LatencyStat lat_ref_read_total_;   // PERF-5 的零飞行参照（与下一行同条件采样）
    LatencyStat lat_tat_read_total_;   // PERF-5：带真实飞行时间的读往返

    // 读响应顺序检查：远端严格按请求顺序回数据
    std::deque<std::pair<uint16_t, unsigned>> expect_reads_;   // {id, 总 beat 数}
    unsigned r_beat_in_burst_ = 0;

    // PERF-5 跑的是"带真实 TAT"的场景，吞吐门限按 PERF-1/2 的标准考核没有意义
    // （链路上多了 40ns 的在飞时间），因此那一轮只看 credit 是否够用，不查门限。
    bool check_thresholds_ = true;

    SC_CTOR(PerfTb) {
        SC_THREAD(ctrl_thread);   sensitive << clk.pos();
        SC_THREAD(link_thread);   sensitive << clk.pos();
        SC_THREAD(ar_thread);     sensitive << clk.pos();
        SC_THREAD(aw_thread);     sensitive << clk.pos();
        SC_THREAD(w_thread);      sensitive << clk.pos();
        SC_THREAD(resp_thread);   sensitive << clk.pos();
    }

    // =========================================================
    //  FDI 链路：两个方向都按 48B/ns 节流
    // =========================================================
    /**
     * ready/valid 的采样约定：DUT 的输出与本线程驱动的 ready 都是"上一拍写入、
     * 本拍可见"的寄存值，因此在同一个时钟沿上同时读到 valid=1 且 ready=1，
     * 就等价于 DUT 内部判定的一次握手，两边对握手时刻的认知完全一致。
     */
    void link_thread() {
        flit_ready.write(false);
        flit_in.write(FlitTransfer{});
        bool rx_valid_held = false;
        AouFlit rx_flit;
        set_link_one_way_ns(0.0);

        while (!rst_n.read()) wait();

        while (true) {
            wait();

            // ======== 出站方向：DUT → 对端 ========
            // (1) DUT 把 Flit 交给链路：占用一次发送速率，进入在飞队列。
            //     "上链路时刻"随包一起带走 —— 延迟统计要的是交给链路的那一刻，
            //     不是对端收到的那一刻，两者相差一个飞行时间。
            FlitTransfer tx = flit_out.read();
            if (tx.valid && flit_ready.read()) {
                tx_pacer.consume();
                ++tx_flits_;
                tx_granules_ += static_cast<unsigned long>(tx.flit.used_granules);
                if (tx.flit.used_granules == 0) ++tx_empty_flits_;
                tx_flight_.push(now_ns(), TimedFlit{tx.flit, now_ns()});
            }
            // (2) 飞行结束的 Flit 交给对端处理
            TimedFlit arrived;
            while (tx_flight_.pop_ready(now_ns(), arrived)) {
                unsigned long rreq_before  = remote.rreq_seen();
                unsigned long wdata_before = remote.wdata_beats();
                remote.consume_flit(now_ns(), arrived.flit);
                if (remote.rreq_seen()   > rreq_before)  t_tx_rreq_  = arrived.t_handshake;
                if (remote.wdata_beats() > wdata_before) t_tx_wdata_ = arrived.t_handshake;
            }
            tx_pacer.tick();
            // 在飞队列满 = 链路上已经塞满了包，此时不能再收，必须撤 ready
            flit_ready.write(tx_pacer.can_transfer() && tx_flight_.has_room());

            // ======== 入站方向：对端 → DUT ========
            // (1) DUT 取走上一包
            if (rx_valid_held && flit_in_ready.read()) {
                ++rx_flits_;
                rx_granules_ += static_cast<unsigned long>(rx_flit.used_granules);
                if (rx_flit.used_granules == 0) ++rx_empty_flits_;
                else t_rx_data_ = now_ns();     // 带载荷的入站 Flit = 响应到达时刻
                rx_scanner_.scan(rx_flit, [](const AouMessage&) {});
                rx_valid_held = false;
                flit_in.write(FlitTransfer{});
            }
            rx_pacer.tick();
            // (2) 对端按链路速率把新 Flit 送上线（占用速率的是发送方，不是接收方）
            if (rx_pacer.can_transfer() && rx_flight_.has_room()) {
                AouFlit candidate;
                if (remote.produce_flit(now_ns(), candidate)) {
                    rx_pacer.consume();
                    rx_flight_.push(now_ns(), candidate);
                }
            }
            // (3) 飞行结束的 Flit 呈现给 DUT
            if (!rx_valid_held && rx_flight_.pop_ready(now_ns(), rx_flit)) {
                rx_valid_held = true;
                flit_in.write(FlitTransfer(rx_flit));
            }
        }
    }

    // =========================================================
    //  AXI 激励：AR
    // =========================================================
    void ar_thread() {
        ar_valid.write(false);
        ar_ch.write(AxChannel{});
        while (!rst_n.read()) wait();
        uint16_t seq = 0;

        while (true) {
            if (ar_todo_ == 0 || outstanding_reads_ >= MAX_OUTSTANDING_BURSTS) {
                ar_valid.write(false);
                wait();
                continue;
            }
            uint16_t id = static_cast<uint16_t>(0x100 + (seq & 0xFF));
            const unsigned beats = burst_len_ + 1u;
            ar_ch.write(make_ax(id, tb_addr(0x1000'0000ULL + seq * 4096ULL), burst_len_));
            ar_valid.write(true);
            ar_pending_ = true;
            do { wait(); } while (!ar_ready.read());
            ar_pending_ = false;
            // 本拍完成握手
            t_ar_issue_ = now_ns();
            expect_reads_.push_back({id, beats});
            ++outstanding_reads_;
            // 必须判零再减：ctrl_thread 可能正好在"检查 ar_todo_"和"等到
            // ARREADY"之间把它清零，无判断的 --ar_todo_ 会把 unsigned 减成
            // 4294967295，激励就再也停不下来了（PERF-5 的读带宽窗口里混进
            // 上一用例的写流量，就是这么来的）。
            if (ar_todo_) --ar_todo_;
            ++seq;
            // valid 保持到下一拍再判断，实现背靠背 1 beat/cycle
        }
    }

    // =========================================================
    //  AXI 激励：AW（与 W 分开，模拟真实主控的独立通道）
    // =========================================================
    void aw_thread() {
        aw_valid.write(false);
        aw_ch.write(AxChannel{});
        while (!rst_n.read()) wait();

        while (true) {
            if (wr_todo_ == 0 || outstanding_writes_ >= MAX_OUTSTANDING_BURSTS) {
                aw_valid.write(false);
                wait();
                continue;
            }
            uint16_t id = static_cast<uint16_t>(0x200 + (aw_issued_ & 0xFF));
            const unsigned beats = burst_len_ + 1u;
            aw_ch.write(make_ax(id, tb_addr(0x2000'0000ULL + aw_issued_ * 4096ULL), burst_len_));
            aw_valid.write(true);
            aw_pending_ = true;
            do { wait(); } while (!aw_ready.read());
            aw_pending_ = false;
            ++aw_issued_;
            ++outstanding_writes_;
            if (wr_todo_) --wr_todo_;      // 判零再减，理由同 ar_thread
            // 把这条 AW 的长度交给 w_thread：W 只认队列，不再现读 burst_len_
            w_pending_.push_back({id, beats});
        }
    }

    // =========================================================
    //  AXI 激励：W。按 AW 的发出顺序供数据（AXI4 的 W 没有 ID）
    // =========================================================
    void w_thread() {
        w_valid.write(false);
        w_ch.write(WChannel{});
        while (!rst_n.read()) wait();

        while (true) {
            if (w_pending_.empty()) {
                w_valid.write(false);
                wait();
                continue;
            }
            const uint16_t id    = w_pending_.front().first;
            const unsigned beats = w_pending_.front().second;
            w_pending_.pop_front();
            for (unsigned i = 0; i < beats; ++i) {
                // 全 strobe → DUT 选用 WriteDataFull，比带 WSTRB 的 WriteData 少 1~3 granule
                w_ch.write(make_w(id, i, i + 1 == beats, true));
                w_valid.write(true);
                do { wait(); } while (!w_ready.read());
                ++w_beats_;
                if (i + 1 == beats) t_w_last_ = now_ns();
            }
        }
    }

    // =========================================================
    //  AXI 响应侧：R/B 恒 ready，记录 beat 并做数据校验
    // =========================================================
    void resp_thread() {
        r_ready.write(true);
        b_ready.write(true);
        while (true) {
            wait();
            if (r_valid.read() && r_ready.read()) {
                RChannel r = r_ch.read();
                ++r_beats_;
                t_r_beat_ = now_ns();
                // 远端严格按请求顺序回数据，因此可以逐 beat 对账
                if (expect_reads_.empty()) {
                    ++r_data_errors_;
                } else {
                    auto& exp = expect_reads_.front();
                    // 逐字节比对：图样每个字节都不同，字节序错了立刻能发现
                    bool ok = (r.id == exp.first) &&
                              (r.last == (r_beat_in_burst_ + 1 == exp.second)) &&
                              (check_rdata_pattern(r.data, exp.first,
                                                   r_beat_in_burst_) < 0);
                    if (!ok) ++r_data_errors_;
                    if (++r_beat_in_burst_ >= exp.second) {
                        r_beat_in_burst_ = 0;
                        expect_reads_.pop_front();
                        if (outstanding_reads_) --outstanding_reads_;
                    }
                }
            }
            if (b_valid.read() && b_ready.read()) {
                ++b_beats_;
                t_b_beat_ = now_ns();
                if (outstanding_writes_) --outstanding_writes_;
            }
        }
    }

    // =========================================================
    //  报告
    // =========================================================
    /*
     * 【排空判据】切换测试用例前必须真正静下来：
     * outstanding_* 只统计"已握手"的事务，还要把"已拉高 valid 但没等到
     * ready"的那一条、以及 AW 已进队但 W 还没发的那些一并算上，
     * 否则改 burst_len_ 会把上一个用例的尾巴带进下一个用例。
     */
    bool reads_idle()  const { return outstanding_reads_  == 0 && !ar_pending_; }
    bool writes_idle() const {
        return outstanding_writes_ == 0 && !aw_pending_ && w_pending_.empty();
    }

    struct Window {
        double        t0 = 0.0;
        unsigned long r_beats = 0, w_beats = 0;
        unsigned long tx_flits = 0, rx_flits = 0;
        unsigned long tx_granules = 0, rx_granules = 0;
        unsigned long tx_bus_gran = 0, rx_bus_gran = 0;   // 非 Misc 消息占的粒度
        unsigned long tx_crd_gran = 0, rx_crd_gran = 0;   // CrdtGrant 占的粒度
    };
    Window snapshot() {
        Window w;
        w.t0 = now_ns();
        w.r_beats = r_beats_;  w.w_beats = w_beats_;
        w.tx_flits = tx_flits_; w.rx_flits = rx_flits_;
        w.tx_granules = tx_granules_; w.rx_granules = rx_granules_;
        w.tx_bus_gran = remote.scanner().business_granules();
        w.rx_bus_gran = rx_scanner_.business_granules();
        w.tx_crd_gran = remote.scanner().misc_granules();
        w.rx_crd_gran = rx_scanner_.misc_granules();
        return w;
    }

    // 返回实测吞吐（GB/s），供 PERF-5 与基线对比
    double report_bandwidth(const char* title, const Window& a, bool is_read) {
        Window b = snapshot();
        double  span_ns   = b.t0 - a.t0;
        unsigned long beats = is_read ? (b.r_beats - a.r_beats) : (b.w_beats - a.w_beats);
        double app_bytes  = static_cast<double>(beats) * AXI_DATA_BYTES;

        // 数据方向：读的数据走 RX（对端→DUT），写的数据走 TX（DUT→对端）。
        // UCIe 是全双工，两个方向是独立线对，各自的 5.333ns/Flit 互不占用，
        // 所以效率必须按"数据所在的那一个方向"算，而不是拿双向 Flit 数去除。
        unsigned long dat_flits = is_read ? (b.rx_flits - a.rx_flits)
                                          : (b.tx_flits - a.tx_flits);
        unsigned long dat_gran  = is_read ? (b.rx_granules - a.rx_granules)
                                          : (b.tx_granules - a.tx_granules);
        unsigned long dat_bus   = is_read ? (b.rx_bus_gran - a.rx_bus_gran)
                                          : (b.tx_bus_gran - a.tx_bus_gran);
        // credit 粒度直接取 Misc 计数，不能用 dat_gran - dat_bus：
        // 消息粒度在"收全"那一刻整条计入，Flit 粒度是逐拍累加的，跨 Flit
        // 消息会让两者在窗口边界差几个粒度，相减会得到 unsigned 的负数。
        unsigned long dat_crd   = is_read ? (b.rx_crd_gran - a.rx_crd_gran)
                                          : (b.tx_crd_gran - a.tx_crd_gran);
        // 反方向：承载请求（读时是 ReadReq，写时是 WriteResp）与 credit 回传
        unsigned long rev_flits = is_read ? (b.tx_flits - a.tx_flits)
                                          : (b.rx_flits - a.rx_flits);
        unsigned long rev_bus   = is_read ? (b.tx_bus_gran - a.tx_bus_gran)
                                          : (b.rx_bus_gran - a.rx_bus_gran);
        unsigned long rev_crd   = is_read ? (b.tx_crd_gran - a.tx_crd_gran)
                                          : (b.rx_crd_gran - a.rx_crd_gran);

        double gbps = app_bytes / span_ns;
        double effA = dat_flits ? app_bytes / (dat_flits * 256.0) : 0.0;
        double effB = dat_flits ? app_bytes / (dat_flits * 240.0) : 0.0;
        // 口径C：把反方向的业务粒度（AXI 请求/响应信息）也折算成物理链路字节，
        // 相当于"假设收发共用一条链路"的保守估计，是最不容易被质疑的口径。
        double wire_bytes_c = (dat_bus + rev_bus) * double(GRANULE_BYTES) *
                              double(FLIT_TOTAL_BYTES) / PAYLOAD_BYTES;
        double effC = wire_bytes_c > 0 ? app_bytes / wire_bytes_c : 0.0;

        double dat_fill = dat_flits ? double(dat_gran) / (dat_flits * GRANULE_COUNT) : 0.0;
        double dat_occ  = dat_flits * FLIT_PERIOD_NS / span_ns;
        double rev_occ  = rev_flits * FLIT_PERIOD_NS / span_ns;
        double axi_util = beats * CLK_PERIOD_NS / span_ns;      // AXI 通道拍占用率

        std::cout << "\n--- " << title << " ---" << std::endl;
        std::cout << std::fixed;
        std::cout << "  测量窗口                : " << std::setprecision(1) << span_ns << " ns（"
                  << MEASURE_CYCLES << " 拍 @" << CLK_PERIOD_NS << "ns）" << std::endl;
        std::cout << "  AXI beat 数 × 位宽      : " << beats << " × " << AXI_DATA_BYTES
                  << "B（burst = " << (burst_len_ + 1) << " beat）" << std::endl;
        std::cout << "  应用数据量              : " << std::setprecision(0) << app_bytes
                  << " B" << std::endl;
        std::cout << "  >> 实测吞吐             : " << std::setprecision(2) << gbps
                  << " GB/s   （链路裸带宽 " << std::setprecision(1) << LINK_BYTES_PER_NS
                  << " GB/s，AXI 侧上限 " << (AXI_DATA_BYTES / CLK_PERIOD_NS) << " GB/s）"
                  << std::endl;
        std::cout << "  AXI 数据通道占用率      : " << std::setprecision(1)
                  << (axi_util * 100) << "%（100% = 每拍一个 beat）" << std::endl;

        std::cout << "  [数据方向 " << (is_read ? "RX 对端→DUT" : "TX DUT→对端") << "]"
                  << std::endl;
        std::cout << "    Flit 数 / 链路占用率  : " << dat_flits << " / "
                  << std::setprecision(1) << (dat_occ * 100)
                  << "%（100% = 每 5.333ns 满发一个 Flit）" << std::endl;
        std::cout << "    平均填充              : " << std::setprecision(2)
                  << (dat_fill * GRANULE_COUNT) << "/48 granule = "
                  << std::setprecision(1) << (dat_fill * 100) << "%" << std::endl;
        std::cout << "    其中业务/credit 粒度  : " << dat_bus << " / "
                  << dat_crd << std::endl;

        std::cout << "  [反方向 " << (is_read ? "TX 请求+credit" : "RX 响应+credit") << "]"
                  << std::endl;
        std::cout << "    Flit 数 / 链路占用率  : " << rev_flits << " / "
                  << std::setprecision(1) << (rev_occ * 100) << "%" << std::endl;
        const double rev_load =
            rev_flits ? 100.0 * (rev_bus + rev_crd) / (rev_flits * GRANULE_COUNT) : 0.0;
        std::cout << "    其中业务/credit 粒度  : " << rev_bus << " / " << rev_crd
                  << "（合计占反方向容量的 " << std::setprecision(2) << rev_load
                  << "%）" << std::endl;

        std::cout << "  >> 口径A 应用/(256B×数据方向Flit)     : "
                  << std::setprecision(2) << (effA * 100) << "%" << std::endl;
        std::cout << "  >> 口径B 应用/(240B×数据方向Flit)     : "
                  << std::setprecision(2) << (effB * 100) << "%" << std::endl;
        std::cout << "  >> 口径C 应用/(双向业务粒度折算裸字节): "
                  << std::setprecision(2) << (effC * 100)
                  << "%（含 AXI 请求/响应开销，收发合算的保守口径）" << std::endl;

        // ---- 瓶颈判定 ----
        // 三个口径必须结合瓶颈来读，否则容易误判：
        //   AXI 受限时链路槽位虽被占满，Flit 却是半空的，口径A/B 会偏低，
        //   但那反映的是"没有数据可填"，不是协议效率差 —— 此时该看口径C。
        double axi_limit = AXI_DATA_BYTES / CLK_PERIOD_NS;
        if (axi_util > 0.95) {
            std::cout << "  >> 瓶颈判定             : AXI 侧（"
                      << std::setprecision(1) << axi_limit
                      << " GB/s 已跑满，链路仍有余量）。此时口径A/B 偏低是因为"
                         "没有足够数据填满 Flit，衡量协议开销应看口径C。" << std::endl;
        } else if (dat_occ > 0.95) {
            std::cout << "  >> 瓶颈判定             : 链路侧（数据方向槽位已 100% 占满，"
                         "Flit 平均填充 " << std::setprecision(1) << (dat_fill * 100)
                      << "%，效率按本模型消息粒度统计）。" << std::endl;
        } else {
            std::cout << "  >> 瓶颈判定             : 两侧均未跑满，瓶颈在桥接内部"
                         "（credit 深度 / FIFO / 打包吞吐），需要进一步定位。" << std::endl;
        }

        // ---- 硬门限：跌破即非零退出，供 CI/回归直接使用 ----
        if (check_thresholds_) {
            std::cout << "  >> 门限检查：" << std::endl;
            check_limit(is_read ? "读吞吐" : "写吞吐", gbps,
                        is_read ? PERF_LIMITS.read_gbps_min : PERF_LIMITS.write_gbps_min,
                        "GB/s");
            if (PERF_LIMITS.effB_min > 0.0)
                check_limit("口径B 链路载荷效率", effB * 100,
                            PERF_LIMITS.effB_min * 100, "%");
            else
                std::cout << "    [SKIP] 口径B 不考核：本位宽为 AXI 侧受限，"
                             "Flit 装不满是物理事实" << std::endl;
        }
        return gbps;
    }

    /**
     * @brief 读写同时压满时的双向报告
     *
     * 单向场景里反方向是空闲的，credit 回传占满反方向也不花钱；读写混合时
     * 两个方向都要跑业务数据，credit 回传就会真的和数据抢 Flit 槽位。
     * 这一项就是用来量化这件事的：看两个方向各自的 credit 粒度开销。
     */
    void report_mixed(const char* title, const Window& a) {
        Window b = snapshot();
        double span_ns = b.t0 - a.t0;
        unsigned long rb = b.r_beats - a.r_beats, wb = b.w_beats - a.w_beats;
        double rd_bytes = double(rb) * AXI_DATA_BYTES;
        double wr_bytes = double(wb) * AXI_DATA_BYTES;

        unsigned long txf = b.tx_flits - a.tx_flits, rxf = b.rx_flits - a.rx_flits;
        unsigned long txb = b.tx_bus_gran - a.tx_bus_gran;
        unsigned long rxb = b.rx_bus_gran - a.rx_bus_gran;
        unsigned long txc = b.tx_crd_gran - a.tx_crd_gran;
        unsigned long rxc = b.rx_crd_gran - a.rx_crd_gran;

        std::cout << "\n--- " << title << " ---" << std::endl;
        std::cout << std::fixed;
        std::cout << "  测量窗口                : " << std::setprecision(1) << span_ns
                  << " ns" << std::endl;
        std::cout << "  读 / 写 应用数据量      : " << std::setprecision(0) << rd_bytes
                  << " B / " << wr_bytes << " B" << std::endl;
        std::cout << "  >> 读吞吐 / 写吞吐      : " << std::setprecision(2)
                  << (rd_bytes / span_ns) << " GB/s / " << (wr_bytes / span_ns)
                  << " GB/s" << std::endl;
        std::cout << "  >> 双向合计吞吐         : " << std::setprecision(2)
                  << ((rd_bytes + wr_bytes) / span_ns) << " GB/s"
                  << "（双向裸带宽合计 " << std::setprecision(1)
                  << (2 * LINK_BYTES_PER_NS) << " GB/s）" << std::endl;
        auto dir = [&](const char* nm, unsigned long f, unsigned long bus,
                       unsigned long crd, double app) {
            std::cout << "  [" << nm << "] Flit " << f
                      << " / 占用率 " << std::setprecision(1)
                      << (f * FLIT_PERIOD_NS / span_ns * 100) << "%"
                      << " / 业务粒度 " << bus
                      << " / credit 粒度 " << crd
                      << "（credit 占该方向容量 " << std::setprecision(2)
                      << (f ? 100.0 * crd / (f * GRANULE_COUNT) : 0.0) << "%）"
                      << std::endl;
            std::cout << "        口径A " << std::setprecision(2)
                      << (f ? app / (f * 256.0) * 100 : 0.0) << "%  口径B "
                      << (f ? app / (f * 240.0) * 100 : 0.0) << "%" << std::endl;
        };
        dir("TX DUT→对端：写数据+读请求", txf, txb, txc, wr_bytes);
        dir("RX 对端→DUT：读数据+写响应", rxf, rxb, rxc, rd_bytes);
    }

    void report_latency() {
        auto line = [](const char* name, const LatencyStat& s) {
            std::cout << "  " << std::left << std::setw(26) << name << std::right
                      << std::fixed << std::setprecision(2)
                      << " min " << std::setw(7) << s.min() << " ns"
                      << " / avg " << std::setw(7) << s.avg() << " ns"
                      << " / max " << std::setw(7) << s.max() << " ns"
                      << "  (n=" << s.count() << ")" << std::endl;
        };
        std::cout << "\n--- PERF-3 空载单事务延迟（链路无其它流量）---" << std::endl;
        std::cout << "  读事务：" << std::endl;
        line("ARVALID→Flit 上链路 (TX)", lat_read_tx_);
        line("入站Flit→RVALID  (RX)",    lat_read_rx_);
        line("ARVALID→RVALID   (往返)",  lat_read_total_);
        std::cout << "  写事务：" << std::endl;
        line("WLAST→Flit 上链路 (TX)",  lat_write_tx_);
        line("入站Flit→BVALID  (RX)",    lat_write_rx_);
        line("WLAST→BVALID     (往返)",  lat_write_total_);

        std::cout << "  说明：链路飞行时间本场景设为 " << std::setprecision(2)
                  << tx_flight_.delay_ns() << " ns，对端存储访问时延设为 "
                  << remote.proc_delay_ns() << " ns；" << std::endl;
        std::cout << "        因此 TX / RX 两栏就是桥接单元自身引入的延迟，"
                     "链路飞行时间与存储器件时延都是可加的独立项。" << std::endl;
        std::cout << "        （5.333ns 是满载时相邻 Flit 的发送间隔，不是空载"
                     "单包的延迟，不要把它加进这里。）" << std::endl;
        std::cout << "  >> 门限检查：" << std::endl;
        check_limit("读 桥内发送侧 TX", lat_read_tx_.avg(),  PERF_LIMITS.lat_tx_max_ns,
                    "ns", false);
        check_limit("读 桥内接收侧 RX", lat_read_rx_.avg(),  PERF_LIMITS.lat_rx_max_ns,
                    "ns", false);
        check_limit("写 桥内发送侧 TX", lat_write_tx_.avg(), PERF_LIMITS.lat_tx_max_ns,
                    "ns", false);
        check_limit("写 桥内接收侧 RX", lat_write_rx_.avg(), PERF_LIMITS.lat_rx_max_ns,
                    "ns", false);
    }

    // =========================================================
    //  主控：按阶段推进
    // =========================================================
    void ctrl_thread() {
        while (!rst_n.read()) wait();
        // 等双方把初始 credit 公布完（容量大时要好几个 CrdtGrant）
        for (unsigned i = 0; i < 200; ++i) wait();

        std::cout << "\n================ 性能仿真配置 ================" << std::endl;
        std::cout << "  AXI 数据位宽    : " << AXI_DATA_WIDTH << " bit ("
                  << AXI_DATA_BYTES << "B/beat)" << std::endl;
        std::cout << "  消息粒度        : ReadData " << CFG_RDATA_GRANULES
                  << " gr / WriteDataFull " << CFG_WDATAFULL_GRANULES
                  << " gr / WriteData " << CFG_WDATA_GRANULES << " gr" << std::endl;
        std::cout << "  模型时钟        : " << CLK_PERIOD_NS << " ns ("
                  << (1000.0 / CLK_PERIOD_NS) << " MHz)，AXI 侧理论上限 "
                  << (AXI_DATA_BYTES / CLK_PERIOD_NS) << " GB/s" << std::endl;
        std::cout << "  链路            : UCIe x16 @24GT/s = " << LINK_BYTES_PER_NS
                  << " GB/s，256B Flit 周期 " << FLIT_PERIOD_NS << " ns" << std::endl;
        std::cout << "  RP 数           : " << PERF_RP_COUNT << std::endl;
        std::cout << "==============================================" << std::endl;

        // -----------------------------------------------------
        //  PERF-3 先做：链路必须是空的，延迟才有意义
        // -----------------------------------------------------
        burst_len_ = 0;                       // 单 beat 事务
        for (unsigned i = 0; i < LATENCY_SAMPLES; ++i) {
            t_ar_issue_ = t_tx_rreq_ = t_rx_data_ = t_r_beat_ = -1.0;
            unsigned long r_before = r_beats_;
            ar_todo_ = 1;
            for (unsigned c = 0; c < 200 && r_beats_ == r_before; ++c) wait();
            if (r_beats_ > r_before && t_ar_issue_ >= 0 && t_tx_rreq_ >= 0 &&
                t_rx_data_ >= 0 && t_r_beat_ >= 0) {
                lat_read_tx_.add(t_tx_rreq_ - t_ar_issue_);
                lat_read_rx_.add(t_r_beat_  - t_rx_data_);
                lat_read_total_.add(t_r_beat_ - t_ar_issue_);
            } else {
                ++perf_errors;
            }
            for (unsigned c = 0; c < 10; ++c) wait();   // 让链路彻底静下来
        }
        for (unsigned i = 0; i < LATENCY_SAMPLES; ++i) {
            t_w_last_ = t_tx_wdata_ = t_rx_data_ = t_b_beat_ = -1.0;
            unsigned long b_before = b_beats_;
            wr_todo_ = 1;
            for (unsigned c = 0; c < 200 && b_beats_ == b_before; ++c) wait();
            if (b_beats_ > b_before && t_w_last_ >= 0 && t_tx_wdata_ >= 0 &&
                t_rx_data_ >= 0 && t_b_beat_ >= 0) {
                lat_write_tx_.add(t_tx_wdata_ - t_w_last_);
                lat_write_rx_.add(t_b_beat_   - t_rx_data_);
                lat_write_total_.add(t_b_beat_ - t_w_last_);
            } else {
                ++perf_errors;
            }
            for (unsigned c = 0; c < 10; ++c) wait();
        }
        report_latency();

        // -----------------------------------------------------
        //  PERF-1 读带宽
        // -----------------------------------------------------
        burst_len_ = static_cast<uint8_t>(BURST_BEATS - 1);
        ar_todo_ = 1000000;                    // 足够大，由 credit/链路自然背压
        for (unsigned c = 0; c < WARMUP_CYCLES; ++c) wait();
        Window w1 = snapshot();
        for (unsigned c = 0; c < MEASURE_CYCLES; ++c) wait();
        double read_gbps_base = report_bandwidth("PERF-1 读带宽（持续压满）", w1, true);
        ar_todo_ = 0;
        // 等未完成读事务全部收尾，避免污染写测试
        for (unsigned c = 0; c < 4000 && !reads_idle(); ++c) wait();

        // -----------------------------------------------------
        //  PERF-2 写带宽
        // -----------------------------------------------------
        wr_todo_ = 1000000;
        for (unsigned c = 0; c < WARMUP_CYCLES; ++c) wait();
        Window w2 = snapshot();
        for (unsigned c = 0; c < MEASURE_CYCLES; ++c) wait();
        report_bandwidth("PERF-2 写带宽（持续压满，全 strobe→WriteDataFull）", w2, false);
        wr_todo_ = 0;
        for (unsigned c = 0; c < 4000 && !writes_idle(); ++c) wait();

        // -----------------------------------------------------
        //  PERF-4 读写混合：两个方向同时跑业务，credit 回传要和数据抢槽位
        // -----------------------------------------------------
        ar_todo_ = 1000000;
        wr_todo_ = 1000000;
        for (unsigned c = 0; c < WARMUP_CYCLES; ++c) wait();
        Window w3 = snapshot();
        for (unsigned c = 0; c < MEASURE_CYCLES; ++c) wait();
        report_mixed("PERF-4 读写混合（两个方向同时压满）", w3);
        ar_todo_ = 0;
        wr_todo_ = 0;
        for (unsigned c = 0; c < 8000 && !(reads_idle() && writes_idle()); ++c) wait();

        // -----------------------------------------------------
        //  PERF-5 真实 TAT：检验按 LINK_TAT_NS 反推的 FIFO 深度与初始 credit
        // -----------------------------------------------------

        std::cout << "\n--- PERF-5 真实 TAT（往返 " << std::fixed << std::setprecision(1)
                  << LINK_TAT_NS << " ns，检验 credit 深度）---" << std::endl;
        std::cout << "  单向飞行时间设为 " << std::setprecision(3)
                  << LINK_ONE_WAY_NS_FOR_TAT << " ns，往返 = 2×"
                  << LINK_ONE_WAY_NS_FOR_TAT << " = "
                  << std::setprecision(1) << LINK_TAT_NS << " ns"
                  << "（序列化 " << std::setprecision(3) << FLIT_PERIOD_NS
                  << "ns 只在满载时表现为发包间隔，不进入空载单包往返）" << std::endl;

        /*
         * 【参照组必须现场重测，不能拿 PERF-3 的数当基线】
         * PERF-3 的读往返均值 13.80ns 里混了几个 TX=8ns 的样本（AR 恰好错过
         * 一个 Flit 槽位）。PERF-5 的采样间隔更长、链路更空，TX 稳定在 4ns，
         * 两组的 TX 分布根本不同。直接相减得到的"增量"会平白少 3.8ns，看起来
         * 像延迟模型算错了，其实是基线取错了。
         * 所以这里先用完全相同的循环参数、在零飞行时间下重测一组参照，
         * 再打开飞行时间测一组，二者相减才是飞行时间的净贡献。
         */
        burst_len_ = 0;
        auto sample_read_latency = [&](LatencyStat& stat) {
            for (unsigned i = 0; i < LATENCY_SAMPLES; ++i) {
                t_ar_issue_ = t_r_beat_ = -1.0;
                unsigned long r_before = r_beats_;
                ar_todo_ = 1;
                for (unsigned c = 0; c < 400 && r_beats_ == r_before; ++c) wait();
                if (r_beats_ > r_before && t_ar_issue_ >= 0 && t_r_beat_ >= 0)
                    stat.add(t_r_beat_ - t_ar_issue_);
                else
                    ++perf_errors;
                for (unsigned c = 0; c < 30; ++c) wait();
            }
        };

        set_link_one_way_ns(0.0);
        for (unsigned c = 0; c < 200; ++c) wait();
        sample_read_latency(lat_ref_read_total_);

        set_link_one_way_ns(LINK_ONE_WAY_NS_FOR_TAT);
        for (unsigned c = 0; c < 200; ++c) wait();   // 让链路彻底静下来再测
        sample_read_latency(lat_tat_read_total_);

        double lat_delta = lat_tat_read_total_.avg() - lat_ref_read_total_.avg();
        std::cout << "  空载读往返：零飞行 " << std::setprecision(2)
                  << lat_ref_read_total_.avg() << " ns → 真实 TAT "
                  << lat_tat_read_total_.avg() << " ns（增加 " << lat_delta
                  << " ns，期望 " << std::setprecision(1) << LINK_TAT_NS << " ns）"
                  << std::endl;
        std::cout << "  >> 门限检查：" << std::endl;
        // 延迟线是按时间戳判定的，增量应当精确等于 TAT，只留一个时钟周期的量化余量
        check_limit("往返增量与 TAT 的偏差", std::fabs(lat_delta - LINK_TAT_NS),
                    CLK_PERIOD_NS, "ns", false);

        burst_len_ = static_cast<uint8_t>(BURST_BEATS - 1);
        ar_todo_ = 1000000;
        for (unsigned c = 0; c < WARMUP_CYCLES; ++c) wait();
        Window w5 = snapshot();
        for (unsigned c = 0; c < MEASURE_CYCLES; ++c) wait();
        check_thresholds_ = false;    // 门限另设：只和零飞行时间的基线比
        double read_gbps_tat =
            report_bandwidth("PERF-5 真实 TAT 下的读带宽", w5, true);
        check_thresholds_ = true;
        ar_todo_ = 0;
        for (unsigned c = 0; c < 8000 && !reads_idle(); ++c) wait();

        std::cout << "  >> credit 深度判据：" << std::endl;
        std::cout << "    零飞行基线 " << std::setprecision(2) << read_gbps_base
                  << " GB/s → 真实 TAT " << read_gbps_tat << " GB/s（保持率 "
                  << (read_gbps_base > 0 ? read_gbps_tat / read_gbps_base * 100 : 0.0)
                  << "%）" << std::endl;
        check_limit("40ns TAT 下的吞吐保持率",
                    read_gbps_base > 0 ? read_gbps_tat / read_gbps_base * 100 : 0.0,
                    95.0, "%");

        std::cout << "\n================ 正确性自检 ================" << std::endl;
        std::cout << "  AXI R beat / W beat / B beat : " << r_beats_ << " / "
                  << w_beats_ << " / " << b_beats_ << std::endl;
        std::cout << "  远端收到 RREQ/WREQ/WDATA     : " << remote.rreq_seen() << " / "
                  << remote.wreq_seen() << " / " << remote.wdata_beats() << std::endl;
        std::cout << "  读数据比对错误               : " << r_data_errors_ << std::endl;
        std::cout << "  远端写数据比对错误           : " << remote.data_mismatch() << std::endl;
        std::cout << "  协议/定界错误                : " << remote.proto_errors() << std::endl;
        std::cout << "  请求字段错误(地址标签/SIZE)  : " << remote.req_field_errors() << std::endl;
        std::cout << "  跨 Flit 消息条数（出站）     : "
                  << remote.scanner().spanning() << std::endl;
        if (r_data_errors_ || remote.data_mismatch() || remote.proto_errors() ||
            remote.req_field_errors())
            ++perf_errors;
        std::cout << "============================================" << std::endl;
        sc_stop();
    }
};

int sc_main(int, char**) {
    g_aou_verbose = false;   // 性能仿真跑几千拍，逐条打印会淹没输出且严重拖慢

    sc_clock clk("clk", CLK_PERIOD_NS, SC_NS);
    sc_signal<bool> rst_n("rst_n");

    sc_signal<bool> aw_valid("aw_valid"), aw_ready("aw_ready");
    sc_signal<bool> w_valid("w_valid"), w_ready("w_ready");
    sc_signal<bool> ar_valid("ar_valid"), ar_ready("ar_ready");
    sc_signal<AxChannel> aw_ch("aw_ch"), ar_ch("ar_ch");
    sc_signal<WChannel> w_ch("w_ch");

    sc_signal<bool> b_valid("b_valid"), b_ready("b_ready");
    sc_signal<bool> r_valid("r_valid"), r_ready("r_ready");
    sc_signal<BChannel> b_ch("b_ch");
    sc_signal<RChannel> r_ch("r_ch");

    sc_signal<FlitTransfer> flit_out("flit_out"), flit_in("flit_in");
    sc_signal<bool> flit_ready("flit_ready"), flit_in_ready("flit_in_ready");

    Axi2Flit dut("axi2flit", PERF_RP_COUNT);
    dut.clk(clk); dut.rst_n(rst_n);
    dut.aw_valid(aw_valid); dut.aw_ready(aw_ready); dut.aw_ch(aw_ch);
    dut.w_valid(w_valid); dut.w_ready(w_ready); dut.w_ch(w_ch);
    dut.ar_valid(ar_valid); dut.ar_ready(ar_ready); dut.ar_ch(ar_ch);
    dut.b_valid(b_valid); dut.b_ready(b_ready); dut.b_ch(b_ch);
    dut.r_valid(r_valid); dut.r_ready(r_ready); dut.r_ch(r_ch);
    dut.flit_out(flit_out); dut.flit_ready(flit_ready);
    dut.flit_in(flit_in); dut.flit_in_ready(flit_in_ready);

    PerfTb tb("perf_tb");
    tb.clk(clk); tb.rst_n(rst_n);
    tb.aw_valid(aw_valid); tb.aw_ready(aw_ready); tb.aw_ch(aw_ch);
    tb.w_valid(w_valid); tb.w_ready(w_ready); tb.w_ch(w_ch);
    tb.ar_valid(ar_valid); tb.ar_ready(ar_ready); tb.ar_ch(ar_ch);
    tb.b_valid(b_valid); tb.b_ready(b_ready); tb.b_ch(b_ch);
    tb.r_valid(r_valid); tb.r_ready(r_ready); tb.r_ch(r_ch);
    tb.flit_out(flit_out); tb.flit_ready(flit_ready);
    tb.flit_in(flit_in); tb.flit_in_ready(flit_in_ready);

    rst_n.write(false);
    sc_start(CLK_PERIOD_NS * 2, SC_NS);
    rst_n.write(true);
    sc_start(200000, SC_NS);

    if (!sc_end_of_simulation_invoked()) {
        ++perf_errors;
        std::cerr << "[FAIL] 性能仿真超时，控制线程未正常结束" << std::endl;
    }
    std::cout << (perf_errors == 0 ? "PERF RUN OK" : "PERF RUN HAS ERRORS") << std::endl;
    return perf_errors == 0 ? 0 : 1;
}
