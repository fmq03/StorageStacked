// libhettrace — trace 写出器。header-only。
//
// 配置全部走环境变量，这样三个 tap 都不必把选项一路透传下来：
//
//   HETTRACE_DIR      输出目录。未设置 => 完全关闭，Emit() 退化为一次分支判断。
//   HETTRACE_FORMAT   bin(默认) | text
//   HETTRACE_FILTER   dram(默认) | all
//                     dram: 只记录落在 addrmap 的 trace_windows 里的访问 ——
//                     DRAM 窗口，加上 Vortex 的 BAR（经 BAR 的访问也是真实内存
//                     流量）。NPU 的 TCM 命中、CP 寄存器读写等不是内存流量，
//                     混进来会让带宽统计虚高。判据是 IsTraced()，不是 IsDram()：
//                     后者只表示 CoralNPU 的 DDR 判定区间，两者含义不同。
//   HETTRACE_BUFSZ    缓冲记录条数，默认 65536
//
// 两级保真度，写同一种记录（见 record.h）：
//
//   Emit()/EmitBurst()  只写数据通道（W/R）。语义与 v1 格式逐字段等价，条数
//                       也一致。设备库里的 tap 用这一级 —— 它们看到的是"一次
//                       访问"，本来就观察不到独立的地址通道和响应通道，硬造
//                       AW/B 记录等于把推测写成观测。
//   BeginWrite() 等     写全部五个通道。gem5 的 HetAxiMonitor 用这一级：它在
//                       packet 路径上，请求与响应是两个不同时刻的事件，能如实
//                       记下 AW→W…→B 的因果与时间差。
//
// 线程安全：无。三个 tap 都在 gem5 事件循环线程上被调用（见
// vortex_gpgpu.h 的 "Concurrency" 注释），故不加锁。

#ifndef HETTRACE_WRITER_H_
#define HETTRACE_WRITER_H_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hettrace/addrmap.h"
#include "hettrace/record.h"

namespace hettrace {

// 宽度 <= 64 字节时的全 lane 使能。AXI 数据总线宽于 64 B 的情况本工程不涉及，
// 真遇到了 strb 字段本身就不够用，必须先扩 record.h。
inline uint64_t FullStrb(uint32_t bytes) {
    return bytes >= 64 ? ~0ull : ((1ull << bytes) - 1ull);
}

// log2(bytes)，bytes 必须是 2 的幂。非 2 的幂在 AXI 上无法用单个 AxSIZE 表达，
// 这里向下取整并由调用方保证对齐 —— 越界比静默取错值好，但热路径上不做断言。
inline uint8_t Log2Size(uint32_t bytes) {
    uint8_t s = 0;
    while ((1u << (s + 1)) <= bytes && s < 7) ++s;
    return s;
}

// HETTrace 可转换 AXI 子集要求首地址按每拍字节数对齐。这个 helper 仍完整
// 实现 FIXED/INCR/WRAP 的逐拍地址，供 writer 的所有全通道路径共用，避免
// 地址通道声明 WRAP、数据通道却误按 INCR 展开的自相矛盾。
inline uint64_t AxiBeatAddress(uint64_t start, uint32_t beat_bytes,
                               uint32_t beats, uint8_t burst,
                               uint32_t beat) {
    if (burst == kBurstFixed) return start;
    if (burst == kBurstWrap) {
        const uint64_t span = static_cast<uint64_t>(beat_bytes) * beats;
        const uint64_t boundary = (start / span) * span;
        return boundary + ((start - boundary +
                            static_cast<uint64_t>(beat) * beat_bytes) % span);
    }
    return start + static_cast<uint64_t>(beat) * beat_bytes;
}

// 一笔 AXI 事务的描述。调用方在请求时构造，在响应时原样传回 —— writer 不保存
// 未完成事务的状态，因此 header-only 且零分配。gem5 的 monitor 本来就要用
// senderState 记住 outstanding packet，让它顺带记住这个描述比在 writer 里再维护
// 一张表更省事，也不会两处状态对不上。
struct AxiTxn {
    uint64_t addr     = 0;
    uint32_t bytes    = 0;              // 整笔字节数
    uint32_t ctx      = 0;
    uint32_t txn      = 0;              // NextTxn() 分配
    uint16_t axi_id   = 0;
    uint8_t  axi_size = 4;              // log2(每拍字节数)，默认 16 B
    uint8_t  burst    = kBurstIncr;
    uint8_t  user     = 0;
    uint8_t  flags    = 0;

    uint32_t beat_bytes() const { return 1u << axi_size; }

    // AxLEN。全通道 API 只接受地址对齐、整拍的事务；ValidateTxn() 会在
    // 写记录前拒绝非整拍、超宽或非法 burst。这里仍向上取整，使无效输入的
    // 诊断值确定，不允许 uint32 下溢。
    uint8_t axi_len() const {
        const uint32_t bb = beat_bytes();
        const uint32_t beats = bb == 0 ? 1u : (bytes + bb - 1u) / bb;
        return static_cast<uint8_t>((beats == 0 ? 1u : beats) - 1u);
    }
};

class TraceWriter {
  public:
    TraceWriter() = default;

    ~TraceWriter() { Close(); }

    TraceWriter(const TraceWriter&)            = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    // 若 HETTRACE_DIR 未设置则返回 false 且保持关闭状态——调用方应把它当作
    // "用户没要 trace"，不是错误。
    //
    // src_name 同时作为文件名: $HETTRACE_DIR/<src_name>.hettrace[.txt]
    //
    // synth=true 表示本源的 AXI 字段是按 docs/06-storage-chain-plan.md 的契约
    // 推导的，不是从真 AXI 信号读到的；它会打在每条记录的 kFlagSynth 上。
    bool Open(uint16_t src_id, const char* src_name, TapLevel level,
              uint64_t clock_period_ticks, uint16_t axi_data_bytes = 16,
              uint16_t axi_addr_bits = kMapAddrBits, bool synth = false,
              uint64_t ticks_per_second = kTicksPerSecond) {
        Close();

        const char* dir = std::getenv("HETTRACE_DIR");
        if (dir == nullptr || dir[0] == '\0') {
            return false;
        }

        const char* fmt = std::getenv("HETTRACE_FORMAT");
        text_ = (fmt != nullptr && std::strcmp(fmt, "text") == 0);

        const char* filt = std::getenv("HETTRACE_FILTER");
        dram_only_ = !(filt != nullptr && std::strcmp(filt, "all") == 0);

        const char* bufsz = std::getenv("HETTRACE_BUFSZ");
        size_t cap = 65536;
        if (bufsz != nullptr) {
            long v = std::strtol(bufsz, nullptr, 10);
            if (v > 0) cap = static_cast<size_t>(v);
        }

        path_ = std::string(dir) + "/" + src_name +
                (text_ ? ".hettrace.txt" : ".hettrace");
        fp_ = std::fopen(path_.c_str(), text_ ? "w" : "wb");
        if (fp_ == nullptr) {
            std::fprintf(stderr, "hettrace: 无法打开 %s\n", path_.c_str());
            return false;
        }

        src_id_             = src_id;
        src_name_           = src_name;
        level_              = level;
        clock_period_ticks_ = clock_period_ticks;
        ticks_per_second_   = ticks_per_second;
        axi_data_bytes_     = axi_data_bytes;
        axi_addr_bits_      = axi_addr_bits;
        synth_              = synth;
        stats_              = Stats();
        seq_                = 0;
        txn_                = 0;
        buf_.clear();
        buf_.reserve(cap);
        cap_ = cap;

        WriteHeader();
        open_ = true;
        return true;
    }

    void Close() {
        if (!open_) return;
        Flush();
        std::fclose(fp_);
        fp_ = nullptr;
        WriteMeta();
        open_ = false;
    }

    bool is_open() const { return open_; }

    const Stats& stats() const { return stats_; }

    const std::string& path() const { return path_; }

    // 事务号。即使 trace 关闭也照常自增，这样调用方不必按 is_open() 分支。
    uint32_t NextTxn() { return txn_++; }

    // -----------------------------------------------------------------------
    // 数据通道级：设备库 tap 用。语义与 v1 格式等价。
    // -----------------------------------------------------------------------

    // 热路径。关闭时只有一次分支。
    bool Emit(uint64_t tick, uint64_t addr, uint32_t size, Op op, uint32_t ctx,
              uint8_t flags = 0) {
        if (!open_) return false;
        if (size == 0 || size > axi_data_bytes_ || size > 64) {
            std::fprintf(
                stderr,
                "hettrace: Emit 拒绝 size=%u（header AXI 数据宽度=%uB）\n",
                static_cast<unsigned>(size),
                static_cast<unsigned>(axi_data_bytes_));
            return false;
        }
        Record r;
        InitRecord(&r);
        r.tick     = tick;
        r.addr     = addr;
        r.size     = size;
        r.ctx      = ctx;
        r.txn      = txn_++;
        r.axi_id   = static_cast<uint16_t>(ctx);
        r.op       = static_cast<uint8_t>(op);
        r.chan     = (op == kWrite) ? kChanW : kChanR;
        r.axi_size = Log2Size(size);
        r.axi_len  = 0;
        r.strb     = (op == kWrite) ? FullStrb(size) : 0;
        r.flags    = flags | kFlagLast;
        Push(r);
        return true;
    }

    // AXI burst 展开：master 侧回调按拍触发，但 AxiAddr 携带的是整笔事务的
    // 首地址（见 hw_primitives.h AxiMasterWriteDriver::OnFallingEdge——每拍都
    // 用同一个 axi_addr_）。若直接按拍记首地址，会得到 N 条相同地址的记录，
    // 让局部性分析完全失真。这里按 AXI4 INCR 语义展开成 len+1 拍。
    bool EmitBurst(uint64_t tick, uint64_t base_addr, uint8_t axi_len,
                   uint8_t axi_size, Op op, uint32_t ctx) {
        if (!open_) return false;
        const uint32_t beat_bytes = axi_size < 32 ? (1u << axi_size) : 0;
        if (beat_bytes == 0 || beat_bytes > axi_data_bytes_ ||
            beat_bytes > 64 || base_addr % beat_bytes != 0) {
            std::fprintf(
                stderr,
                "hettrace: EmitBurst 拒绝非法几何 "
                "(addr=0x%llx AxLEN=%u AxSIZE=%u bus=%uB)\n",
                static_cast<unsigned long long>(base_addr),
                static_cast<unsigned>(axi_len),
                static_cast<unsigned>(axi_size),
                static_cast<unsigned>(axi_data_bytes_));
            return false;
        }
        const uint32_t beats      = static_cast<uint32_t>(axi_len) + 1u;
        const uint32_t id         = txn_++;
        for (uint32_t i = 0; i < beats; ++i) {
            Record r;
            InitRecord(&r);
            r.tick     = tick;
            r.addr     = base_addr + static_cast<uint64_t>(i) * beat_bytes;
            r.size     = beat_bytes;
            r.ctx      = ctx;
            r.txn      = id;
            r.axi_id   = static_cast<uint16_t>(ctx);
            r.op       = static_cast<uint8_t>(op);
            r.chan     = (op == kWrite) ? kChanW : kChanR;
            r.axi_size = axi_size;
            r.axi_len  = axi_len;
            r.strb     = (op == kWrite) ? FullStrb(beat_bytes) : 0;
            r.flags    = (i == 0 ? 0 : kFlagBurstBeat) |
                         (i + 1 == beats ? kFlagLast : 0);
            Push(r);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // 全通道级：gem5 HetAxiMonitor 用。
    //
    // 写事务：请求时 BeginWrite()（写出 AW 与全部 W），响应时 CompleteWrite()
    //         （写出 B）。
    // 读事务：请求时 BeginRead()（只写 AR —— 读数据此刻还不存在），响应时
    //         CompleteRead()（写出全部 R）。
    //
    // 这个不对称是有意的：它如实反映 AXI 上写数据随请求走、读数据随响应回。
    // 把 R 也放在请求时刻写，会让"读延迟"在 trace 里彻底消失。
    // -----------------------------------------------------------------------

    // strb_per_beat 为 nullptr 表示每拍全 lane 使能。
    bool BeginWrite(uint64_t tick, const AxiTxn& t,
                    const uint64_t* strb_per_beat = nullptr) {
        if (!open_) return false;
        if (!ValidateTxn(t, "BeginWrite")) return false;
        EmitAddrChan(tick, t, /*write=*/true);
        const uint32_t bb    = t.beat_bytes();
        const uint32_t beats = static_cast<uint32_t>(t.axi_len()) + 1u;
        for (uint32_t i = 0; i < beats; ++i) {
            Record r;
            InitRecord(&r);
            r.tick     = tick;
            r.addr     = AxiBeatAddress(t.addr, bb, beats, t.burst, i);
            r.size     = bb;
            r.ctx      = t.ctx;
            r.txn      = t.txn;
            r.axi_id   = t.axi_id;
            r.op       = kWrite;
            r.chan     = kChanW;
            r.axi_size = t.axi_size;
            r.axi_len  = t.axi_len();
            r.burst    = t.burst;
            r.user     = t.user;
            r.strb     = strb_per_beat
                             ? strb_per_beat[i]
                             : (FullStrb(bb) <<
                                (r.addr % axi_data_bytes_));
            r.flags    = t.flags | (i == 0 ? 0 : kFlagBurstBeat) |
                         (i + 1 == beats ? kFlagLast : 0);
            Push(r);
        }
        return true;
    }

    bool CompleteWrite(uint64_t tick, const AxiTxn& t, uint8_t resp) {
        if (!open_) return false;
        if (!ValidateTxn(t, "CompleteWrite")) return false;
        Record r;
        InitRecord(&r);
        r.tick     = tick;
        r.addr     = t.addr;
        r.size     = 0;            // B 通道不搬字节
        r.ctx      = t.ctx;
        r.txn      = t.txn;
        r.axi_id   = t.axi_id;
        r.op       = kWrite;
        r.chan     = kChanB;
        r.axi_size = t.axi_size;
        r.axi_len  = t.axi_len();
        r.burst    = t.burst;
        r.user     = t.user;
        r.resp     = resp;
        r.flags    = t.flags | kFlagLast;
        Push(r);
        return true;
    }

    bool BeginRead(uint64_t tick, const AxiTxn& t) {
        if (!open_) return false;
        if (!ValidateTxn(t, "BeginRead")) return false;
        EmitAddrChan(tick, t, /*write=*/false);
        return true;
    }

    bool CompleteRead(uint64_t tick, const AxiTxn& t, uint8_t resp) {
        if (!open_) return false;
        if (!ValidateTxn(t, "CompleteRead")) return false;
        const uint32_t bb    = t.beat_bytes();
        const uint32_t beats = static_cast<uint32_t>(t.axi_len()) + 1u;
        for (uint32_t i = 0; i < beats; ++i) {
            Record r;
            InitRecord(&r);
            r.tick     = tick;
            r.addr     = AxiBeatAddress(t.addr, bb, beats, t.burst, i);
            r.size     = bb;
            r.ctx      = t.ctx;
            r.txn      = t.txn;
            r.axi_id   = t.axi_id;
            r.op       = kRead;
            r.chan     = kChanR;
            r.axi_size = t.axi_size;
            r.axi_len  = t.axi_len();
            r.burst    = t.burst;
            r.user     = t.user;
            r.resp     = resp;
            r.flags    = t.flags | (i == 0 ? 0 : kFlagBurstBeat) |
                         (i + 1 == beats ? kFlagLast : 0);
            Push(r);
        }
        return true;
    }

    void Flush() {
        if (fp_ == nullptr || buf_.empty()) return;
        if (text_) {
            for (const Record& r : buf_) {
                std::fprintf(
                    fp_,
                    "%llu %u %s %s 0x%llx %u %u %u %u %s %s %u 0x%llx %u %u %u "
                    "0x%02x\n",
                    static_cast<unsigned long long>(r.tick),
                    static_cast<unsigned>(r.src_id), ChanName(r.chan),
                    r.op == kWrite ? "W" : "R",
                    static_cast<unsigned long long>(r.addr),
                    static_cast<unsigned>(r.size),
                    static_cast<unsigned>(r.axi_id),
                    static_cast<unsigned>(r.axi_len),
                    static_cast<unsigned>(r.axi_size), BurstName(r.burst),
                    RespName(r.resp), static_cast<unsigned>(r.user),
                    static_cast<unsigned long long>(r.strb),
                    static_cast<unsigned>(r.ctx), static_cast<unsigned>(r.seq),
                    static_cast<unsigned>(r.txn),
                    static_cast<unsigned>(r.flags));
            }
        } else {
            std::fwrite(buf_.data(), sizeof(Record), buf_.size(), fp_);
        }
        buf_.clear();
    }

  private:
    bool ValidateTxn(const AxiTxn& t, const char* operation) const {
        const bool shift_ok = t.axi_size < 32;
        const uint32_t bb = shift_ok ? (1u << t.axi_size) : 0;
        const uint32_t beats = bb == 0 ? 0 : t.bytes / bb;
        const bool burst_ok =
            t.burst == kBurstIncr ||
            (t.burst == kBurstFixed && beats <= 16) ||
            (t.burst == kBurstWrap &&
             (beats == 2 || beats == 4 || beats == 8 || beats == 16));
        const bool valid =
            bb != 0 && bb <= axi_data_bytes_ && t.bytes != 0 &&
            t.bytes % bb == 0 && beats >= 1 && beats <= 256 &&
            t.addr % bb == 0 && burst_ok;
        if (!valid) {
            std::fprintf(
                stderr,
                "hettrace: %s 拒绝非法 AXI 几何 "
                "(addr=0x%llx bytes=%u AxSIZE=%u burst=%u bus=%uB)\n",
                operation, static_cast<unsigned long long>(t.addr),
                static_cast<unsigned>(t.bytes),
                static_cast<unsigned>(t.axi_size),
                static_cast<unsigned>(t.burst),
                static_cast<unsigned>(axi_data_bytes_));
        }
        return valid;
    }

    void InitRecord(Record* r) const {
        std::memset(r, 0, sizeof(*r));
        r->src_id = src_id_;
        r->burst  = kBurstIncr;
        r->resp   = kRespOkay;
        r->user   = static_cast<uint8_t>(src_id_);
    }

    void EmitAddrChan(uint64_t tick, const AxiTxn& t, bool write) {
        Record r;
        InitRecord(&r);
        r.tick     = tick;
        r.addr     = t.addr;
        r.size     = t.bytes;      // 地址通道记整笔字节数
        r.ctx      = t.ctx;
        r.txn      = t.txn;
        r.axi_id   = t.axi_id;
        r.op       = write ? kWrite : kRead;
        r.chan     = write ? kChanAw : kChanAr;
        r.axi_size = t.axi_size;
        r.axi_len  = t.axi_len();
        r.burst    = t.burst;
        r.user     = t.user;
        r.flags    = t.flags;
        Push(r);
    }

    // 过滤、统计、落缓冲。所有写记录的路径都必须经过这里，否则 meta.json 里的
    // 计数与文件内容会对不上，而那正是 validate 唯一能发现"进程被杀、缓冲没刷"
    // 的手段。
    void Push(Record& r) {
        if (!open_) return;

        // 先分类、再过滤。未映射地址通常也不在 trace window 内；若先 return，
        // 默认 HETTRACE_FILTER=dram 会把真正的地址图错误静默算成普通 filtered，
        // 文件侧又没有记录可供 validator 复核。meta.unmapped 必须即使在记录被
        // 过滤时也保留下来，才能让默认配置可靠地暴露错误地址。
        if (RegionOf(r.addr) == nullptr) {
            ++stats_.unmapped;
            r.flags |= kFlagUnmapped;
        }
        if (dram_only_ && !IsTraced(r.addr)) {
            ++stats_.filtered;
            return;
        }
        if (synth_) r.flags |= kFlagSynth;

        if (stats_.emitted == 0) {
            stats_.first_tick = r.tick;
        } else if (r.tick < stats_.last_tick) {
            ++stats_.non_monotonic;
        }
        stats_.last_tick = r.tick;
        ++stats_.emitted;
        if (IsDataChan(r.chan)) {
            ++stats_.data_records;
            stats_.bytes += r.size;
        }
        if (r.chan == kChanAw || r.chan == kChanAr) ++stats_.transactions;

        r.seq = seq_++;
        buf_.push_back(r);
        if (buf_.size() >= cap_) Flush();
    }

    void WriteHeader() {
        if (text_) {
            // 文本模式下头部写成注释，字段与二进制头一一对应，便于人读。
            std::fprintf(fp_, "# hettrace v%u text\n", kFormatVersion);
            std::fprintf(fp_, "# src_id=%u name=%s level=%u\n",
                         static_cast<unsigned>(src_id_), src_name_.c_str(),
                         static_cast<unsigned>(level_));
            std::fprintf(fp_, "# ticks_per_second=%llu clock_period_ticks=%llu\n",
                         static_cast<unsigned long long>(ticks_per_second_),
                         static_cast<unsigned long long>(clock_period_ticks_));
            std::fprintf(fp_, "# axi_data_bytes=%u axi_addr_bits=%u\n",
                         static_cast<unsigned>(axi_data_bytes_),
                         static_cast<unsigned>(axi_addr_bits_));
            std::fprintf(fp_, "# filter=%s\n", dram_only_ ? "dram" : "all");
            std::fprintf(fp_, "# tick src chan op addr size axi_id len axsize "
                              "burst resp user strb ctx seq txn flags\n");
            return;
        }
        FileHeader h;
        std::memset(&h, 0, sizeof(h));
        std::memcpy(h.magic, kMagic, sizeof(kMagic));
        h.version            = kFormatVersion;
        h.record_size        = static_cast<uint32_t>(sizeof(Record));
        h.ticks_per_second   = ticks_per_second_;
        h.clock_period_ticks = clock_period_ticks_;
        h.src_id             = src_id_;
        h.level              = static_cast<uint8_t>(level_);
        h.flags              = dram_only_ ? kHdrFilteredDram : 0;
        h.axi_data_bytes     = axi_data_bytes_;
        h.axi_addr_bits      = axi_addr_bits_;
        std::strncpy(h.name, src_name_.c_str(), kNameMax - 1);
        std::fwrite(&h, sizeof(h), 1, fp_);
    }

    // 侧车 meta 文件。下游 validate 用它交叉核对记录数——若 meta 说 emitted=N
    // 而文件里只有 M<N 条，说明进程被杀且缓冲未刷出，这种 trace 不能用。
    void WriteMeta() {
        const std::string mp = path_ + ".meta.json";
        FILE* mf = std::fopen(mp.c_str(), "w");
        if (mf == nullptr) return;
        std::fprintf(mf, "{\n");
        std::fprintf(mf, "  \"src_id\": %u,\n", static_cast<unsigned>(src_id_));
        std::fprintf(mf, "  \"name\": \"%s\",\n", src_name_.c_str());
        std::fprintf(mf, "  \"level\": %u,\n", static_cast<unsigned>(level_));
        std::fprintf(mf, "  \"format\": \"%s\",\n", text_ ? "text" : "bin");
        std::fprintf(mf, "  \"filter\": \"%s\",\n", dram_only_ ? "dram" : "all");
        std::fprintf(mf, "  \"synth\": %s,\n", synth_ ? "true" : "false");
        std::fprintf(mf, "  \"axi_data_bytes\": %u,\n",
                     static_cast<unsigned>(axi_data_bytes_));
        std::fprintf(mf, "  \"axi_addr_bits\": %u,\n",
                     static_cast<unsigned>(axi_addr_bits_));
        std::fprintf(mf, "  \"ticks_per_second\": %llu,\n",
                     static_cast<unsigned long long>(ticks_per_second_));
        std::fprintf(mf, "  \"clock_period_ticks\": %llu,\n",
                     static_cast<unsigned long long>(clock_period_ticks_));
        std::fprintf(mf, "  \"emitted\": %llu,\n",
                     static_cast<unsigned long long>(stats_.emitted));
        std::fprintf(mf, "  \"data_records\": %llu,\n",
                     static_cast<unsigned long long>(stats_.data_records));
        std::fprintf(mf, "  \"transactions\": %llu,\n",
                     static_cast<unsigned long long>(stats_.transactions));
        std::fprintf(mf, "  \"filtered\": %llu,\n",
                     static_cast<unsigned long long>(stats_.filtered));
        std::fprintf(mf, "  \"unmapped\": %llu,\n",
                     static_cast<unsigned long long>(stats_.unmapped));
        std::fprintf(mf, "  \"non_monotonic\": %llu,\n",
                     static_cast<unsigned long long>(stats_.non_monotonic));
        std::fprintf(mf, "  \"bytes\": %llu,\n",
                     static_cast<unsigned long long>(stats_.bytes));
        std::fprintf(mf, "  \"first_tick\": %llu,\n",
                     static_cast<unsigned long long>(stats_.first_tick));
        std::fprintf(mf, "  \"last_tick\": %llu\n",
                     static_cast<unsigned long long>(stats_.last_tick));
        std::fprintf(mf, "}\n");
        std::fclose(mf);
    }

    FILE*               fp_                 = nullptr;
    bool                open_               = false;
    bool                text_               = false;
    bool                dram_only_          = true;
    bool                synth_              = false;
    uint16_t            src_id_             = 0;
    uint16_t            axi_data_bytes_     = 16;
    uint16_t            axi_addr_bits_      = kMapAddrBits;
    TapLevel            level_              = kLevelPostLlc;
    uint64_t            clock_period_ticks_ = 0;
    uint64_t            ticks_per_second_ = kTicksPerSecond;
    uint32_t            seq_                = 0;
    uint32_t            txn_                = 0;
    size_t              cap_                = 65536;
    std::string         src_name_;
    std::string         path_;
    std::vector<Record> buf_;
    Stats               stats_;
};

}  // namespace hettrace

#endif  // HETTRACE_WRITER_H_
