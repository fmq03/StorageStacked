/**
 * AXI事务激励与全链路自检顶层。实例化桥、链路、响应端和简单突发存储。
 * 独立记分板仅根据AXI握手维护参考字节数据，不读取被测存储内容。
 */
#include "axi2flit.h"
#include "ucie_link.h"
#include "aou_target.h"
#include "simple_burst_memory.h"
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

SC_MODULE(FullLinkTb) {
    static constexpr uint64_t BASE = 0x1234567800000000ULL;
    static constexpr unsigned MEM_SIZE = 128 * 1024;
    Config cfg;
    unsigned rp_count;
    bool stress, replay, corrupt_expected;
    sc_clock clk{"clk", 2, SC_NS};
    sc_signal<bool> rst{"rst_n"};
    sc_signal<unsigned> state{"link_state"}, testcase{"testcase"};
    sc_signal<bool> av, ar, wv, wr, arv, arr, bv, br, rv, rr;
    sc_signal<AxChannel> aw, ra;
    sc_signal<WChannel> w;
    sc_signal<BChannel> b;
    sc_signal<RChannel> r;
    sc_signal<FlitTransfer> tx, rx;
    sc_signal<bool> txr, rxr;
    sc_signal<sc_biguint<AXI_DATA_WIDTH>> wave_wdata, wave_rdata;
    sc_signal<sc_biguint<AXI_DATA_BYTES>> wave_wstrb;
    Stats stats;
    sc_fifo<FdiFlit> soc_tx, soc_rx, mem_tx, mem_rx;
    sc_fifo<SimpleMemRequest> requests;
    sc_fifo<SimpleMemResponse> responses;
    Axi2Flit bridge;
    UcieAouAdapter adapter;
    UcieLink link;
    AouTarget target;
    SimpleBurstMemory memory;
    bool done = false;
    unsigned failures = 0, cases = 0;
    uint64_t aw_count = 0, w_count = 0, ar_count = 0, b_count = 0, r_count = 0;
    uint64_t b_stalls = 0, r_stalls = 0;
    std::ofstream csv;

    SC_HAS_PROCESS(FullLinkTb);
    FullLinkTb(sc_module_name name, Config config, unsigned planes,
               bool pressure, bool errors, bool negative, const std::string& prefix)
        : sc_module(name), cfg(config), rp_count(planes), stress(pressure),
          replay(errors), corrupt_expected(negative),
          soc_tx("soc_tx", pressure ? 1 : 8), soc_rx("soc_rx", pressure ? 1 : 8),
          mem_tx("mem_tx", pressure ? 1 : 8), mem_rx("mem_rx", pressure ? 1 : 8),
          requests("requests", pressure ? 1 : 4), responses("responses", pressure ? 1 : 4),
          bridge("bridge", planes), adapter("adapter", cfg),
          link("link", cfg, &stats, sc_time(cfg.ui_fs(), SC_FS)),
          target("target", cfg, planes),
          memory("memory", BASE, MEM_SIZE, sc_time(pressure ? 80 : 20, SC_NS)),
          csv(prefix + ".csv") {
        if (!csv) throw std::runtime_error("cannot open transaction CSV");
        bridge.clk(clk); bridge.rst_n(rst);
        bridge.aw_valid(av); bridge.aw_ready(ar); bridge.aw_ch(aw);
        bridge.w_valid(wv); bridge.w_ready(wr); bridge.w_ch(w);
        bridge.ar_valid(arv); bridge.ar_ready(arr); bridge.ar_ch(ra);
        bridge.b_valid(bv); bridge.b_ready(br); bridge.b_ch(b);
        bridge.r_valid(rv); bridge.r_ready(rr); bridge.r_ch(r);
        bridge.flit_out(tx); bridge.flit_ready(txr);
        bridge.flit_in(rx); bridge.flit_in_ready(rxr);
        adapter.clk(clk); adapter.rst_n(rst); adapter.link_state(state);
        adapter.tx(tx); adapter.tx_ready(txr); adapter.rx(rx); adapter.rx_ready(rxr);
        adapter.fifo_tx(soc_tx); adapter.fifo_rx(soc_rx);
        link.soc_tx_in(soc_tx); link.soc_rx_out(soc_rx);
        link.mem_rx_out(mem_rx); link.mem_tx_in(mem_tx); link.link_state(state);
        target.clk(clk); target.rst_n(rst); target.link_rx(mem_rx); target.link_tx(mem_tx);
        target.mem_req(requests); target.mem_rsp(responses);
        memory.request(requests); memory.response(responses);
        csv << "time_ns,case,channel,id,address,beat,last,resp,data_hex,strb_bits\n";
        SC_THREAD(stimulus);
        SC_THREAD(monitor); sensitive << clk.posedge_event();
        SC_THREAD(ready_driver); sensitive << clk.negedge_event();
        SC_METHOD(trace_data); sensitive << w << r;
    }

    // 将 AXI 字段拆成可直接查看的波形信号，数据总线包含全部字节。
    void trace(sc_trace_file* f) {
        sc_trace(f, clk, "clk"); sc_trace(f, rst, "rst_n");
        sc_trace(f, state, "link_state"); sc_trace(f, testcase, "testcase");
        sc_trace(f, av, "axi.awvalid"); sc_trace(f, ar, "axi.awready");
        sc_trace(f, wv, "axi.wvalid"); sc_trace(f, wr, "axi.wready");
        sc_trace(f, arv, "axi.arvalid"); sc_trace(f, arr, "axi.arready");
        sc_trace(f, bv, "axi.bvalid"); sc_trace(f, br, "axi.bready");
        sc_trace(f, rv, "axi.rvalid"); sc_trace(f, rr, "axi.rready");
        auto addr = [&](const AxChannel& a, const std::string& n) {
            sc_trace(f, a.id, n + "id"); sc_trace(f, a.addr, n + "addr");
            sc_trace(f, a.len, n + "len"); sc_trace(f, a.size, n + "size");
            sc_trace(f, a.burst, n + "burst"); sc_trace(f, a.qos, n + "qos");
            sc_trace(f, a.user, n + "user"); sc_trace(f, a.lock, n + "lock");
            sc_trace(f, a.cache, n + "cache"); sc_trace(f, a.prot, n + "prot");
        };
        addr(aw.read(), "axi.aw"); addr(ra.read(), "axi.ar");
        sc_trace(f, wave_wdata, "axi.wdata"); sc_trace(f, wave_wstrb, "axi.wstrb");
        sc_trace(f, w.read().last, "axi.wlast"); sc_trace(f, w.read().user, "axi.wuser");
        sc_trace(f, b.read().id, "axi.bid"); sc_trace(f, b.read().resp, "axi.bresp");
        sc_trace(f, b.read().user, "axi.buser");
        sc_trace(f, wave_rdata, "axi.rdata"); sc_trace(f, r.read().last, "axi.rlast");
        sc_trace(f, r.read().id, "axi.rid"); sc_trace(f, r.read().resp, "axi.rresp");
        sc_trace(f, r.read().user, "axi.ruser");
        sc_trace(f, tx, "flit.tx"); sc_trace(f, txr, "flit.tx_ready");
        sc_trace(f, rx, "flit.rx"); sc_trace(f, rxr, "flit.rx_ready");
    }

private:
    struct WriteExpected { AxChannel a; std::vector<WChannel> data; };
    struct ReadExpected { RChannel value; uint64_t address; unsigned beat; };
    std::deque<WriteExpected> pending_w_;
    std::map<unsigned, std::deque<BChannel>> expected_b_;
    std::map<unsigned, std::deque<ReadExpected>> expected_r_;
    std::map<uint64_t, uint8_t> reference_;
    std::mt19937 stimulus_rng_{0xA012}, ready_rng_{0xB031};
    unsigned hold_r_ = 0, hold_b_ = 0;
    bool random_ready_ = false;

    void check(bool ok, const std::string& what) {
        if (ok) return;
        ++failures;
        if (failures <= 20) std::cerr << "[FAIL] " << sc_time_stamp() << " " << what << "\n";
    }
    void tick() { wait(clk.negedge_event()); }
    void cycles(unsigned n) { while (n--) tick(); }
    template<class Pred> void until(Pred p, const char* what) {
        for (unsigned n = 0; n < 100000; ++n) { if (p()) return; tick(); }
        throw std::runtime_error(what);
    }
    static unsigned status(const AxChannel& a) {
        uint64_t len = (uint64_t(a.len) + 1) << a.size;
        if (a.addr < BASE || a.addr - BASE >= MEM_SIZE || len > MEM_SIZE - (a.addr - BASE)) return 3;
        return a.lock ? 2 : 0;
    }
    static std::string hex_data(const uint8_t* data, unsigned n) {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned i = n; i; --i) out << std::setw(2) << unsigned(data[i - 1]);
        return out.str();
    }
    void log(const char* ch, unsigned id, uint64_t addr, unsigned beat,
             bool last, unsigned resp, const uint8_t* data = nullptr, const uint8_t* strb = nullptr) {
        csv << sc_time_stamp().to_seconds() * 1e9 << ',' << testcase.read() << ',' << ch << ','
            << id << ",0x" << std::hex << addr << std::dec << ',' << beat << ',' << last << ',' << resp << ',';
        if (data) csv << hex_data(data, AXI_DATA_BYTES);
        csv << ',';
        if (strb) for (int j = AXI_DATA_BYTES - 1; j >= 0; --j) csv << (strb[j] ? '1' : '0');
        csv << '\n';
    }
    void trace_data() {
        sc_biguint<AXI_DATA_WIDTH> wd = 0, rd = 0;
        sc_biguint<AXI_DATA_BYTES> st = 0;
        for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
            wd.range(j * 8 + 7, j * 8) = w.read().data[j];
            rd.range(j * 8 + 7, j * 8) = r.read().data[j];
            st[j] = w.read().strb[j] != 0;
        }
        wave_wdata.write(wd); wave_rdata.write(rd); wave_wstrb.write(st);
    }
    // 仅在上升沿握手时更新独立期望数据和事务账本，并检查五通道背压稳定性。
    void monitor() {
        bool old_av = false, old_wv = false, old_arv = false, old_bv = false, old_rv = false;
        AxChannel saved_aw, saved_ar; WChannel saved_w; BChannel saved_b; RChannel saved_r;
        while (true) {
            wait();
            if (!rst.read()) continue;
            if (old_av) check(av.read() && aw.read() == saved_aw, "AW changed under backpressure");
            if (old_wv) check(wv.read() && w.read() == saved_w, "W changed under backpressure");
            if (old_arv) check(arv.read() && ra.read() == saved_ar, "AR changed under backpressure");
            if (old_bv) check(bv.read() && b.read() == saved_b, "B changed under backpressure");
            if (old_rv) check(rv.read() && r.read() == saved_r, "R changed under backpressure");
            old_av = av.read() && !ar.read(); saved_aw = aw.read();
            old_wv = wv.read() && !wr.read(); saved_w = w.read();
            old_arv = arv.read() && !arr.read(); saved_ar = ra.read();
            old_bv = bv.read() && !br.read(); saved_b = b.read();
            old_rv = rv.read() && !rr.read(); saved_r = r.read();
            b_stalls += old_bv; r_stalls += old_rv;
            if (av.read() && ar.read()) {
                ++aw_count; pending_w_.push_back({aw.read(), {}});
                log("AW", aw.read().id, aw.read().addr, 0, false, 0);
            }
            if (wv.read() && wr.read()) {
                ++w_count;
                if (pending_w_.empty()) { check(false, "W accepted without AW"); continue; }
                auto& p = pending_w_.front();
                check(w.read().last == (p.data.size() == p.a.len), "WLAST/AWLEN mismatch");
                log("W", p.a.id, p.a.addr, p.data.size(), w.read().last, 0, w.read().data, w.read().strb);
                p.data.push_back(w.read());
                if (p.data.size() == unsigned(p.a.len) + 1) {
                    BChannel expected; expected.id = p.a.id; expected.user = p.a.user;
                    expected.resp = status(p.a);

                    if (!expected.resp) {
                        for (unsigned n = 0; n < p.data.size(); ++n) {
                            uint64_t address = p.a.addr + (uint64_t(n) << p.a.size);
                            uint64_t bus_base = address & ~(uint64_t(AXI_DATA_BYTES) - 1);
                            for (unsigned j = 0; j < AXI_DATA_BYTES; ++j)
                                if (p.data[n].strb[j]) reference_[bus_base + j] = p.data[n].data[j];
                        }
                    }
                    expected_b_[expected.id].push_back(expected);
                    pending_w_.pop_front();
                }
            }
            if (arv.read() && arr.read()) {
                ++ar_count; const auto a = ra.read(); log("AR", a.id, a.addr, 0, false, 0);
                for (unsigned n = 0; n <= a.len; ++n) {
                    RChannel expected; expected.id = a.id; expected.user = a.user;
                    expected.last = n == a.len; expected.resp = status(a);
                    uint64_t address = a.addr + (uint64_t(n) << a.size);
                    if (!expected.resp) for (unsigned j = 0; j < (1u << a.size); ++j)
                        expected.data[(address + j) % AXI_DATA_BYTES] = reference_[address + j];
                    if (corrupt_expected) { expected.data[address % AXI_DATA_BYTES] ^= 1; corrupt_expected = false; }
                    expected_r_[a.id].push_back({expected, address, n});
                }
            }
            if (bv.read() && br.read()) {
                ++b_count; const auto got = b.read(); auto& q = expected_b_[got.id];
                log("B", got.id, 0, 0, true, got.resp);
                check(!q.empty(), "unexpected/duplicate/early B");
                if (!q.empty()) {
                    check(got.id == q.front().id && got.resp == q.front().resp && got.user == q.front().user,
                          "B ID/RESP/USER mismatch"); q.pop_front();
                }
            }
            if (rv.read() && rr.read()) {
                ++r_count; const auto got = r.read(); auto& q = expected_r_[got.id];
                log("R", got.id, q.empty() ? 0 : q.front().address,
                    q.empty() ? 0 : q.front().beat, got.last, got.resp, got.data);
                check(!q.empty(), "unexpected/duplicate R");
                if (!q.empty()) {
                    const auto expected = q.front().value; q.pop_front();
                    check(got.id == expected.id && got.resp == expected.resp && got.user == expected.user &&
                          got.last == expected.last, "R ID/RESP/USER/LAST mismatch");
                    for (unsigned j = 0; j < AXI_DATA_BYTES; ++j)
                        if (got.data[j] != expected.data[j]) {
                            check(false, "R data mismatch id=" + std::to_string(got.id) + " lane=" + std::to_string(j));
                            break;
                        }
                }
            }
        }
    }
    // 在下降沿驱动响应背压，确保下一上升沿采样值稳定。
    void ready_driver() {
        br.write(false); rr.write(false);
        while (true) {
            wait();
            if (!rst.read()) continue;
            br.write(!hold_b_ && (!random_ready_ || ready_rng_() % 4 != 0));
            rr.write(!hold_r_ && (!random_ready_ || ready_rng_() % 3 != 0));
            if (hold_b_) --hold_b_;
            if (hold_r_) --hold_r_;
        }
    }
    AxChannel address(uint16_t id, uint64_t offset, unsigned beats, unsigned size = AXI_SIZE_CODE) {
        AxChannel a; a.id = id; a.addr = BASE + offset; a.len = beats - 1;
        a.size = size; a.qos = id % rp_count; a.user = 0xA000 | id;
        return a;
    }
    // 地址保持有效直至监视器确认实际握手，避免将发送意图计为已接受请求。
    void send_address(const AxChannel& a, bool read) {
        uint64_t before = read ? ar_count : aw_count;
        if (read) { ra.write(a); arv.write(true); } else { aw.write(a); av.write(true); }
        until([&] { return (read ? ar_count : aw_count) > before; }, "AXI address timeout");
        (read ? arv : av).write(false);
    }
    void send_data(const AxChannel& a, unsigned salt, unsigned mask = 0, bool gaps = false) {
        for (unsigned n = 0; n <= a.len; ++n) {
            if (gaps) cycles(stimulus_rng_() % 4);
            WChannel beat; beat.last = n == a.len; beat.user = 0x5100 | n;
            uint64_t addr = a.addr + (uint64_t(n) << a.size);
            unsigned lane = addr % AXI_DATA_BYTES;
            for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
                beat.data[j] = uint8_t(salt + n * 37 + j * 13 + (j >> 3));
                beat.strb[j] = j >= lane && j < lane + (1u << a.size) &&
                    (mask == 0 || (mask == 1 && j % 3 != 1));
            }
            auto before = w_count; w.write(beat); wv.write(true);
            until([&] { return w_count > before; }, "AXI W timeout");
            wv.write(false);
        }
    }
    void drain() {
        until([&] {
            if (!pending_w_.empty()) return false;
            for (auto& p : expected_b_) if (!p.second.empty()) return false;
            for (auto& p : expected_r_) if (!p.second.empty()) return false;
            return b_count == aw_count;
        }, "AXI response timeout");
    }
    // 等待写响应完成后发起读请求，建立明确的写后读依赖。
    void write_read(AxChannel a, unsigned salt, unsigned mask = 0, bool gaps = false) {
        send_address(a, false); send_data(a, salt, mask, gaps); drain();
        send_address(a, true); drain();
    }
    template<class Body> void run_case(unsigned id, const char* title, Body body) {
        testcase.write(id); tick(); unsigned before = failures;
        auto start = sc_time_stamp(); body(); drain(); cycles(4); ++cases;
        std::cout << (before == failures ? "[PASS] " : "[FAIL] ") << "TC" << id << " " << title
                  << " (" << start << " .. " << sc_time_stamp() << ")\n";
    }
    void stimulus() {
        try {
            rst.write(false); av.write(false); arv.write(false); wv.write(false);
            until([&] { return state.read() == unsigned(LinkState::Active); }, "link training timeout");
            cycles(2); rst.write(true); cycles(100);
            random_ready_ = stress;
            run_case(1, "zero-initialized read", [&] { send_address(address(1, 0, 1), true); });
            run_case(2, "single-beat write/read", [&] { write_read(address(2, 0x1000, 1), 0x31); });
            run_case(3, "burst 2/4/16 and full-width 4KB limit", [&] {
                for (unsigned count : {2u, 4u, 16u, 4096u / AXI_DATA_BYTES})
                    write_read(address(3, 0x2000, count), count);
            });
            run_case(4, "masked and zero-strobe overwrite", [&] {
                auto a = address(4, 0x3000, 4);
                write_read(a, 0x11); write_read(a, 0x89, 1); write_read(a, 0xE1, 2);
            });
            run_case(5, "narrow 4-byte burst with lane rotation", [&] {
                write_read(address(5, 0x4004, 16, 2), 0x53, 1, true);
                send_address(address(5, 0x4000, 4), true);
            });
            run_case(6, "maximum AXI 256-beat burst, exceeds credit window", [&] {
                write_read(address(6, 0x5000, 256, 2), 0xAB);
            });
            run_case(7, "burst ends exactly at 4KB", [&] {
                write_read(address(7, 0x8000 - 4 * AXI_DATA_BYTES, 4), 0xD1);
            });
            run_case(8, "out-of-range DECERR, no alias to valid window", [&] {
                write_read(address(8, MEM_SIZE, 4), 0x22);

                auto high = address(8, 0x1000, 1); high.addr += 1ULL << 32;
                write_read(high, 0xF2);
                send_address(address(2, 0x1000, 1), true);
            });
            run_case(9, "multiple AW, mixed outstanding reads/writes, multiple RP", [&] {
                std::vector<AxChannel> jobs;
                for (unsigned i = 0; i < 8; ++i) {
                    jobs.push_back(address(32 + i, 0x9000 + i * 256, 2));
                    send_address(jobs.back(), false);
                }

                for (unsigned i = 0; i < 8; ++i) {
                    send_address(address(64 + i, 0, 2), true);
                    send_data(jobs[i], 0x60 + i, 0, true);
                }
                drain();
                for (auto a : jobs) send_address(a, true);
            });
            run_case(10, "same-ID read/write response ordering", [&] {
                for (unsigned i = 0; i < 6; ++i) {
                    auto a = address(100, 0xB000 + i * 256, 2);
                    send_address(a, false); send_data(a, 11 + i);
                }
                drain();
                for (unsigned i = 0; i < 6; ++i) send_address(address(100, 0xB000 + i * 256, 2), true);
            });
            run_case(11, "long B/R backpressure and credit recovery", [&] {
                hold_b_ = 800; hold_r_ = 1800; random_ready_ = true;
                const unsigned count = 4096 / AXI_DATA_BYTES;
                for (unsigned i = 0; i < 4; ++i) {

                    auto a = address(120 + i * rp_count, 0xC000 + i * 4096, count);
                    send_address(a, false); send_data(a, 0x40 + i);
                }
                drain();
                hold_r_ = 3000;
                for (unsigned i = 0; i < 4; ++i)
                    send_address(address(120 + i * rp_count, 0xC000 + i * 4096, count), true);
                drain(); random_ready_ = stress;
            });
            run_case(12, "fixed-seed random legal bursts and strobes", [&] {
                for (unsigned i = 0; i < 24; ++i) {
                    unsigned beats = 1 + stimulus_rng_() % 16;
                    unsigned page = 16 + stimulus_rng_() % 8;
                    write_read(address(200 + i, page * 4096, beats), stimulus_rng_(), i % 3, true);
                }
            });
            until([&] { return target.idle() && !soc_tx.num_available() && !soc_rx.num_available() &&
                               !mem_tx.num_available() && !mem_rx.num_available() &&
                               stats.forward.ack_count >= stats.forward.tx_new_flits &&
                               stats.reverse.ack_count >= stats.reverse.tx_new_flits; }, "link drain timeout");
            // 排空后继续观察静默期，以发现迟到或重复的响应。
            cycles(1000);
            check(bridge.order_violations() == 0, "AXI RP ordering violations");
            check(target.reads == ar_count && target.writes == aw_count && target.write_beats == w_count &&
                  target.read_beats == r_count, "Target/AXI accounting mismatch");
            check(memory.completed == aw_count + ar_count, "memory completed count mismatch");
            check(b_stalls > 0 && r_stalls > 0, "backpressure coverage missing");
            check(target.response_credit_stalls > 0, "response credit exhaustion not exercised");

            if (stress && AXI_DATA_WIDTH != 256)
                check(target.tx_spanning > 0, "response cross-flit coverage missing");
            if (replay) check(stats.forward.crc_fail_count + stats.reverse.crc_fail_count > 0 &&
                              stats.forward.tx_replay_flits + stats.reverse.tx_replay_flits > 0,
                              "requested CRC/replay coverage missing");
            else check(stats.forward.crc_fail_count + stats.reverse.crc_fail_count == 0, "unexpected CRC error");
            std::cout << "AXI: AW=" << aw_count << " W=" << w_count << " B=" << b_count
                      << " AR=" << ar_count << " R=" << r_count << "\n"
                      << "TARGET: reads=" << target.reads << " writes=" << target.writes
                      << " max_outstanding=" << target.max_outstanding
                      << " credit_stalls=" << target.response_credit_stalls
                      << " rx_spanning=" << target.rx_spanning << " tx_spanning=" << target.tx_spanning << "\n"
                      << "LINK: forward=" << stats.forward.tx_new_flits << " reverse=" << stats.reverse.tx_new_flits
                      << " crc_fail=" << stats.forward.crc_fail_count + stats.reverse.crc_fail_count
                      << " replay=" << stats.forward.tx_replay_flits + stats.reverse.tx_replay_flits << "\n"
                      << "FULL_LINK: " << cases << " cases / " << failures << " errors\n";
            done = true; csv.flush(); sc_stop();
        } catch (const std::exception& e) {
            check(false, e.what()); csv.flush(); done = true; sc_stop();
        }
    }
};

// 选择位宽对应的可执行文件及运行场景，使用独立产物名称并设置全局超时。
int sc_main(int argc, char** argv) {
    unsigned rp = 1; bool stress = false, replay = false, negative = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stress") stress = true;
        else if (arg == "--replay") replay = true;
        else if (arg == "--negative-scoreboard") negative = true;
        else if (arg == "--rp" && i + 1 < argc) rp = std::stoul(argv[++i]);
        else { std::cerr << "usage: --rp 1..4 --stress --replay --negative-scoreboard\n"; return 2; }
    }
    if (rp < 1 || rp > 4) return 2;
    sc_set_time_resolution(1, SC_FS);
    g_aou_verbose = false;
    Config cfg = make_aou_ucie_config();
    cfg.awgn_sigma = cfg.jitter_sigma_ui = cfg.isi_h1 = cfg.isi_h2 = 0;
    cfg.lane_skew_max_ui = 0; cfg.extra_flit_error_rate = replay ? 0.02 : 0.0;
    cfg.retry_buffer_size = stress ? 8 : 64;
    cfg.feedback_ui = stress ? 256 : 32;
    require_valid_config(cfg);
    std::string prefix = "sim/full_link_" + std::to_string(AXI_DATA_WIDTH) + "_rp" + std::to_string(rp)
        + (stress ? "_stress" : "") + (replay ? "_replay" : "") + (negative ? "_negative" : "");
    FullLinkTb tb("tb", cfg, rp, stress, replay, negative, prefix);
    auto* vcd = sc_create_vcd_trace_file(prefix.c_str());
    vcd->set_time_unit(1, SC_FS); tb.trace(vcd);
    std::cout << "CONFIG: width=" << AXI_DATA_WIDTH << " RP=" << rp << " stress=" << stress
              << " replay=" << replay << " VCD=" << prefix << ".vcd\n";
    sc_start(2, SC_MS);
    sc_close_vcd_trace_file(vcd);
    if (!tb.done) { std::cerr << "[FAIL] global simulation watchdog\n"; return 1; }
    std::cout << (!tb.failures ? "ALL FULL-LINK TESTS PASSED\n" : "FULL-LINK TESTS FAILED\n");
    return tb.failures ? 1 : 0;
}
