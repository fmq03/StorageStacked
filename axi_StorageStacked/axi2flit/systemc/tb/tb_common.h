/**
 * 测试平台共用工具：独立帧扫描、链路节流、飞行队列、数据图样和简化响应器。
 */
#pragma once

#include "axi2flit.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <vector>

// ============================================================
//  FlitScanner：独立的 Flit → 消息提取器
// ============================================================
class FlitScanner {
public:
    using MsgSink = std::function<void(const AouMessage&)>;

    explicit FlitScanner(unsigned rp_count = DEFAULT_RESOURCE_PLANES)
        : rp_count_(rp_count) {}

    void reset() {
        carry_active_ = false;
        carry_have_ = 0;
        flits_ = granules_ = messages_ = spanning_ = errors_ = 0;
        business_granules_ = misc_granules_ = 0;
    }

    /**
     * @brief 扫描一个已经握手接收的 Flit，把其中完整的消息交给 sink
     *
     * 定界规则（与 aou_types.h 的注释一致）：
     *   MsgStart[i]=1 → granule i 是一条新消息的首粒度；
     *   消息总长度由首字节自描述，不能靠"两个 MsgStart 位之间的距离"推断
     *   —— 消息被截断到下一个 Flit 时，它后面根本没有第二个 MsgStart 位。
     */
    void scan(const AouFlit& flit, const MsgSink& sink) {
        ++flits_;
        // 独立扫描器也只看线上字段；不能用发送对象的辅助粒度帮助 DUT 通过测试。
        // 每段实际识别出的片段单独计数，空粒度即使非零也不参与统计。

        int g = 0;
        // ---- (a) 上一个 Flit 尾部截断的消息，续传部分必须落在 G0 ----
        if (carry_active_) {
            if ((flit.msg_start & 1ULL) != 0) {
                ++errors_;                 // 续传中却出现新消息起始，链路失步
                carry_active_ = false;
                carry_have_ = 0;
            } else {
                int need = carry_.granules - carry_have_;
                int take = std::min(need, GRANULE_COUNT);
                granules_ += take;
                std::copy_n(&flit.payload[0], take * GRANULE_BYTES,
                            carry_.data + carry_have_ * GRANULE_BYTES);
                carry_have_ += take;
                g = take;
                if (carry_have_ >= carry_.granules) {
                    carry_active_ = false;
                    carry_have_ = 0;
                    ++messages_;
                    if (carry_.type != MsgType::Misc)
                        business_granules_ += carry_.granules;
                    else
                        misc_granules_    += carry_.granules;
                    sink(carry_);
                }
            }
        }

        // ---- (b) 本 Flit 内新起的消息 ----
        while (g < GRANULE_COUNT) {
            if (((flit.msg_start >> g) & 1ULL) == 0) { ++g; continue; }
            uint8_t b0 = flit.payload[g * GRANULE_BYTES];
            int total = message_granules_from_header(b0);
            if (total <= 0) { ++errors_; break; }

            int take = std::min(total, GRANULE_COUNT - g);
            granules_ += take;
            AouMessage msg;
            msg.type     = msg_type_of(b0);
            msg.rp       = msg_rp_of(b0);
            msg.granules = total;
            msg.byte_len = total * GRANULE_BYTES;
            std::copy_n(&flit.payload[g * GRANULE_BYTES], take * GRANULE_BYTES, msg.data);

            if (take < total) {            // 被 Flit 尾部截断，记录续传状态
                carry_ = msg;
                carry_have_ = take;
                carry_active_ = true;
                ++spanning_;
                break;                     // 截断只可能发生在 Flit 末尾
            }
            if (msg.type != MsgType::Misc && msg.rp >= rp_count_) ++errors_;
            ++messages_;
            if (msg.type != MsgType::Misc)
                business_granules_ += static_cast<unsigned long>(msg.granules);
            else
                misc_granules_     += static_cast<unsigned long>(msg.granules);
            sink(msg);
            g += total;
        }
    }

    bool          carry_active() const { return carry_active_; }
    unsigned long flits()        const { return flits_; }
    unsigned long granules()     const { return granules_; }
    unsigned long messages()     const { return messages_; }
    unsigned long spanning()     const { return spanning_; }   // 跨 Flit 的消息条数
    unsigned long errors()       const { return errors_; }
    // 业务消息（非 Misc）占用的粒度总数。用来把"载荷"与"纯 credit 开销"分开：
    // business_granules() / misc_granules() 都在"消息收全"的那一刻整条计入，
    // 而 granules() 是逐 Flit 累加的。跨 Flit 消息会让二者在窗口边界上短暂
    // 对不齐，所以 credit 粒度必须用 misc_granules() 直接取，不能拿
    // granules() - business_granules() 去减——那样会在边界上减出负数
    // （unsigned 下就是 18446744073709551610 这种天文数字）。
    unsigned long business_granules() const { return business_granules_; }
    unsigned long misc_granules()     const { return misc_granules_; }

private:
    unsigned      rp_count_;
    bool          carry_active_ = false;
    AouMessage    carry_{};
    int           carry_have_ = 0;
    unsigned long flits_ = 0, granules_ = 0, messages_ = 0, spanning_ = 0, errors_ = 0;
    unsigned long business_granules_ = 0;
    unsigned long misc_granules_     = 0;
};

// 把一串消息按 桥接消息 规则打成若干 Flit：装不下时截断到下一个 Flit 的 G0 续传。
// testbench 用它构造"消息跨 Flit"的入站激励，检验 DUT 解包侧的续传重组。
inline std::vector<AouFlit> pack_messages_into_flits(
        const std::vector<AouMessage>& msgs) {
    std::vector<AouFlit> flits;
    AouFlit cur; cur.clear();
    for (const AouMessage& m : msgs) {
        int done = 0;
        while (done < m.granules) {
            done += cur.pack_fragment(m, done);
            if (cur.remaining_granules() == 0) { flits.push_back(cur); cur.clear(); }
        }
    }
    if (cur.used_granules > 0) flits.push_back(cur);
    return flits;
}

// ============================================================
//  LinkPacer：把链路速率折算到时钟节奏上
// ============================================================
/**
 * 【LinkPacer 只负责"速率"，不负责"延迟"】
 *
 * 一个 256B Flit 在 UCIe x16 @24GT/s 上要占 256/48 = 5.333ns，而模型时钟是
 * 2ns —— 5.333 不是 2 的整数倍，不能简单地"每 3 拍收一包"（那等于 42.7GB/s，
 * 平白损失 11% 带宽，会把测量结果引到错误的结论上）。
 *
 * 这里用"时间预算累加"的办法得到正确的平均速率：
 *   每拍给预算加 2ns，预算攒够 5.333ns 就允许一次 Flit 握手并扣掉 5.333ns。
 * 长期平均即为 1 flit / 5.333ns。预算上限设为 (flit + clk)，保证空闲时最多
 * 只能"攒"出一包的余量，不会在长时间空闲后突然爆发一串超速握手。
 *
 * 【必须说清楚的一点】
 * 正因为空闲时预算是攒满的，**空载链路上的第一个 Flit 不承担任何序列化延迟**
 * ——它一到就能走。所以"每个 Flit 给往返延迟贡献 5.333ns"是错的：那是稳态
 * 满载时的排队间隔，不是空载单事务的延迟。空载延迟由下面的 FlitDelayLine
 * 显式建模，两者不能混为一谈。
 */
class LinkPacer {
public:
    explicit LinkPacer(double clk_ns = MODEL_CLK_PERIOD_NS,
                       double flit_ns = FLIT_PERIOD_NS)
        : clk_ns_(clk_ns), flit_ns_(flit_ns) {}

    void reset() { budget_ns_ = 0.0; }
    void tick() {
        budget_ns_ = std::min(budget_ns_ + clk_ns_, flit_ns_ + clk_ns_);
    }
    bool can_transfer() const { return budget_ns_ >= flit_ns_; }
    void consume()            { budget_ns_ -= flit_ns_; }

private:
    double clk_ns_;
    double flit_ns_;
    double budget_ns_ = 0.0;
};

// ============================================================
//  FlitDelayLine：链路的"飞行时间"
// ============================================================

// 按发送时刻加固定延迟入队，到期后允许取出；背压时保留队头。
template <typename T>
class FlitDelayLine {
public:
    // 配置只影响随后入队的帧；队列中的帧保留到达时刻，clear() 可清空队列。
    void configure(double delay_ns, double flit_ns = FLIT_PERIOD_NS) {
        delay_ns_ = delay_ns;
        // 物理在飞包数 = ceil(飞行时间 / 每包占用时间)，再留 2 格余量，
        // 用于容纳配置飞行时间内的帧及采样相位余量。
        const double in_flight = (flit_ns > 0.0) ? (delay_ns_ / flit_ns) : 0.0;
        capacity_ = 2u + static_cast<unsigned>(std::ceil(in_flight - 1e-9));
    }
    double   delay_ns() const { return delay_ns_; }
    unsigned capacity() const { return capacity_; }
    bool     has_room() const { return q_.size() < capacity_; }
    bool     empty()    const { return q_.empty(); }
    void     clear()          { q_.clear(); }

    void push(double now_ns, const T& v) { q_.push_back({now_ns + delay_ns_, v}); }

    bool pop_ready(double now_ns, T& out) {
        // 1e-9 容差：5.333ns 这类无限小数在浮点上不会正好相等
        if (q_.empty() || q_.front().first > now_ns + 1e-9) return false;
        out = q_.front().second;
        q_.pop_front();
        return true;
    }

private:
    double   delay_ns_  = 0.0;
    unsigned capacity_  = 2;
    std::deque<std::pair<double, T>> q_;
};

// 链路 Flit + 它被链路取走的时刻（延迟统计要用"上链路"那一刻，不是"到达"那一刻）
struct TimedFlit {
    AouFlit flit{};
    double  t_handshake = 0.0;
};

static constexpr double LINK_ONE_WAY_NS_FOR_TAT = LINK_TAT_NS / 2.0;

// ============================================================
//  统计工具
// ============================================================
struct LatencyStat {
    void add(double ns) {
        if (n == 0 || ns < min_ns) min_ns = ns;
        if (n == 0 || ns > max_ns) max_ns = ns;
        sum_ns += ns;
        ++n;
    }
    double avg() const { return n ? sum_ns / n : 0.0; }
    double min() const { return n ? min_ns : 0.0; }
    double max() const { return n ? max_ns : 0.0; }
    unsigned long count() const { return n; }

    double        min_ns = 0.0, max_ns = 0.0, sum_ns = 0.0;
    unsigned long n = 0;
};

// ============================================================
//  AXI 测试激励构造
// ============================================================

inline uint8_t wdata_pattern_byte(uint16_t id, unsigned beat, int idx) {
    const uint8_t seed = static_cast<uint8_t>((id * 37u + beat * 11u + 0xA5u) & 0xFFu);
    return static_cast<uint8_t>(seed ^ static_cast<uint8_t>(idx * 5u)
                                     ^ static_cast<uint8_t>(1u << (idx & 7)));
}
inline uint8_t rdata_pattern_byte(uint16_t id, unsigned beat, int idx) {
    const uint8_t seed = static_cast<uint8_t>((id * 31u + beat * 7u + 0x5Au) & 0xFFu);
    return static_cast<uint8_t>(seed ^ static_cast<uint8_t>(idx * 3u)
                                     ^ static_cast<uint8_t>(1u << (7 - (idx & 7))));
}

// 整条 beat 的填充与校验。校验函数返回第一个不一致的字节下标，全对返回 -1，
// 失败时能直接指出"错在第几个字节"，比一个 bool 好定位得多。
inline void fill_wdata_pattern(uint8_t* d, uint16_t id, unsigned beat) {
    for (int i = 0; i < AXI_DATA_BYTES; ++i) d[i] = wdata_pattern_byte(id, beat, i);
}
inline void fill_rdata_pattern(uint8_t* d, uint16_t id, unsigned beat) {
    for (int i = 0; i < AXI_DATA_BYTES; ++i) d[i] = rdata_pattern_byte(id, beat, i);
}
inline int check_rdata_pattern(const uint8_t* d, uint16_t id, unsigned beat) {
    for (int i = 0; i < AXI_DATA_BYTES; ++i)
        if (d[i] != rdata_pattern_byte(id, beat, i)) return i;
    return -1;
}

static constexpr uint64_t TB_ADDR_TAG = 0x0123'4567ULL;
inline uint64_t tb_addr(uint64_t low32) {
    return (TB_ADDR_TAG << 32) | (low32 & 0xFFFF'FFFFULL);
}

inline AxChannel make_ax(uint16_t id, uint64_t addr, uint8_t len, uint8_t qos = 0) {
    AxChannel ax;
    ax.id   = id & AXI_ID_MASK;
    ax.addr = addr;
    ax.len  = len;                 // AxLEN，实际 beat 数 = len + 1
    ax.size = AXI_SIZE_CODE;       // 与编译期数据位宽一致
    ax.burst = 1;                  // INCR，本模型只支持这一种
    ax.qos  = qos;
    return ax;
}

inline WChannel make_w(uint16_t id, unsigned beat, bool last, bool full_strobe = true) {
    WChannel w;
    fill_wdata_pattern(w.data, id, beat);
    std::fill(std::begin(w.strb), std::end(w.strb), full_strobe ? 0xFF : 0x00);
    if (!full_strobe) {
        // 制造一个"部分 strobe"beat，强制走带 WSTRB 的 WriteData 路径
        for (int i = 0; i < AXI_STRB_WIDTH; i += 2) w.strb[i] = 0xFF;
    }
    w.last = last;
    return w;
}

// ============================================================
//  RemoteAouModel：链路对端的 桥接消息 协议模型
// ============================================================

// 根据请求 ID 和拍号生成模式数据并核对写数据，不保存按地址访问的内存。
// 与节流器、延迟队列共同用于桥性能基线，不能代替全链路存储功能验证。
class RemoteAouModel {
public:
    explicit RemoteAouModel(unsigned rp_count = DEFAULT_RESOURCE_PLANES)
        : rp_count_(rp_count),
          scanner_(rp_count),
          credits_(rp_count, make_remote_capacity(rp_count)) {
        reset();
    }

    void reset() {
        scanner_.reset();
        credits_.reset();
        credits_.publish_initial_capacity();   // 分批公布本端接收容量
        for (auto& q : read_jobs_)  q.clear();
        for (auto& q : write_jobs_) q.clear();
        pending_resp_.clear();
        pending_credit_.clear();
        spill_active_ = false;
        spill_done_ = 0;
        now_ns_ = 0.0;
        rreq_seen_ = wreq_seen_ = wdata_beats_ = 0;
        rdata_sent_ = wresp_sent_ = 0;
        data_mismatch_ = proto_errors_ = req_field_errors_ = 0;
    }

    void set_delays(double proc_ns, double credit_release_ns) {
        proc_delay_ns_     = proc_ns;
        credit_release_ns_ = credit_release_ns;
    }
    double proc_delay_ns()     const { return proc_delay_ns_; }
    double credit_release_ns() const { return credit_release_ns_; }

    // -------- DUT → 远端方向：消费一个已到达的 Flit --------
    void consume_flit(double now_ns, const AouFlit& flit) {
        now_ns_ = now_ns;
        decode_header_credits(flit.msg_credit, rp_count_,
            [this](const CreditUpdate& u) { credits_.add_tx_credit(u); });
        scanner_.scan(flit, [this](const AouMessage& m) { on_message(m); });
    }

    // -------- 远端 → DUT 方向：生成下一个待发 Flit --------
    // 返回 false 表示本端此刻既没有数据也没有 credit 要发，链路应保持空闲。
    bool produce_flit(double now_ns, AouFlit& flit) {
        now_ns_ = now_ns;
        release_due_credits();       // 接收缓冲释放时延到点了才归还 credit
        flit.clear();

        // (a) 上一包尾部截断的消息，续传部分放在 G0（不置 MsgStart）
        if (spill_active_) {
            int took = flit.pack_fragment(spill_msg_, spill_done_);
            spill_done_ += took;
            if (spill_done_ >= spill_msg_.granules) {
                spill_active_ = false;
                spill_done_ = 0;
            }
        }

        // (b) 继续填新消息，直到 Flit 满或没有可发的响应
        while (!spill_active_ && flit.remaining_granules() > 0) {
            AouMessage msg;
            if (!next_response(msg)) break;
            int took = flit.pack_fragment(msg, 0);
            if (took <= 0) break;
            if (took < msg.granules) {          // 截断，剩余部分下包续传
                spill_msg_ = msg;
                spill_done_ = took;
                spill_active_ = true;
            }
            if (msg.type == MsgType::ReadData) ++rdata_sent_; else ++wresp_sent_;
        }

        // (c) 协议头捎带 credit。数据流空闲时这就是唯一的 credit 回传途径，
        //     此时发出的是"只有 header 有效"的空 Flit —— 反向链路本来就闲着，
        //     本测试对端通过这种帧在无业务时归还credit。
        flit.msg_credit = credits_.take_header_grant();
        return flit.used_granules > 0 || flit.msg_credit != 0;
    }

    // -------- 统计 --------
    unsigned long rreq_seen()      const { return rreq_seen_; }
    unsigned long wreq_seen()      const { return wreq_seen_; }
    unsigned long wdata_beats()    const { return wdata_beats_; }
    unsigned long rdata_sent()     const { return rdata_sent_; }
    unsigned long wresp_sent()     const { return wresp_sent_; }
    unsigned long data_mismatch()  const { return data_mismatch_; }
    unsigned long proto_errors()   const { return proto_errors_ + scanner_.errors(); }
    unsigned long req_field_errors() const { return req_field_errors_; }
    const FlitScanner& scanner()   const { return scanner_; }

private:
    // ready_at：这个请求最早可以开始产生响应的时刻（= 收到时刻 + 存储访问时延）
    struct ReadJob  { uint16_t id; uint16_t user; uint8_t rp; unsigned total; unsigned done;
                      double ready_at; };
    struct WriteJob { uint16_t id; uint16_t user; uint8_t rp; unsigned total; unsigned done;
                      double ready_at; };

    // 远端的接收容量：它只收 WREQ / RREQ / WDATA 三类，同样按 credit 环路反推。
    static CreditMatrix make_remote_capacity(unsigned rp_count) {
        CreditMatrix cap{};
        for (auto& per_rp : cap) per_rp.fill(0);
        // 请求类消息的到达率上限是 AXI 的 1 条/拍，不是链路带宽，按环路拍数定容量
        const unsigned req_msgs = CREDIT_LOOP_CYCLES + FIFO_MARGIN_ENTRIES;
        // 写数据受链路灌入速率约束，按"一个环路时间内链路能灌进多少条"定容量。
        // 用带 WSTRB 的 WriteData 粒度数（更大的那个）定容量，保证两种写数据
        // 消息都装得下。
        const unsigned wdata_msgs =
            messages_in_flight(CFG_WDATA_GRANULES) + FIFO_MARGIN_ENTRIES;
        for (unsigned rp = 0; rp < rp_count; ++rp) {
            cap[rp][credit_kind_index(CreditKind::WriteReq)]  = req_msgs * WREQ_GRANULES;
            cap[rp][credit_kind_index(CreditKind::ReadReq)]   = req_msgs * RREQ_GRANULES;
            cap[rp][credit_kind_index(CreditKind::WriteData)] =
                wdata_msgs * CFG_WDATA_GRANULES;
        }
        return cap;
    }

    void on_message(const AouMessage& msg) {
        if (msg.type == MsgType::Misc) {
            CreditMatrix grants{};
            if (!decode_crdt_grant_message(msg, rp_count_, grants)) { ++proto_errors_; return; }
            for (unsigned rp = 0; rp < rp_count_; ++rp)
                for (unsigned k = 0; k < CREDIT_KIND_COUNT; ++k)
                    if (grants[rp][k])
                        credits_.add_tx_credit(CreditUpdate{
                            static_cast<uint8_t>(rp), static_cast<CreditKind>(k),
                            grants[rp][k]});
            return;
        }
        if (msg.rp >= rp_count_) { ++proto_errors_; return; }

        CreditReturn ret{msg.rp, msgtype_to_credit_kind(msg.type),
                         static_cast<unsigned>(msg.granules)};
        if (credit_release_ns_ <= 0.0) credits_.return_rx_credit(ret);
        else pending_credit_.push_back({now_ns_ + credit_release_ns_, ret});

        switch (msg.type) {
            case MsgType::ReadReq: {
                AxChannel ar;
                if (!MsgDecoder::decode_req(msg, ar)) { ++proto_errors_; return; }
                check_req_fields(ar);
                read_jobs_[msg.rp].push_back(
                    ReadJob{ar.id, ar.user, msg.rp, unsigned(ar.len) + 1u, 0,
                            now_ns_ + proc_delay_ns_});
                ++rreq_seen_;
                break;
            }
            case MsgType::WriteReq: {
                AxChannel aw;
                if (!MsgDecoder::decode_req(msg, aw)) { ++proto_errors_; return; }
                check_req_fields(aw);
                write_jobs_[msg.rp].push_back(
                    WriteJob{aw.id, aw.user, msg.rp, unsigned(aw.len) + 1u, 0,
                             now_ns_ + proc_delay_ns_});
                ++wreq_seen_;
                break;
            }
            case MsgType::WriteData:
            case MsgType::WriteDataFull: {
                WChannel w;
                if (!MsgDecoder::decode_write_data(msg, w)) { ++proto_errors_; return; }
                ++wdata_beats_;
                // AXI4 的 W 没有 ID，写数据严格按 AW 顺序归属；这正是 桥接消息
                // 接收侧必须靠 AWLEN 重建 WLAST 的原因。
                if (write_jobs_[msg.rp].empty()) { ++proto_errors_; return; }
                WriteJob& job = write_jobs_[msg.rp].front();
                for (int i = 0; i < AXI_DATA_BYTES; ++i) {
                    bool masked = (msg.type == MsgType::WriteData) && (w.strb[i] == 0);
                    if (!masked && w.data[i] != wdata_pattern_byte(job.id, job.done, i)) {
                        ++data_mismatch_; break;
                    }
                }
                if (++job.done >= job.total) {          // WLAST 重建点
                    BChannel b;
                    b.id = job.id; b.resp = 0; b.user = job.user;
                    // 写完成时刻不早于"请求就绪时刻"，也不早于"最后一拍数据 + 访问时延"
                    double at = std::max(job.ready_at, now_ns_ + proc_delay_ns_);
                    pending_resp_.push_back(
                        PendingResp{MsgBuilder::build_write_resp(b, job.rp), at});
                    write_jobs_[msg.rp].pop_front();
                }
                break;
            }
            default:
                ++proto_errors_;   // 远端不应收到 ReadData/WriteResp
                break;
        }
    }

    // 挑一条下一步要发的响应消息，同时检查并扣除 credit。
    // 优先发 WriteResp（1 granule，早发早释放对端缓冲），其次按 RP 轮转发 ReadData。
    bool next_response(AouMessage& out) {
        for (std::size_t i = 0; i < pending_resp_.size(); ++i) {
            PendingResp& p = pending_resp_[i];
            if (p.ready_at > now_ns_ + 1e-9) continue;     // 存储访问时延未到
            AouMessage& m = p.msg;
            if (!credits_.can_consume(m.rp, m.type, m.granules)) continue;
            credits_.consume(m.rp, m.type, m.granules);
            out = m;
            pending_resp_.erase(pending_resp_.begin() + i);
            return true;
        }
        for (unsigned checked = 0; checked < rp_count_; ++checked) {
            unsigned rp = (next_rp_ + checked) % rp_count_;
            if (read_jobs_[rp].empty()) continue;
            ReadJob& job = read_jobs_[rp].front();
            if (job.ready_at > now_ns_ + 1e-9) continue;   // 存储访问时延未到
            RChannel r;
            r.id = job.id;
            r.user = job.user;
            r.resp = 0;
            r.last = (job.done + 1 == job.total);
            fill_rdata_pattern(r.data, job.id, job.done);
            AouMessage m = MsgBuilder::build_read_data(r, static_cast<uint8_t>(rp));
            if (!credits_.can_consume(static_cast<uint8_t>(rp), m.type, m.granules)) continue;
            credits_.consume(static_cast<uint8_t>(rp), m.type, m.granules);
            if (++job.done >= job.total) read_jobs_[rp].pop_front();
            next_rp_ = (rp + 1) % rp_count_;
            out = m;
            return true;
        }
        return false;
    }

    // 到点的 credit 才真正进入待归还队列
    void release_due_credits() {
        while (!pending_credit_.empty() &&
               pending_credit_.front().first <= now_ns_ + 1e-9) {
            credits_.return_rx_credit(pending_credit_.front().second);
            pending_credit_.pop_front();
        }
    }

    /**
     * 请求字段自检。
     * 只查那些"TB 一侧确知应该是什么"的字段：
     *   - AxADDR 高 32 位必须是 TB_ADDR_TAG，任何 64bit 端序/半字交换都会破坏它；
     *   - AxSIZE 必须等于本次编译的数据位宽编码；
     *   - AxBURST 必须是 INCR（本模型只支持 INCR，解码器固定填 1，这里做冗余确认）。
     * ID / LEN / USER 由响应回路对账，不在这里重复。
     */
    void check_req_fields(const AxChannel& ax) {
        if ((ax.addr >> 32) != TB_ADDR_TAG)   ++req_field_errors_;
        if (ax.size != AXI_SIZE_CODE)         ++req_field_errors_;
        if (ax.burst != 1)                    ++req_field_errors_;
    }

    struct PendingResp { AouMessage msg; double ready_at; };

    unsigned      rp_count_;
    unsigned      next_rp_ = 0;
    FlitScanner   scanner_;
    CreditManager credits_;

    double proc_delay_ns_     = 0.0;
    double credit_release_ns_ = 0.0;
    double now_ns_            = 0.0;

    std::deque<ReadJob>      read_jobs_[MAX_RESOURCE_PLANES];
    std::deque<WriteJob>     write_jobs_[MAX_RESOURCE_PLANES];
    std::vector<PendingResp> pending_resp_;
    std::deque<std::pair<double, CreditReturn>> pending_credit_;

    bool       spill_active_ = false;
    AouMessage spill_msg_{};
    int        spill_done_ = 0;

    unsigned long rreq_seen_ = 0, wreq_seen_ = 0, wdata_beats_ = 0;
    unsigned long rdata_sent_ = 0, wresp_sent_ = 0;
    unsigned long data_mismatch_ = 0, proto_errors_ = 0;
    unsigned long req_field_errors_ = 0;
};
