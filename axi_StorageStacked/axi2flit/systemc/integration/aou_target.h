/**
 * 存储侧响应端。解包入站请求、组装写突发、提交内存事务并打包返回数据与状态。
 * 每个写请求槽预留完整突发空间，写数据转入该空间后才能归还接收credit。
 */
#pragma once
#include "aou_stream_decoder.h"
#include "ucie_aou_adapter.h"
#include "msg_decoder.h"
#include "credit_manager.h"
#include "simple_mem_if.h"
#include "axi_contract.h"
#include <deque>

SC_MODULE(AouTarget) {
    sc_in<bool> clk, rst_n;
    sc_fifo_in<FdiFlit> link_rx;
    sc_fifo_out<FdiFlit> link_tx;
    sc_fifo_out<SimpleMemRequest> mem_req;
    sc_fifo_in<SimpleMemResponse> mem_rsp;
    // 每个资源平面分别容纳四笔读请求、四笔写请求及 64 粒度的待组装写数据。
    static constexpr unsigned REQUEST_SLOTS = 4, WDATA_GRANULES = 64;
    static constexpr unsigned MAX_OUTSTANDING = 8;
    uint64_t reads = 0, writes = 0, read_beats = 0, write_beats = 0;
    uint64_t rx_flits = 0, tx_flits = 0, rx_spanning = 0, tx_spanning = 0;
    uint64_t response_credit_stalls = 0, memory_stalls = 0, tx_fifo_stalls = 0;
    unsigned max_outstanding = 0, max_wdata_granules = 0;

    SC_HAS_PROCESS(AouTarget);
    AouTarget(sc_module_name name, const Config& cfg, unsigned rp_count = 1)
        : sc_module(name), rp_count_(rp_count), credits_(rp_count, capacity(rp_count)) {
        require_aou_ucie_config(cfg);
        SC_THREAD(run);
        sensitive << clk.pos();
    }

    bool idle() const {
        if (outstanding_ || spill_offset_ || decoder_.carry_active() ||
            credits_.has_pending_returns()) return false;
        for (unsigned rp = 0; rp < rp_count_; ++rp)
            if (!writes_[rp].empty() || !reads_[rp].empty() || !data_[rp].empty()) return false;
        return true;
    }

private:
    struct WriteJob { SimpleMemRequest req; };
    struct Reply { SimpleMemResponse rsp; unsigned beat = 0; };
    struct Ticket { bool write; uint8_t rp; uint16_t id; unsigned beats; };
    unsigned rp_count_, outstanding_ = 0, next_submit_ = 0, next_reply_ = 0;
    CreditManager credits_;
    AouStreamDecoder decoder_;
    std::array<std::deque<WriteJob>, MAX_RESOURCE_PLANES> writes_;
    std::array<std::deque<SimpleMemRequest>, MAX_RESOURCE_PLANES> reads_;
    std::array<std::deque<AouMessage>, MAX_RESOURCE_PLANES> data_;
    std::array<unsigned, MAX_RESOURCE_PLANES> data_granules_{};
    std::array<std::deque<Reply>, MAX_RESOURCE_PLANES> read_replies_, write_replies_;
    std::deque<Ticket> tickets_;
    AouMessage spill_;
    int spill_offset_ = 0;
    bool spill_completes_ = false;
    uint64_t trace_id_ = 0;

    static CreditMatrix capacity(unsigned count) {
        if (count < 1 || count > MAX_RESOURCE_PLANES)
            throw std::invalid_argument("Target RP count must be 1..4");
        CreditMatrix c{};
        for (unsigned rp = 0; rp < count; ++rp) {
            c[rp][credit_kind_index(CreditKind::WriteReq)] = REQUEST_SLOTS * WREQ_GRANULES;
            c[rp][credit_kind_index(CreditKind::ReadReq)] = REQUEST_SLOTS * RREQ_GRANULES;
            c[rp][credit_kind_index(CreditKind::WriteData)] = WDATA_GRANULES;
        }
        return c;
    }
    static void require(bool ok, const char* what) {
        if (!ok) throw std::runtime_error(what);
    }
    void release(uint8_t rp, CreditKind kind, unsigned n) {
        credits_.return_rx_credit({rp, kind, n});
    }
    // 每拍取一帧；先验证完整消息结构，再更新credit并分流请求和写数据。
    void receive() {
        FdiFlit fdi;
        if (!link_rx.nb_read(fdi)) return;
        require(fdi.payload.size() == 250 && fdi.valid_bytes == 250, "Target FDI size");
        AouWireFlit bytes;
        std::copy(fdi.payload.begin(), fdi.payload.end(), bytes.begin());
        auto flit = deserialize_aou(bytes);
        require(flit.fdid == 0, "Target unsupported FDId");
        bool had_carry = decoder_.carry_active();
        const auto parsed = decoder_.consume(flit);
        ++rx_flits;
        if (had_carry) ++rx_spanning;
        decode_header_credits(flit.msg_credit, rp_count_,
                              [&](const CreditUpdate& c) { credits_.add_tx_credit(c); });
        for (const auto& m : parsed.messages) {
            if (m.type == MsgType::Misc) {
                CreditMatrix grants{};
                require(decode_crdt_grant_message(m, rp_count_, grants), "Target unexpected Misc");
                for (unsigned rp = 0; rp < rp_count_; ++rp)
                    for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k)
                        credits_.add_tx_credit({uint8_t(rp), CreditKind(k), grants[rp][k]});
                continue;
            }
            require(m.rp < rp_count_, "Target invalid RP");
            if (m.type == MsgType::WriteReq || m.type == MsgType::ReadReq) {
                SimpleMemRequest req;
                req.rp = m.rp; req.write = m.type == MsgType::WriteReq;
                require(MsgDecoder::decode_req(m, req.address), "Target request decode");
                require(!axi_request_error(req.address), "Target invalid AXI request");
                if (req.write) {
                    require(writes_[m.rp].size() < REQUEST_SLOTS, "Target WREQ capacity exceeded");
                    req.write_beats.reserve(req.beats());
                    writes_[m.rp].push_back({std::move(req)}); ++writes;
                } else {
                    require(reads_[m.rp].size() < REQUEST_SLOTS, "Target RREQ capacity exceeded");
                    reads_[m.rp].push_back(std::move(req)); ++reads;
                }
            } else if (m.type == MsgType::WriteData || m.type == MsgType::WriteDataFull) {
                auto& used = data_granules_[m.rp];
                used += m.granules;
                require(used <= WDATA_GRANULES, "Target WDATA capacity exceeded");
                max_wdata_granules = std::max(max_wdata_granules, used);
                data_[m.rp].push_back(m); ++write_beats;
            } else require(false, "Target received a response instead of a request");
        }
    }
    // 每个资源平面每拍组装一拍写数据，按写请求接收顺序配对。
    // 写数据进入已预留的突发空间后归还credit，使长突发可分批通过接收窗口。
    void assemble() {
        for (unsigned rp = 0; rp < rp_count_; ++rp) {

            for (auto& job : writes_[rp]) {
                if (job.req.write_beats.size() == job.req.beats()) continue;
                if (data_[rp].empty()) break;
                const auto m = data_[rp].front();
                WChannel w;
                require(MsgDecoder::decode_write_data(m, w), "Target WDATA decode");
                SimpleMemWriteBeat beat;
                std::copy_n(w.data, AXI_DATA_BYTES, beat.data.begin());
                std::copy_n(w.strb, AXI_DATA_BYTES, beat.strobe.begin());
                beat.user = w.user;
                job.req.write_beats.push_back(beat);
                data_[rp].pop_front(); data_granules_[rp] -= m.granules;
                release(rp, CreditKind::WriteData, m.granules);
                break;
            }
        }
    }
    // 在资源平面和读写方向间轮询，每拍最多提交一笔完整请求。
    // FIFO 接受后释放请求接收槽，未完成事务槽保留至响应序列化结束。
    void submit() {
        for (unsigned n = 0; n < rp_count_ * 2; ++n) {
            unsigned slot = (next_submit_ + n) % (rp_count_ * 2);
            unsigned rp = slot / 2;
            bool wr = slot % 2;
            if (wr ? (writes_[rp].empty() || writes_[rp].front().req.write_beats.size() !=
                      writes_[rp].front().req.beats()) : reads_[rp].empty()) continue;
            if (outstanding_ == MAX_OUTSTANDING || mem_req.num_free() == 0) {
                ++memory_stalls; return;
            }
            const auto& req = wr ? writes_[rp].front().req : reads_[rp].front();
            require(mem_req.nb_write(req), "Target memory reservation failed");
            tickets_.push_back({wr, uint8_t(rp), req.address.id, req.beats()});
            ++outstanding_; max_outstanding = std::max(max_outstanding, outstanding_);
            if (wr) writes_[rp].pop_front(); else reads_[rp].pop_front();
            release(rp, wr ? CreditKind::WriteReq : CreditKind::ReadReq,
                    wr ? WREQ_GRANULES : RREQ_GRANULES);
            next_submit_ = (slot + 1) % (rp_count_ * 2);
            return;
        }
    }
    // 用方向、资源平面和 ID 匹配最早待完成请求；后端须保持同组完成顺序。
    void collect() {
        SimpleMemResponse rsp;
        if (!mem_rsp.nb_read(rsp)) return;
        require(rsp.rp < rp_count_, "Target response RP");

        auto it = std::find_if(tickets_.begin(), tickets_.end(), [&](const Ticket& t) {
            return t.write == rsp.write && t.rp == rsp.rp && t.id == rsp.id;
        });
        require(it != tickets_.end(), "Target unmatched memory response");
        require(rsp.read_beats.size() == (rsp.write ? 0 : it->beats), "Target response length");
        tickets_.erase(it);
        (rsp.write ? write_replies_ : read_replies_)[rsp.rp].push_back({std::move(rsp), 0});
    }
    // 优先写响应，再轮询各资源平面的读响应；发送前扣除对端接收credit。
    bool next_message(AouMessage& msg, bool& completes) {
        completes = false;
        for (bool wr : {true, false}) {
            for (unsigned n = 0; n < rp_count_; ++n) {
                unsigned rp = (next_reply_ + n) % rp_count_;
                auto& q = (wr ? write_replies_ : read_replies_)[rp];
                if (q.empty()) continue;
                auto& p = q.front();
                if (wr) {
                    BChannel b; b.id = p.rsp.id; b.user = p.rsp.user; b.resp = p.rsp.resp;
                    msg = MsgBuilder::build_write_resp(b, rp);
                } else {
                    RChannel r; const auto& b = p.rsp.read_beats[p.beat];
                    r.id = p.rsp.id; r.user = b.user; r.resp = b.resp;
                    r.last = p.beat + 1 == p.rsp.read_beats.size();
                    std::copy(b.data.begin(), b.data.end(), r.data);
                    msg = MsgBuilder::build_read_data(r, rp);
                }
                if (!credits_.can_consume(rp, msg.type, msg.granules)) {
                    ++response_credit_stalls; continue;
                }
                credits_.consume(rp, msg.type, msg.granules);
                if (!wr) ++read_beats;
                if (wr || ++p.beat == p.rsp.read_beats.size()) {
                    q.pop_front(); completes = true;
                }
                next_reply_ = (rp + 1) % rp_count_;
                return true;
            }
        }
        return false;
    }
    // 先续传上一帧尾部的消息，再装入新消息；credit可随业务帧或单独回传。
    // 末条响应跨帧时，只有全部片段写入发送 FIFO 后才能释放未完成事务槽。
    void transmit() {
        if (link_tx.num_free() == 0) { ++tx_fifo_stalls; return; }
        AouFlit f;
        if (spill_offset_) {
            spill_offset_ += f.pack_fragment(spill_, spill_offset_);
            if (spill_offset_ == spill_.granules) {
                spill_offset_ = 0;
                if (spill_completes_) --outstanding_;
            }
        }
        while (!spill_offset_ && f.remaining_granules()) {
            AouMessage m;
            bool completes = false;
            if (!next_message(m, completes)) break;
            int took = f.pack_fragment(m, 0);
            if (took < m.granules) {
                spill_ = m; spill_offset_ = took; spill_completes_ = completes; ++tx_spanning;
            } else if (completes) --outstanding_;
        }
        f.msg_credit = credits_.take_header_grant();
        if (!f.used_granules && !f.msg_credit) return;
        const auto bytes = serialize_aou(f);
        FdiFlit fdi;
        fdi.payload.assign(bytes.begin(), bytes.end()); fdi.valid_bytes = bytes.size();
        fdi.kind = BusinessKind::Response; fdi.vc = 0; fdi.transaction_id = trace_id_++;
        fdi.transaction_start = fdi.fdi_time = sc_time_stamp();
        require(link_tx.nb_write(fdi), "Target TX reservation failed");
        ++tx_flits;
    }
    // 启动复位释放后发布初始容量；运行期间的复位要求重建完整链路。
    void run() {
        while (!rst_n.read()) wait();
        credits_.publish_initial_capacity();
        while (true) {
            try {
                require(rst_n.read(), "Target hot reset requires complete link reconstruction");
                receive(); collect(); assemble(); submit(); transmit();
            } catch (const std::exception& e) {
                SC_REPORT_FATAL("AouTarget", e.what());
            }
            wait();
        }
    }
};
