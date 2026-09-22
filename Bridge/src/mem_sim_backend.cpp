// MemSimBackend 实现：把 AouTarget 交付的整笔 AXI 突发，转换成 mem_sim 的
// 逐事务读写，并把结果装配回整笔响应。
//
// 三条贯穿全文件的规则，都来自对 mem_sim 与 AouTarget 的实测勘察：
//
//  1. 错误必须在提交前自行判定。mem_sim 对越界是 **抛 std::out_of_range**
//     而不是返回错误码，也没有 lock / strobe 合法域的概念。若把越界请求递给
//     MemorySystem，异常会穿透 step() 并留下半更新状态，AouTarget 侧对应的
//     ticket 会永久悬挂。所以越界/lock/非法 strobe 一律在这里合成错误响应，
//     根本不进 mem_sim。
//
//  2. 响应必须按提交顺序交付。AouTarget::collect() 用 find_if 找第一个匹配
//     (write,rp,id) 的 ticket，而 mem_sim 会乱序完成不同 bank 的请求。乱序
//     交付的后果是：需要重构的读响应长度对不上会 SC_REPORT_FATAL，长度恰好
//     相同时则会静默把 A 的数据配上 B 的地址。这里用一个全局 FIFO 保证顺序
//     （比逐 (write,rp,id) 队列保守，代价是有队头阻塞）。
//
//  3. SC_THREAD 绝不允许阻塞。sc_fifo::read()/write() 在满/空时会 delta 等待，
//     让线程脱离时钟相位，之后每拍推进 mem_sim 的节奏就乱了。全程只用
//     nb_read/nb_write；压力模式下 FIFO 深度是 1，任何一处阻塞都会立刻死锁。
#include "mem_sim_backend.h"

#include "axi_contract.h"
#include "axi_if.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

constexpr unsigned kMaxBurstsInFlight = 8;  // 与 AouTarget::MAX_OUTSTANDING 对齐

// 由 AXI size 码得出的每拍字节数。
inline unsigned beat_width(std::uint8_t size_code) {
    return 1u << size_code;
}

}  // namespace

// 一笔待提交的 mem_sim 事务：它属于某个突发、某个内存 chunk。
struct MemSimBackend::Impl {
    struct Chunk {
        std::uint64_t tag = 0;
        std::uint64_t mem_addr = 0;   // 该 chunk 在 mem_sim 地址空间中的起点
        std::uint32_t span_lo = 0;    // 突发实际覆盖的区间 [lo, hi)
        std::uint32_t span_hi = 0;
        bool done = false;
        std::vector<std::uint8_t> data;  // 读回的数据
    };

    struct Burst {
        SimpleMemRequest req;
        bool write = false;
        unsigned beats = 0;
        unsigned width = 0;       // 每拍字节数 S
        std::uint64_t mem_base = 0;  // 突发起点在 mem_sim 地址空间中的位置
        std::uint64_t total = 0;  // 本次搬运的总字节数 T
        unsigned status = 0;      // 合成的错误码；0 表示无错
        bool synthesize_only = false;  // 预校验失败，不提交 mem_sim
        std::vector<Chunk> chunks;
        unsigned chunks_done = 0;
    };

    struct PendingTxn {
        std::shared_ptr<Burst> burst;
        unsigned chunk = 0;
        bridge::MemTxn txn;
    };

    std::uint64_t base = 0;
    std::size_t size = 0;
    unsigned ticks_per_clock = 8;
    std::unique_ptr<bridge::MemBackend> backend;

    std::deque<std::shared_ptr<Burst>> in_flight;  // 提交顺序，也是交付顺序
    std::deque<PendingTxn> submit_queue;
    std::unordered_map<std::uint64_t, std::pair<std::shared_ptr<Burst>, unsigned>>
        tag_index;
    std::uint64_t next_tag = 1;

    // 把一笔突发拆成若干对齐到 transaction_bytes 的内存 chunk。
    // 全程在 mem_sim 地址空间里算，避免 base 未对齐带来的边界问题。
    void build_chunks(Burst& b) {
        if (b.synthesize_only) return;
        const auto& a = b.req.address;
        const std::uint32_t tb = backend->transaction_bytes();
        const std::uint64_t lo = b.mem_base;
        const std::uint64_t hi = b.mem_base + b.total;

        std::uint64_t chunk_start = (lo / tb) * tb;
        while (chunk_start < hi) {
            Chunk c;
            c.mem_addr = chunk_start;
            c.span_lo = static_cast<std::uint32_t>(
                std::max<std::uint64_t>(lo, chunk_start) - chunk_start);
            c.span_hi = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(hi, chunk_start + tb) - chunk_start);
            c.tag = b.req.address.id;  // 占位，稍后统一分配
            b.chunks.push_back(std::move(c));
            chunk_start += tb;
        }
        (void)a;
    }

    // 为写请求构造某个 chunk 的 payload 与 byte_mask。
    // AXI 第 n 拍覆盖线性区间 [A+nS, A+nS+S)，其字节落在该拍的
    // [L, L+S) 通道上，L = (A+nS) % AXI_DATA_BYTES。
    void fill_write_chunk(const Burst& b, std::uint32_t chunk_index,
                          bridge::MemTxn& txn) const {
        const std::uint32_t tb = backend->transaction_bytes();
        const auto& a = b.req.address;
        const std::uint64_t burst_lo = b.mem_base;
        const Chunk& c = b.chunks[chunk_index];

        txn.payload.assign(tb, 0);
        txn.byte_mask.assign(tb, 0);
        txn.has_byte_mask = true;

        for (std::uint32_t off = c.span_lo; off < c.span_hi; ++off) {
            const std::uint64_t mem_addr = c.mem_addr + off;
            const std::uint64_t lin = mem_addr - burst_lo;  // 相对突发起点的线性偏移
            const unsigned n = static_cast<unsigned>(lin / b.width);
            const unsigned lane = static_cast<unsigned>(
                ((a.addr + static_cast<std::uint64_t>(n) * b.width) %
                 AXI_DATA_BYTES) +
                (lin % b.width));
            if (lane >= AXI_DATA_BYTES) continue;
            const auto& beat = b.req.write_beats[n];
            txn.payload[off] = beat.data[lane];
            txn.byte_mask[off] = beat.strobe[lane] ? 0xFF : 0x00;
        }
    }

    // 把一个 chunk 的读回数据按同一映射放回各拍的 lane 上。
    void scatter_read_chunk(const Burst& b, std::uint32_t chunk_index,
                            const std::vector<std::uint8_t>& data,
                            std::vector<SimpleMemReadBeat>& out) const {
        const auto& a = b.req.address;
        const Chunk& c = b.chunks[chunk_index];
        for (std::uint32_t off = c.span_lo; off < c.span_hi; ++off) {
            if (off >= data.size()) continue;
            const std::uint64_t mem_addr = c.mem_addr + off;
            const std::uint64_t lin = mem_addr - b.mem_base;
            if (lin >= b.total) continue;
            const unsigned n = static_cast<unsigned>(lin / b.width);
            const unsigned lane = static_cast<unsigned>(
                ((a.addr + static_cast<std::uint64_t>(n) * b.width) %
                 AXI_DATA_BYTES) +
                (lin % b.width));
            if (lane >= AXI_DATA_BYTES || n >= out.size()) continue;
            out[n].data[lane] = data[off];
            out[n].user = a.user;
            out[n].resp = static_cast<std::uint8_t>(b.status);
        }
    }
};

MemSimBackend::MemSimBackend(sc_module_name name, std::uint64_t base,
                             std::size_t size, sc_time access, sc_time per_beat)
    : sc_module(name), impl_(new Impl()) {
    (void)access;     // 时序由 mem_sim 的真实 DRAM 时序决定，参数仅为兼容签名
    (void)per_beat;
    if (size == 0 || base > UINT64_MAX - (size - 1)) {
        throw std::invalid_argument("MemSimBackend: 非法地址窗口");
    }
    impl_->base = base;
    impl_->size = size;

    bridge::MemBackend::Options opt;
    opt.standard = "hbm4";
    opt.channels = 1;
    // 一个 mem_sim transaction 覆盖一个 AXI 拍，突发切分与 lane 映射最简单。
    // 若 AXI_DATA_BYTES 超过 HBM 的 line_size(64)，退回到 64 并让一拍拆成
    // 多个 chunk——build_chunks/fill_write_chunk 已经按通用情形处理。
    opt.transaction_bytes =
        static_cast<int>(std::min<unsigned>(AXI_DATA_BYTES, 64));
    opt.retain_command_trace = false;
    opt.response_queue_capacity = 0;
    impl_->backend = std::make_unique<bridge::MemBackend>(opt);

    if (impl_->backend->window_bytes() < size) {
        throw std::invalid_argument(
            "MemSimBackend: 请求的地址窗口超过 mem_sim 的可寻址容量");
    }

    SC_THREAD(run);
    sensitive << clk.pos();
}

MemSimBackend::~MemSimBackend() = default;

bool MemSimBackend::idle() const {
    return impl_->in_flight.empty() && impl_->submit_queue.empty();
}

bool MemSimBackend::memory_quiescent() const {
    return impl_->backend->quiescent();
}

void MemSimBackend::finish_memory() { impl_->backend->finish(0); }

void MemSimBackend::run() {
    using bridge::MemResp;
    using bridge::MemTxn;

    // 复位：清空所有状态。AouTarget 不允许运行期热复位，这里也就只在启动
    // 阶段等待一次。
    while (!rst_n.read()) wait();

    Impl& s = *impl_;

    while (true) {
        // ---- 1. 推进 mem_sim 的时间轴 --------------------------------
        // SystemC 时钟周期 / 一个 mem_sim tick。2 ns / 250 ps = 8。
        s.backend->step(s.ticks_per_clock);

        // ---- 2. 收下 mem_sim 已完成的响应 ----------------------------
        MemResp r;
        while (s.backend->poll(r)) {
            auto it = s.tag_index.find(r.host_request_id);
            if (it == s.tag_index.end()) continue;
            const auto burst = it->second.first;
            const unsigned ci = it->second.second;
            auto& chunk = burst->chunks[ci];
            chunk.done = true;
            chunk.data = std::move(r.data);
            if (r.status != 0 && burst->status == 0) {
                // mem_sim 的状态里只有 Ok 与 EccCorrected 允许映射成 AXI OK；
                // UninitializedData 必须映射成 OK，否则"读未写过但零初始化的
                // 内存"会被判成错误，现有的零初始化用例会挂。
                if (r.status != 1 && r.status != 2) {
                    burst->status = 2;  // SLVERR
                }
            }
            ++burst->chunks_done;
            s.tag_index.erase(it);
        }

        // ---- 3. 提交待发事务（每次一拍，反压时原样重试）--------------
        if (!s.submit_queue.empty()) {
            auto& p = s.submit_queue.front();
            const int rc = s.backend->submit(p.txn);
            if (rc == 0) {
                ++submitted_txns;
                s.tag_index[p.txn.host_request_id] = {p.burst, p.chunk};
                s.submit_queue.pop_front();
            } else if (rc > 0) {
                ++retried_submits;  // 反压：保持队首，下一拍重试
            } else {
                SC_REPORT_FATAL("MemSimBackend", "mem_sim 拒绝了事务");
            }
        }

        // ---- 4. 接纳新的请求 -----------------------------------------
        SimpleMemRequest req;
        if (s.in_flight.size() < kMaxBurstsInFlight && request.nb_read(req)) {
            admit(req);
        }

        // ---- 5. 按顺序交付已完成的响应 -------------------------------
        while (!s.in_flight.empty()) {
            const auto burst = s.in_flight.front();
            const bool complete =
                burst->synthesize_only ||
                burst->chunks_done == burst->chunks.size();
            if (!complete) break;

            SimpleMemResponse rsp;
            rsp.write = burst->write;
            rsp.rp = burst->req.rp;
            rsp.id = burst->req.address.id;
            rsp.user = burst->req.address.user;
            rsp.resp = static_cast<std::uint8_t>(burst->status);

            if (burst->write) {
                // 写响应不回数据，且即使出错也必须给一个确定的状态。
                rsp.read_beats.clear();
            } else {
                // 读响应必须恰好给出 len+1 拍，错误时亦然——AouTarget 会硬校验。
                rsp.read_beats.assign(burst->beats, SimpleMemReadBeat{});
                for (unsigned n = 0; n < burst->beats; ++n) {
                    rsp.read_beats[n].data.fill(0);
                    rsp.read_beats[n].resp =
                        static_cast<std::uint8_t>(burst->status);
                    rsp.read_beats[n].user = burst->req.address.user;
                }
                for (unsigned ci = 0; ci < burst->chunks.size(); ++ci) {
                    s.scatter_read_chunk(*burst, ci, burst->chunks[ci].data,
                                         rsp.read_beats);
                }
            }

            if (!response.nb_write(rsp)) break;  // 反压：保持队首，下一拍再试

            if (burst->write) {
                write_bytes += burst->total;
            } else {
                read_bytes += burst->total;
            }
            ++completed;
            if (burst->status) ++error_responses;
            s.in_flight.pop_front();
        }

        wait();
    }
}

// 预校验并接纳一笔突发。任何一条不满足都直接合成错误响应，不进入 mem_sim——
// 见文件头的规则 1。
void MemSimBackend::admit(const SimpleMemRequest& req) {
    Impl& s = *impl_;
    auto burst = std::make_shared<Impl::Burst>();
    burst->req = req;
    burst->write = req.write;
    burst->beats = req.beats();
    burst->width = beat_width(req.address.size);

    const auto& a = req.address;
    const std::uint64_t T = static_cast<std::uint64_t>(burst->beats) * burst->width;

    // 结构性合法性与写数据条数：AouTarget 已经挡过一部分，这里再确认一次，
    // 免得它变成 SC_REPORT_FATAL 而不是一条可诊断的错误响应。
    const bool contract_ok =
        !axi_request_error(a) && (!req.write || req.write_beats.size() == burst->beats);
    if (!contract_ok) {
        ++rejected_contract;
        burst->status = 2;  // SLVERR
        burst->synthesize_only = true;
        s.in_flight.push_back(burst);
        return;
    }

    // 地址窗口：越界是 DECERR。必须在提交前挡住——mem_sim 会抛异常。
    const bool overflow = a.addr < s.base || a.addr - s.base > UINT64_MAX - T;
    const std::uint64_t mem_base = overflow ? 0 : a.addr - s.base;
    if (overflow || mem_base + T > s.size ||
        mem_base + T > s.backend->window_bytes()) {
        ++rejected_out_of_range;
        burst->status = 3;  // DECERR
        burst->synthesize_only = true;
        s.in_flight.push_back(burst);
        return;
    }
    burst->mem_base = mem_base;
    burst->total = T;

    // 独占访问不支持。
    if (a.lock) {
        ++rejected_lock;
        burst->status = 2;  // SLVERR
        burst->synthesize_only = true;
        s.in_flight.push_back(burst);
        return;
    }

    // 写选通必须落在本拍有效的通道区间内。**先全部检查完再决定**——
    // 任一错误都不修改内存，避免部分写副作用（与 SimpleBurstMemory 一致）。
    if (req.write) {
        bool bad = false;
        for (unsigned n = 0; n < burst->beats && !bad; ++n) {
            const unsigned lane = static_cast<unsigned>(
                (a.addr + static_cast<std::uint64_t>(n) * burst->width) %
                AXI_DATA_BYTES);
            for (unsigned j = 0; j < AXI_DATA_BYTES; ++j) {
                if (req.write_beats[n].strobe[j] &&
                    (j < lane || j >= lane + burst->width)) {
                    bad = true;
                    break;
                }
            }
        }
        if (bad) {
            ++rejected_strobe;
            burst->status = 2;  // SLVERR
            burst->synthesize_only = true;
            s.in_flight.push_back(burst);
            return;
        }
    }

    // 通过预校验：切成内存 chunk 并排入提交通道。
    s.build_chunks(*burst);
    for (unsigned ci = 0; ci < burst->chunks.size(); ++ci) {
        const std::uint64_t tag = s.next_tag++;
        burst->chunks[ci].tag = tag;
        Impl::PendingTxn p;
        p.burst = burst;
        p.chunk = ci;
        p.txn.address = burst->chunks[ci].mem_addr;
        p.txn.bytes = s.backend->transaction_bytes();
        p.txn.write = req.write;
        p.txn.host_request_id = tag;
        if (req.write) s.fill_write_chunk(*burst, ci, p.txn);
        s.submit_queue.push_back(std::move(p));
    }
    s.in_flight.push_back(burst);
}
