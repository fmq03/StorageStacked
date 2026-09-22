#include "gem5_axi_agent.h"

#include <cstring>

namespace {

// 一笔在途事务的状态。
enum class Phase { Idle, WriteAw, WriteW, WriteB, ReadAr, ReadR, Done };

}  // namespace

// socket 线程与 SC_THREAD 之间的共享状态。所有跨线程访问都在锁内。
struct Gem5AxiAgent::Shared {
    std::mutex m;
    std::condition_variable cv_req;  // socket → SC：有新请求
    std::condition_variable cv_rsp;  // SC → socket：有响应可发

    std::deque<bridge::Gem5AxiRequest> requests;
    std::deque<std::vector<std::uint8_t>> responses;
    std::size_t depth = 4;

    bool eof = false;    // 对端正常关闭
    bool stop = false;   // 要求 socket 线程退出
    bool in_flight = false;  // SC 侧是否有未完成的事务
    std::string error;

    // 由 SC_THREAD 持有并在完成后交回 socket 线程
    std::vector<std::uint8_t> pending_read;
};

Gem5AxiAgent::Gem5AxiAgent(sc_module_name name, const std::string& socket_path,
                           std::size_t queue_depth)
    : sc_module(name), socket_path_(socket_path), shared_(new Shared()) {
    shared_->depth = queue_depth;
    // socket 线程必须在成员都初始化完之后再起，并且只捕获 this。
    socket_thread_ = std::thread([this] { socket_loop(); });
    SC_THREAD(axi_loop);
    sensitive << clk.pos();
}

Gem5AxiAgent::~Gem5AxiAgent() {
    {
        std::lock_guard<std::mutex> lk(shared_->m);
        shared_->stop = true;
    }
    shared_->cv_req.notify_all();
    shared_->cv_rsp.notify_all();
    if (socket_thread_.joinable()) socket_thread_.join();
}

bool Gem5AxiAgent::eof_seen() const {
    std::lock_guard<std::mutex> lk(shared_->m);
    return shared_->eof;
}

bool Gem5AxiAgent::idle() const {
    std::lock_guard<std::mutex> lk(shared_->m);
    return !shared_->in_flight && shared_->requests.empty();
}

const std::string& Gem5AxiAgent::last_error() const { return shared_->error; }

// ---------------------------------------------------------------------------
// socket 线程：阻塞 I/O。绝不触碰 SystemC 对象。
// ---------------------------------------------------------------------------
void Gem5AxiAgent::socket_loop() {
    bridge::Gem5AxiSocket sock;
    if (!sock.connect(socket_path_)) {
        std::lock_guard<std::mutex> lk(shared_->m);
        shared_->error = sock.last_error();
        shared_->eof = true;
        return;
    }

    while (true) {
        {
            std::lock_guard<std::mutex> lk(shared_->m);
            if (shared_->stop) break;
        }

        bridge::Gem5AxiRequest req;
        if (!sock.read_request(req)) {
            std::lock_guard<std::mutex> lk(shared_->m);
            if (sock.peer_closed()) {
                shared_->eof = true;  // gem5 正常退出
            } else {
                shared_->error = sock.last_error();
                shared_->eof = true;
            }
            shared_->cv_req.notify_all();
            break;
        }

        // 入队，等待 SC 侧取走（有界背压）
        {
            std::unique_lock<std::mutex> lk(shared_->m);
            shared_->cv_req.wait(lk, [&] {
                return shared_->stop || shared_->requests.size() < shared_->depth;
            });
            if (shared_->stop) break;
            shared_->requests.push_back(std::move(req));
            shared_->in_flight = true;
        }
        shared_->cv_req.notify_all();

        // 等 SC 侧完成并交回响应数据。**注意此处不持锁去做 I/O**。
        std::vector<std::uint8_t> read_data;
        {
            std::unique_lock<std::mutex> lk(shared_->m);
            shared_->cv_rsp.wait(lk, [&] {
                return shared_->stop || !shared_->responses.empty();
            });
            if (shared_->stop) break;
            read_data = std::move(shared_->responses.front());
            shared_->responses.pop_front();
        }

        if (!sock.send_response(read_data)) {
            std::lock_guard<std::mutex> lk(shared_->m);
            shared_->error = sock.last_error();
            shared_->eof = true;
            shared_->cv_req.notify_all();
            break;
        }
    }
    sock.close();
}

// ---------------------------------------------------------------------------
// SC_THREAD：时钟驱动，驱动 AXI 五通道。全程不阻塞。
// ---------------------------------------------------------------------------
void Gem5AxiAgent::axi_loop() {
    Phase phase = Phase::Idle;
    bridge::Gem5AxiRequest cur;
    unsigned beat = 0;
    std::vector<std::uint8_t> read_acc;

    auto publish = [&](std::vector<std::uint8_t> data) {
        std::lock_guard<std::mutex> lk(shared_->m);
        shared_->responses.push_back(std::move(data));
        shared_->in_flight = false;
        shared_->cv_rsp.notify_all();
    };

    // 把 gem5 的连续 payload 铺到 AXI 拍的通道上。
    // gem5 保证 burst 内每拍地址满足 addr % beat_bytes == 0，所以
    // lane + beat_bytes <= AXI_DATA_BYTES 恒成立。
    auto fill_write_channel = [&](WChannel& w, unsigned n) {
        const unsigned wb = cur.beat_bytes();
        const unsigned lane =
            static_cast<unsigned>((cur.addr + std::uint64_t(n) * wb) % AXI_DATA_BYTES);
        // WChannel 的 data/strb 是 C 数组，不是 std::array
        std::memset(w.data, 0, sizeof(w.data));
        std::memset(w.strb, 0, sizeof(w.strb));
        for (unsigned j = 0; j < wb; ++j) {
            w.data[lane + j] = cur.payload[std::uint64_t(n) * wb + j];
            // 协议不携带 WSTRB，只能按全字节有效处理。
            w.strb[lane + j] = 0xFF;
        }
        w.last = (n + 1 == cur.beats());
        w.user = 0;
        w.valid = true;
        w.ready = false;
    };

    auto drive_ax = [&](AxChannel& a) {
        a.addr = cur.addr;
        a.len = static_cast<std::uint8_t>(cur.length);
        a.size = static_cast<std::uint8_t>(cur.size);
        a.burst = 1;  // 只支持 INCR
        a.id = 0;     // 固定 ID：lock-step 下只有一笔在途，无需区分
        a.qos = 0;    // 固定映射到 RP0，天然满足 RpOrderGuard
        a.cache = 0;
        a.prot = 0;
        a.lock = 0;
        a.user = 0;
        a.valid = true;
        a.ready = false;
    };

    // 初值：全部拉低
    aw_valid.write(false);
    w_valid.write(false);
    ar_valid.write(false);
    b_ready.write(false);
    r_ready.write(false);
    {
        AxChannel a{};
        a.valid = false;
        a.ready = false;
        aw_ch.write(a);
        ar_ch.write(a);
    }
    {
        WChannel w{};
        w.valid = false;
        w.ready = false;
        w_ch.write(w);
    }

    while (true) {
        wait();  // clk.pos()

        if (phase == Phase::Idle) {
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(shared_->m);
                if (!shared_->requests.empty()) {
                    cur = std::move(shared_->requests.front());
                    shared_->requests.pop_front();
                    have = true;
                }
            }
            if (have) {
                read_acc.clear();
                beat = 0;
                if (cur.write) {
                    AxChannel a{};
                    drive_ax(a);
                    aw_ch.write(a);
                    aw_valid.write(true);
                    phase = Phase::WriteAw;
                } else {
                    AxChannel a{};
                    drive_ax(a);
                    ar_ch.write(a);
                    ar_valid.write(true);
                    phase = Phase::ReadAr;
                }
            }
            continue;
        }

        if (phase == Phase::WriteAw) {
            if (aw_ready.read()) {
                aw_valid.write(false);
                ++aw_count;
                WChannel w{};
                fill_write_channel(w, beat);
                w_ch.write(w);
                w_valid.write(true);
                phase = Phase::WriteW;
            }
            continue;
        }

        if (phase == Phase::WriteW) {
            if (w_ready.read()) {
                ++w_count;
                ++beat;
                if (beat == cur.beats()) {
                    w_valid.write(false);
                    b_ready.write(true);
                    phase = Phase::WriteB;
                } else {
                    WChannel w{};
                    fill_write_channel(w, beat);
                    w_ch.write(w);
                }
            }
            continue;
        }

        if (phase == Phase::WriteB) {
            if (b_valid.read()) {
                ++b_count;
                b_ready.write(false);
                publish({});  // 写响应：response_bytes 必须为 0
                phase = Phase::Idle;
            }
            continue;
        }

        if (phase == Phase::ReadAr) {
            if (ar_ready.read()) {
                ar_valid.write(false);
                ++ar_count;
                r_ready.write(true);
                phase = Phase::ReadR;
            }
            continue;
        }

        if (phase == Phase::ReadR) {
            if (r_valid.read()) {
                const RChannel r = r_ch.read();
                const unsigned wb = cur.beat_bytes();
                const unsigned lane = static_cast<unsigned>(
                    (cur.addr + std::uint64_t(beat) * wb) % AXI_DATA_BYTES);
                read_acc.resize(read_acc.size() + wb);
                const std::size_t base = read_acc.size() - wb;
                for (unsigned j = 0; j < wb; ++j) {
                    read_acc[base + j] = r.data[lane + j];
                }
                ++r_count;
                ++beat;
                if (r.last || beat == cur.beats()) {
                    ++r_txn_count;  // 一笔读事务完成（RLAST）
                    r_ready.write(false);
                    publish(std::move(read_acc));
                    read_acc.clear();
                    beat = 0;
                    phase = Phase::Idle;
                }
            }
            continue;
        }
    }
}
