/**
 * 桥功能自检。覆盖credit初始化、资源平面路由、背压、响应还原、跨帧续传和顺序检查。
 */
#include <systemc.h>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <iostream>
#include <vector>
#include "axi2flit.h"
#include "tb_common.h"
#include "aou_wire.h"

static constexpr double   CLK_PERIOD_NS = MODEL_CLK_PERIOD_NS;
static constexpr unsigned TEST_RP_COUNT = 2;
static int test_errors = 0;

SC_MODULE(BridgeTb) {
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

    // ---- 链路监视统计（由 monitor_thread 维护）----
    unsigned     app_msg_count[MAX_RESOURCE_PLANES][CREDIT_KIND_COUNT] = {};
    CreditMatrix credits_observed_from_dut{};   // 两条路径（CrdtGrant + MsgCredit）合计
    CreditMatrix header_credits_observed{};     // 仅 MsgCredit 捎带的部分
    unsigned crdt_grant_flits = 0;              // 专用 CrdtGrant Flit 数
    unsigned business_flits_with_credit = 0;    // 既载业务消息又捎带 credit 的 Flit 数
    unsigned accepted_flits = 0;
    FlitScanner tx_scanner{TEST_RP_COUNT};      // 独立实现的出站 Flit 解析器

    // DUT 的同 ID 跨 RP 顺序违例计数器。TB 与 DUT 之间没有信号可以传这个值，
    // 直接给一个指针即可（只读、只在 sc_main 里赋一次）。
    const Axi2Flit* dut_ptr = nullptr;
    unsigned long dut_order_violations() const {
        return dut_ptr ? dut_ptr->order_violations() : 0;
    }

    // ---- AXI 响应通道监视（记录真正握手成功的 beat）----
    std::vector<RChannel> r_beats;
    std::vector<BChannel> b_beats;

    SC_CTOR(BridgeTb) {
        SC_THREAD(run_thread);
        sensitive << clk.pos();
        SC_THREAD(monitor_thread);
        sensitive << clk.pos();
        SC_THREAD(axi_resp_monitor_thread);
        sensitive << clk.pos();
    }

    // =========================================================
    //  基础工具
    // =========================================================
    void check(bool condition, const std::string& message) {
        if (condition) {
            std::cout << "[PASS] " << message << std::endl;
        } else {
            ++test_errors;
            std::cerr << "[FAIL] " << message << " @" << sc_time_stamp() << std::endl;
        }
    }

    void wait_cycles(unsigned cycles) {
        for (unsigned i = 0; i < cycles; ++i) wait();
    }

    bool wait_until(const std::function<bool()>& predicate, unsigned max_cycles) {
        for (unsigned i = 0; i < max_cycles; ++i) {
            if (predicate()) return true;
            wait();
        }
        return predicate();
    }

    uint16_t make_header_credit(uint8_t rp,
                                unsigned wreq, unsigned rreq, unsigned wdata,
                                unsigned rdata = 0, unsigned wresp = 0) {
        return static_cast<uint16_t>((rp & 0x3) << 14) |
               static_cast<uint16_t>(encode_credit_amount(wreq) << 0) |
               static_cast<uint16_t>(encode_credit_amount(rreq) << 3) |
               static_cast<uint16_t>(encode_credit_amount(wdata) << 6) |
               static_cast<uint16_t>(encode_credit_amount(rdata) << 9) |
               static_cast<uint16_t>(encode_credit_amount(wresp, 3) << 12);
    }

    // FDI 输入遵循 ready/valid：valid 一旦拉高，在观察到 ready 前保持 Flit 不变。
    void send_inbound_flit(const AouFlit& flit) {
        while (!flit_in_ready.read()) wait();
        // 所有入站激励只经过 250B 内容；used_granules 故意丢弃，覆盖真实
        // UCIe 接入之后的条件，包括 credit-only、非满包和跨 Flit 的尾片段。
        FlitTransfer transfer(deserialize_aou(serialize_aou(flit)));
        flit_in.write(transfer);
        do { wait(); } while (!flit_in_ready.read());
        flit_in.write(FlitTransfer{});
        wait(); // 与下一包至少间隔一个采样沿，避免测试端产生重复握手歧义。
    }

    void send_credit_header(uint8_t rp, unsigned wreq, unsigned rreq, unsigned wdata) {
        AouFlit flit;
        flit.clear();
        flit.msg_credit = make_header_credit(rp, wreq, rreq, wdata);
        send_inbound_flit(flit);
    }

    // ---- AXI Master 侧激励 ----
    void send_ar(uint16_t id, uint64_t addr, uint8_t qos, uint8_t len = 0) {
        ar_ch.write(make_ax(id, addr, len, qos));
        ar_valid.write(true);
        do { wait(); } while (!ar_ready.read());
        ar_valid.write(false);
        wait();
    }

    void send_aw(uint16_t id, uint64_t addr, uint8_t qos, uint8_t len = 0) {
        aw_ch.write(make_ax(id, addr, len, qos));
        aw_valid.write(true);
        do { wait(); } while (!aw_ready.read());
        aw_valid.write(false);
        wait();
    }

    // 发送 beats 个 W beat（全 strobe → DUT 应选用更省粒度的 WriteDataFull）
    void send_w_burst(uint16_t id, unsigned beats, bool full_strobe = true) {
        for (unsigned i = 0; i < beats; ++i) {
            w_ch.write(make_w(id, i, i + 1 == beats, full_strobe));
            w_valid.write(true);
            do { wait(); } while (!w_ready.read());
            w_valid.write(false);
            wait();
        }
    }

    // 构造一个同时含 ReadData 与 WriteResp 的入站 Flit
    void send_return_messages(uint8_t r_rp, uint8_t b_rp,
                              uint16_t extra_header_credit = 0) {
        RChannel r;
        // AXUSER/PROF 只有 12bit（AXI_USER_MASK=0xFFF），激励值必须落在该范围内，
        // 否则失败的是测试激励而不是 DUT。
        r.id = 0x31; r.resp = 0; r.last = true; r.user = 0x357;
        std::fill(std::begin(r.data), std::end(r.data), 0x5A);

        BChannel b;
        b.id = 0x22; b.resp = 2; b.user = 0x468;

        AouFlit flit;
        flit.clear();
        flit.msg_credit = extra_header_credit;
        sc_assert(flit.pack_message(MsgBuilder::build_read_data(r, r_rp)));
        sc_assert(flit.pack_message(MsgBuilder::build_write_resp(b, b_rp)));
        send_inbound_flit(flit);
    }

    // =========================================================
    //  主测试流程
    // =========================================================
    void run_thread() {
        aw_valid.write(false);
        w_valid.write(false);
        ar_valid.write(false);
        aw_ch.write(AxChannel{});
        w_ch.write(WChannel{});
        ar_ch.write(AxChannel{});
        b_ready.write(true);
        r_ready.write(true);
        flit_ready.write(true);
        flit_in.write(FlitTransfer{});

        while (!rst_n.read()) wait();
        wait();

        std::cout << "\n[配置] AXI_DATA_WIDTH=" << AXI_DATA_WIDTH
                  << "b, RP_COUNT=" << TEST_RP_COUNT
                  << ", ReadData=" << CFG_RDATA_GRANULES << "gr"
                  << ", WriteDataFull=" << CFG_WDATAFULL_GRANULES << "gr" << std::endl;

        std::cout << "\n===== TC1: 启动 credit 公布与 RP 参数化 =====" << std::endl;
        // 单个 credit 字段一次最多编码 128 granule，接收容量（1024b 下 486 granule）
        // 必须分多个 CrdtGrant 累加发满，因此这里检查的是"累计值"而非"一次到位"。
        check(wait_until([this] { return crdt_grant_flits >= 1; }, 10),
              "DUT 退出复位后主动发送 CrdtGrant");
        bool published = wait_until([this] {
            for (unsigned rp = 0; rp < TEST_RP_COUNT; ++rp) {
                if (credits_observed_from_dut[rp][credit_kind_index(CreditKind::ReadData)]
                        < RX_RDATA_CREDITS_PER_RP) return false;
                if (credits_observed_from_dut[rp][credit_kind_index(CreditKind::WriteResp)]
                        < RX_WRESP_CREDITS_PER_RP) return false;
            }
            return true;
        }, 80);
        check(published, "初始 RDATA/WRESP 接收容量被完整公布（可跨多条 CrdtGrant 累加）");
        for (unsigned rp = 0; rp < TEST_RP_COUNT; ++rp) {
            check(credits_observed_from_dut[rp][credit_kind_index(CreditKind::ReadData)] ==
                      RX_RDATA_CREDITS_PER_RP &&
                  credits_observed_from_dut[rp][credit_kind_index(CreditKind::WriteResp)] ==
                      RX_WRESP_CREDITS_PER_RP,
                  "公布的 credit 总量不多不少等于接收 FIFO granule 容量");
            check(credits_observed_from_dut[rp][credit_kind_index(CreditKind::WriteReq)] == 0 &&
                  credits_observed_from_dut[rp][credit_kind_index(CreditKind::ReadReq)] == 0 &&
                  credits_observed_from_dut[rp][credit_kind_index(CreditKind::WriteData)] == 0,
                  "initiator 侧未实现的入站请求/写数据资源初始 credit 为 0");
        }

        // 对端初始只给 RP0 一个 RREQ，给 RP1 一组写请求/数据 credit。
        // WDATA 给 32 granule：1024b 的 WriteDataFull 是 27 granule，给 8 会不够。
        CreditMatrix peer_initial{};
        peer_initial[0][credit_kind_index(CreditKind::ReadReq)]   = 4;
        peer_initial[1][credit_kind_index(CreditKind::WriteReq)]  = 4;
        peer_initial[1][credit_kind_index(CreditKind::WriteData)] = 32;
        AouFlit peer_grant;
        peer_grant.clear();
        sc_assert(peer_grant.pack_message(
            build_crdt_grant_message(peer_initial, TEST_RP_COUNT)));
        send_inbound_flit(peer_grant);

        std::cout << "\n===== TC2: AW/W 路由到 RP1 =====" << std::endl;
        send_aw(0x10, tb_addr(0x2000'0000), 1);
        send_w_burst(0x10, 1);
        check(wait_until([this] {
            return app_msg_count[1][credit_kind_index(CreditKind::WriteReq)] == 1 &&
                   app_msg_count[1][credit_kind_index(CreditKind::WriteData)] == 1;
        }, 20), "AW 的 QOS 映射到 RP1，W beat 继承同一 RP");

        std::cout << "\n===== TC3: FDI ready/valid 背压保持 =====" << std::endl;
        flit_ready.write(false);
        unsigned before_accept = accepted_flits;
        send_ar(0x01, tb_addr(0x1000'0000), 0);
        check(wait_until([this] { return flit_out.read().valid; }, 15),
              "有 credit 的 ReadReq 形成有效 Flit");
        FlitTransfer stalled = flit_out.read();
        wait_cycles(3);
        check(flit_out.read().valid && flit_out.read() == stalled,
              "flit_ready=0 时 valid、header 和 payload 保持稳定");
        check(accepted_flits == before_accept,
              "背压期间链路监视器没有误计握手");
        flit_ready.write(true);
        check(wait_until([this] {
            return app_msg_count[0][credit_kind_index(CreditKind::ReadReq)] == 1;
        }, 8), "解除背压后 Flit 仅被接收一次");

        std::cout << "\n===== TC4: credit 耗尽及多 RP 独立前进 =====" << std::endl;
        send_ar(0x02, tb_addr(0x1000'0100), 0); // RP0 只剩 1 granule，不够 3，应等待
        send_ar(0x03, tb_addr(0x1000'0200), 1); // RP1 尚未获得 RREQ credit，也应等待
        wait_cycles(8);
        check(app_msg_count[0][credit_kind_index(CreditKind::ReadReq)] == 1 &&
              app_msg_count[1][credit_kind_index(CreditKind::ReadReq)] == 0,
              "credit 为 0 时消息可以本地排队但不会上链路");

        send_credit_header(1, 0, 4, 0);
        check(wait_until([this] {
            return app_msg_count[1][credit_kind_index(CreditKind::ReadReq)] == 1;
        }, 15), "RP0 仍阻塞时，RP1 获得 credit 后可独立前进");
        check(app_msg_count[0][credit_kind_index(CreditKind::ReadReq)] == 1,
              "RP1 grant 不会误增加 RP0 credit");

        send_credit_header(0, 0, 4, 0);
        check(wait_until([this] {
            return app_msg_count[0][credit_kind_index(CreditKind::ReadReq)] == 2;
        }, 15), "RP0 补充 credit 后恢复发送");

        std::cout << "\n===== TC5: Flit 解包、AXI B/R 保持及 credit 回填 =====" << std::endl;
        b_ready.write(false);
        r_ready.write(false);
        send_return_messages(0, 1);
        check(wait_until([this] { return b_valid.read() && r_valid.read(); }, 12),
              "同一入站 Flit 被拆分为 AXI R 和 B 两个独立通道");
        BChannel held_b = b_ch.read();
        RChannel held_r = r_ch.read();
        wait_cycles(3);
        check(b_valid.read() && b_ch.read() == held_b,
              "BREADY=0 时 BVALID 和响应内容保持稳定");
        check(r_valid.read() && r_ch.read() == held_r,
              "RREADY=0 时 RVALID 和读数据内容保持稳定");
        check(held_b.id == 0x22 && held_b.resp == 2 && held_b.user == 0x468,
              "WriteResp 字段正确还原到 AXI B");
        check(held_r.id == 0x31 && held_r.last && held_r.user == 0x357 &&
              held_r.data[0] == 0x5A && held_r.data[AXI_DATA_BYTES - 1] == 0x5A,
              "ReadData 字段正确还原到 AXI R（含首尾数据字节）");

        b_ready.write(true);
        r_ready.write(true);
        check(wait_until([this] { return !b_valid.read() && !r_valid.read(); }, 6),
              "AXI B/R 握手后释放接收缓冲");
        // 归还量按离散编码分批发放（27 granule → 16+8+1+1+1），故用累计值判定。
        check(wait_until([this] {
            return credits_observed_from_dut[0][credit_kind_index(CreditKind::ReadData)] >=
                       RX_RDATA_CREDITS_PER_RP + unsigned(CFG_RDATA_GRANULES) &&
                   credits_observed_from_dut[1][credit_kind_index(CreditKind::WriteResp)] >=
                       RX_WRESP_CREDITS_PER_RP + unsigned(WRESP_GRANULES);
        }, 40), "释放的 RDATA/WRESP granule 通过 credit 通道完整归还");

        std::cout << "\n===== TC6: MsgCredit 捎带回填 =====" << std::endl;
        // 制造"业务消息与待归还 credit 同时存在"的时刻：先背压 FDI 让 DUT 攒住
        // 一个待发 Flit，同时让 R/B 完成 AXI 握手产生待归还 credit，然后放开背压。
        // 时序要点：专用 CrdtGrant 只在"输出寄存器空闲"时才会发出，所以先用一条
        // 业务 Flit 把输出寄存器占住，再制造待归还 credit —— 此时 credit 无法走
        // 专用 CrdtGrant 通道，只能等下一个业务 Flit 的 MsgCredit 捎带出去。
        unsigned piggyback_before = business_flits_with_credit;
        send_credit_header(0, 0, 8, 0);       // 预备 2 条 ReadReq 的 credit
        flit_ready.write(false);
        send_ar(0x04, tb_addr(0x1000'0300), 0);        // ① 占住输出寄存器
        wait_cycles(2);
        send_return_messages(0, 0);           // ② R/B 在 AXI 侧握手 → 产生待归还 credit
        wait_cycles(6);
        send_ar(0x05, tb_addr(0x1000'0400), 0);        // ③ 在建 Flit 里放一条业务消息
        wait_cycles(4);
        flit_ready.write(true);               // ④ 放行：③ 的 Flit 应捎带 ② 的 credit
        check(wait_until([this, piggyback_before] {
            return business_flits_with_credit > piggyback_before;
        }, 25), "业务 Flit 的 MsgCredit 字段成功捎带 credit 回传");

        std::cout << "\n===== TC7: 发送方向消息跨 Flit 续传 =====" << std::endl;
        // 6 个单 beat 写突发 = 6×(WriteReq 3gr + WriteDataFull Ngr)。
        //   256b ：18 + 7×N → 第 5 个 WriteDataFull 落在 46 granule 处被截断
        //   1024b：18 + 27  → 第 2 个 WriteDataFull 就会被截断
        // 两种配置下 Flit 边界都必然落在消息中间，跨包续传是唯一正确解。
        const unsigned kBursts = 6;
        unsigned wreq_before  = app_msg_count[0][credit_kind_index(CreditKind::WriteReq)];
        unsigned wdata_before = app_msg_count[0][credit_kind_index(CreditKind::WriteData)];
        unsigned span_before  = static_cast<unsigned>(tx_scanner.spanning());
        send_credit_header(0, 64, 0, 128);
        send_credit_header(0, 64, 0, 128);    // 1024b 需要 6×27=162 granule
        flit_ready.write(false);               // 背压，逼 DUT 把 Flit 填满
        for (unsigned i = 0; i < kBursts; ++i) send_aw(0x40 + i, tb_addr(0x3000'0000 + i * 0x100), 0);
        for (unsigned i = 0; i < kBursts; ++i) send_w_burst(0x40 + i, 1);
        wait_cycles(4);
        flit_ready.write(true);
        check(wait_until([this, wreq_before, wdata_before] {
            return app_msg_count[0][credit_kind_index(CreditKind::WriteReq)]
                       >= wreq_before + kBursts &&
                   app_msg_count[0][credit_kind_index(CreditKind::WriteData)]
                       >= wdata_before + kBursts;
        }, 60), "跨 Flit 续传后所有写请求/写数据消息都能被独立解析器完整还原");
        check(tx_scanner.spanning() > span_before,
              "确实产生了跨 Flit 的消息（否则本用例没有覆盖到续传路径）");
        check(tx_scanner.errors() == 0,
              "独立解析器在整个发送流上没有发现定界错误");

        std::cout << "\n===== TC8: 接收方向跨 Flit 消息重组 =====" << std::endl;
        // 构造 1 条 WriteResp + 6 条 ReadData。1 granule 的错位使得后面的
        // ReadData 必然横跨 Flit 边界（256b：49 granule；1024b：163 granule）。
        std::size_t r_before = r_beats.size();
        std::size_t b_before = b_beats.size();
        std::vector<AouMessage> inbound;
        BChannel spanning_b;
        spanning_b.id = 0x77; spanning_b.resp = 0; spanning_b.user = 0x0ABC;
        inbound.push_back(MsgBuilder::build_write_resp(spanning_b, 0));
        for (unsigned i = 0; i < 6; ++i) {
            RChannel r;
            r.id = 0x50 + i; r.resp = 0; r.last = (i == 5); r.user = 0x100 + i;
            fill_rdata_pattern(r.data, static_cast<uint16_t>(0x50 + i), i);
            inbound.push_back(MsgBuilder::build_read_data(r, 0));
        }
        std::vector<AouFlit> inbound_flits = pack_messages_into_flits(inbound);
        check(inbound_flits.size() >= 2, "激励确实被拆成了多个 Flit（存在跨包消息）");
        for (const AouFlit& f : inbound_flits) send_inbound_flit(f);

        check(wait_until([this, r_before] { return r_beats.size() >= r_before + 6; }, 60),
              "跨 Flit 的 6 条 ReadData 全部重组成 AXI R beat");
        check(b_beats.size() >= b_before + 1, "同批次的 WriteResp 也正确还原");
        bool rx_ok = (r_beats.size() >= r_before + 6);
        if (rx_ok) {
            for (unsigned i = 0; i < 6 && rx_ok; ++i) {
                const RChannel& r = r_beats[r_before + i];
                // 逐字节核对随字节位置变化的图样，检测字节顺序颠倒和位错位。
                rx_ok = (r.id == 0x50 + i) && (r.user == 0x100 + i) &&
                        (r.last == (i == 5)) &&
                        (check_rdata_pattern(r.data, static_cast<uint16_t>(0x50 + i), i) < 0);
            }
        }
        check(rx_ok, "重组后的 R beat 顺序、ID、USER、RLAST 与数据内容全部正确");

        std::cout << "\n===== TC9: 同 ID 跨 RP 顺序约束（资源平面顺序约束）=====" << std::endl;
        /*
         * 这里分两步：
         *   ① 先确认前面 8 个用例（都遵守约束：每个 ID 只用一个 QoS）没有
         *      触发任何违例 —— 如果检查器本身太敏感、会误报，这一条就挂了；
         *   ② 再用 RpOrderGuard 单独做一组"故意违规"的负向验证。
         * 第二步不通过 DUT 走激励，原因是：一旦真的把同 ID 劈到两个 RP，
         * 后续 R/B 的响应顺序就是不确定的，TB 自己的记分板也没法给出稳定
         * 期望值 —— 那样测的是"乱序之后会怎样"，而不是"能否检出乱序风险"。
         * 检出能力用检查器的直接单测覆盖更干净、更确定。
         */
        check(dut_order_violations() == 0,
              "遵守约束的 8 个用例全程没有误报同 ID 跨 RP 违例");

        {
            RpOrderGuard g("单测");
            // 合规：同一个 ID 反复用同一个 RP
            bool ok1 = g.bind(0x123, 0) && g.bind(0x123, 0) && g.bind(0x123, 0);
            check(ok1 && g.violations() == 0, "同 ID 固定映射到同一 RP：判为合规");

            // 合规：不同 ID 落在不同 RP，互不相干
            bool ok2 = g.bind(0x124, 1) && g.bind(0x125, 1);
            check(ok2 && g.violations() == 0, "不同 ID 分散到不同 RP：判为合规");

            // 违规：ID 0x123 还有 3 笔未完成，却要换到 RP1
            bool bad = g.bind(0x123, 1);
            check(!bad && g.violations() == 1,
                  "同 ID 未完成事务被劈到第二个 RP：检出 1 次违例");

            // 全部销账后换 RP 是允许的 —— 此时已不存在顺序歧义
            for (unsigned i = 0; i < 4; ++i) g.retire(0x123);   // 3 笔合规 + 1 笔违规
            bool ok3 = g.bind(0x123, 1);
            check(ok3 && g.violations() == 1,
                  "该 ID 全部完成后改用另一个 RP：不再计为违例");

            // ID 只看低 AXI_ID_WIDTH 位，超出范围的高位不应造成表项串扰
            g.reset();
            check(g.bind(0x001, 0) && g.bind(AXI_ID_MASK + 1 + 1, 1) == false,
                  "ID 按 AXI_ID_MASK 截断后落到同一表项，跨 RP 仍能检出");
        }

        wait_cycles(5);
        std::cout << "\n===== 测试汇总 =====" << std::endl;
        std::cout << "AXI_DATA_WIDTH      : " << AXI_DATA_WIDTH << "b" << std::endl;
        std::cout << "Accepted FDI flits  : " << accepted_flits << std::endl;
        std::cout << "  dedicated CrdtGrant: " << crdt_grant_flits << std::endl;
        std::cout << "  业务 Flit 捎带credit: " << business_flits_with_credit << std::endl;
        std::cout << "TX 消息总数         : " << tx_scanner.messages()
                  << "（其中跨 Flit " << tx_scanner.spanning() << " 条）" << std::endl;
        std::cout << "TX 解析错误         : " << tx_scanner.errors() << std::endl;
        std::cout << "AXI R/B beat 数     : " << r_beats.size()
                  << " / " << b_beats.size() << std::endl;
        std::cout << "同 ID 跨 RP 顺序违例 : " << dut_order_violations() << std::endl;
        std::cout << "errors              : " << test_errors << std::endl;
        sc_stop();
    }

    void check_req_fields_on_wire(const AouMessage& msg) {
        if (msg.type != MsgType::ReadReq && msg.type != MsgType::WriteReq) return;
        AxChannel ax;
        if (!MsgDecoder::decode_req(msg, ax)) {
            ++test_errors;
            std::cerr << "[FAIL] 出站请求消息无法解码 @" << sc_time_stamp() << std::endl;
            return;
        }
        if ((ax.addr >> 32) != TB_ADDR_TAG) {
            ++test_errors;
            std::cerr << "[FAIL] 链路上的 AxADDR 高 32 位 = 0x" << std::hex
                      << (ax.addr >> 32) << "，期望 0x" << TB_ADDR_TAG << std::dec
                      << " @" << sc_time_stamp() << std::endl;
        }
        if (ax.size != AXI_SIZE_CODE) {
            ++test_errors;
            std::cerr << "[FAIL] 链路上的 AxSIZE = " << unsigned(ax.size)
                      << "，期望 " << unsigned(AXI_SIZE_CODE)
                      << " @" << sc_time_stamp() << std::endl;
        }
        if (ax.burst != 1) {
            ++test_errors;
            std::cerr << "[FAIL] 链路上的 AxBURST 不是 INCR @"
                      << sc_time_stamp() << std::endl;
        }
    }

    // =========================================================
    //  出站 Flit 监视：用独立实现的 FlitScanner 解析，交叉验证打包器
    // =========================================================
    void monitor_thread() {
        while (true) {
            wait();
            FlitTransfer transfer = flit_out.read();
            if (!transfer.valid || !flit_ready.read()) continue;
            ++accepted_flits;
            // 出站同样穿过线格式边界，再由独立 FlitScanner 核对消息。
            const AouFlit flit = deserialize_aou(serialize_aou(transfer.flit));

            bool had_header_credit = (flit.msg_credit != 0);
            decode_header_credits(flit.msg_credit, TEST_RP_COUNT,
                [this](const CreditUpdate& update) {
                    credits_observed_from_dut[update.rp][credit_kind_index(update.kind)] +=
                        update.granules;
                    header_credits_observed[update.rp][credit_kind_index(update.kind)] +=
                        update.granules;
                });

            bool had_business_msg = false;
            tx_scanner.scan(flit, [&](const AouMessage& msg) {
                if (msg.type == MsgType::Misc) {
                    CreditMatrix grants{};
                    if (decode_crdt_grant_message(msg, TEST_RP_COUNT, grants)) {
                        ++crdt_grant_flits;
                        for (unsigned rp = 0; rp < TEST_RP_COUNT; ++rp)
                            for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k)
                                credits_observed_from_dut[rp][k] += grants[rp][k];
                    } else {
                        ++test_errors;
                        std::cerr << "[FAIL] 无法解析的 Misc 消息 @"
                                  << sc_time_stamp() << std::endl;
                    }
                } else {
                    had_business_msg = true;
                    CreditKind kind = msgtype_to_credit_kind(msg.type);
                    if (msg.rp < TEST_RP_COUNT && kind != CreditKind::Count)
                        ++app_msg_count[msg.rp][credit_kind_index(kind)];
                    check_req_fields_on_wire(msg);
                }
            });
            if (had_business_msg && had_header_credit) ++business_flits_with_credit;
        }
    }

    // =========================================================
    //  AXI 响应通道监视：只记录 valid && ready 的那一拍，即真正的 beat
    // =========================================================
    void axi_resp_monitor_thread() {
        while (true) {
            wait();
            if (r_valid.read() && r_ready.read()) r_beats.push_back(r_ch.read());
            if (b_valid.read() && b_ready.read()) b_beats.push_back(b_ch.read());
        }
    }
};

int sc_main(int, char**) {
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

    Axi2Flit dut("axi2flit", TEST_RP_COUNT);
    dut.clk(clk); dut.rst_n(rst_n);
    dut.aw_valid(aw_valid); dut.aw_ready(aw_ready); dut.aw_ch(aw_ch);
    dut.w_valid(w_valid); dut.w_ready(w_ready); dut.w_ch(w_ch);
    dut.ar_valid(ar_valid); dut.ar_ready(ar_ready); dut.ar_ch(ar_ch);
    dut.b_valid(b_valid); dut.b_ready(b_ready); dut.b_ch(b_ch);
    dut.r_valid(r_valid); dut.r_ready(r_ready); dut.r_ch(r_ch);
    dut.flit_out(flit_out); dut.flit_ready(flit_ready);
    dut.flit_in(flit_in); dut.flit_in_ready(flit_in_ready);

    BridgeTb tb("tb");
    tb.clk(clk); tb.rst_n(rst_n);
    tb.aw_valid(aw_valid); tb.aw_ready(aw_ready); tb.aw_ch(aw_ch);
    tb.w_valid(w_valid); tb.w_ready(w_ready); tb.w_ch(w_ch);
    tb.ar_valid(ar_valid); tb.ar_ready(ar_ready); tb.ar_ch(ar_ch);
    tb.b_valid(b_valid); tb.b_ready(b_ready); tb.b_ch(b_ch);
    tb.r_valid(r_valid); tb.r_ready(r_ready); tb.r_ch(r_ch);
    tb.flit_out(flit_out); tb.flit_ready(flit_ready);
    tb.flit_in(flit_in); tb.flit_in_ready(flit_in_ready);
    tb.dut_ptr = &dut;      // 供 TC9 读取 DUT 的顺序违例计数

    sc_trace_file* tf = sc_create_vcd_trace_file("sim/waveform");
    tf->set_time_unit(1, SC_NS);
    sc_trace(tf, clk, "clk"); sc_trace(tf, rst_n, "rst_n");
    sc_trace(tf, aw_valid, "axi.aw_valid"); sc_trace(tf, aw_ready, "axi.aw_ready");
    sc_trace(tf, w_valid, "axi.w_valid"); sc_trace(tf, w_ready, "axi.w_ready");
    sc_trace(tf, ar_valid, "axi.ar_valid"); sc_trace(tf, ar_ready, "axi.ar_ready");
    sc_trace(tf, b_valid, "axi.b_valid"); sc_trace(tf, b_ready, "axi.b_ready");
    sc_trace(tf, r_valid, "axi.r_valid"); sc_trace(tf, r_ready, "axi.r_ready");
    sc_trace(tf, flit_out, "fdi.tx"); sc_trace(tf, flit_ready, "fdi.tx_ready");
    sc_trace(tf, flit_in, "fdi.rx"); sc_trace(tf, flit_in_ready, "fdi.rx_ready");

    rst_n.write(false);
    sc_start(CLK_PERIOD_NS * 2, SC_NS);
    rst_n.write(true);
    sc_start(4000, SC_NS);
    sc_close_vcd_trace_file(tf);

    if (!sc_end_of_simulation_invoked()) {
        ++test_errors;
        std::cerr << "[FAIL] 仿真超时，测试线程未正常结束" << std::endl;
    }
    std::cout << (test_errors == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << std::endl;
    return test_errors == 0 ? 0 : 1;
}
