// libcoralnpu-gem5 实现。ABI 契约与设计约束见 coralnpu_gem5.h。
//
// 安装位置: $CORALNPU_HOME/gem5int/coralnpu_gem5.cc（由 coralnpuint/install.sh 拷入）
//
// 这个文件是 CoralNPU 的 bazel/Verilator 世界与 gem5 的 SCons 世界之间的唯一
// 接缝。C++ 类型全部止步于此，往外只有 coralnpu_gem5.h 里那些扁平 C 函数。
//
// AXI master 的两个回调是全文重点：它们同时干三件事 ——
//   1. 喂 trace tap（本工程的目的）；
//   2. 把数据路由到 gem5 内存或库内私有 DDR；
//   3. 窗口外的访问按参考实现的语义落到 mailbox。
// wrapper 的异步 request/response seam 允许回调只发请求；gem5 DmaPort 完成后再
// 调 complete_read/complete_write。没有 timing backend 时才走同步兼容路径。

#include "coralnpu_gem5.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <verilated.h>

#include "coralnpu_trace.h"
#include "hw_sim/core_mini_axi_wrapper.h"
#include "tests/verilator_sim/elf.h"

namespace {

// 库内私有 DDR 的大小。参考实现开满 1 GiB（整个窗口），但那是它自己的进程；
// 接了 gem5 后端之后这块数组一个字节都用不到，所以这里改成**首次用到才分配**，
// 而且用 calloc —— 靠内核的零页做惰性提交，不像 vector::resize(n, 0) 会把
// 1 GiB 全部写脏。地址落在已分配区之外时不 abort（那会带走整个 gem5 进程），
// 而是记一次 anomaly 并返回零。
constexpr uint64_t kPrivateDdrSize = 256ull * 1024 * 1024;

constexpr uint32_t kDdrBase = CORALNPU_GEM5_DDR_BASE;
constexpr uint32_t kDdrEnd  = CORALNPU_GEM5_DDR_BASE + CORALNPU_GEM5_DDR_SIZE;

// ctrl 寄存器，取自 core_mini_axi_wrapper_example.cc。
constexpr uint32_t kCtrlReg    = 0x30000;
constexpr uint32_t kCtrlPcReg  = 0x30004;

inline bool IsDdr(uint32_t addr) { return addr >= kDdrBase && addr < kDdrEnd; }

}  // namespace

// gem5 侧只见到 coralnpu_gem5_handle_t，这个结构体的布局永远不会漏出去。
struct coralnpu_gem5_device_s {
    VerilatedContext   context;
    CoreMiniAxiWrapper wrapper{&context};
    uint32_t startAddress = 0;
    unsigned startStage = 3;
    std::shared_ptr<bool> startWrite;

    // 用裸指针而不是成员对象，为的是 trace_close() 能确定性地关掉它而不必等
    // 整个 Device 销毁 —— gem5 不保证退出时销毁 SimObject，.meta.json 侧车
    // 文件却必须落盘（否则下游没法判断 trace 有没有被截断）。
    // AXI 回调只在 Step() 里被调用，读这个指针前查非空即可。
    coralnpu_gem5::CoralNpuTraceTap* tap = nullptr;

    // 外部内存后端。两个都非空时私有 DDR 完全不参与。
    coralnpu_gem5_mem_read_t  mem_read  = nullptr;
    coralnpu_gem5_mem_write_t mem_write = nullptr;
    void*                     mem_ctx   = nullptr;

    // Asynchronous timing backend. When installed, DDR requests leave the
    // library immediately but AXI B/R is not produced until the gem5 side
    // calls coralnpu_gem5_complete_* from its DMA completion event.
    coralnpu_gem5_timing_read_t  timing_read  = nullptr;
    coralnpu_gem5_timing_write_t timing_write = nullptr;
    void*                        timing_ctx   = nullptr;

    uint8_t* ddr = nullptr;  // 私有 DDR，惰性 calloc

    // 越界的私有 DDR 访问次数。与 tap 的 burst 异常合并上报。
    uint64_t oob = 0;

    bool trace_open = false;

    // Close() 之后 tap 就被销毁了，但 gem5 侧打退出摘要时还想知道写了多少条。
    // 关闭时把计数抄下来，于是 trace_emitted() 与调用顺序无关。
    uint64_t emitted_cache = 0;
    uint64_t burst_cache   = 0;

    ~coralnpu_gem5_device_s() {
        delete tap;
        std::free(ddr);
    }

    bool has_backend() const { return mem_read != nullptr && mem_write != nullptr; }
    bool has_timing_backend() const {
        return timing_read != nullptr && timing_write != nullptr;
    }

    // 私有 DDR，惰性分配。返回 nullptr 表示越界（已记 anomaly）。
    uint8_t* ddr_ptr(uint32_t dev_addr, uint32_t size) {
        const uint64_t off = static_cast<uint64_t>(dev_addr) - kDdrBase;
        if (off + size > kPrivateDdrSize) {
            ++oob;
            return nullptr;
        }
        if (ddr == nullptr) {
            ddr = static_cast<uint8_t*>(std::calloc(kPrivateDdrSize, 1));
            if (ddr == nullptr) {
                ++oob;
                return nullptr;
            }
        }
        return ddr + off;
    }
};

namespace {

using Device = coralnpu_gem5_device_s;

// ---- AXI master 回调 -------------------------------------------------------
//
// 每次回调 = 一个 16 字节对齐窗口，见 coralnpu_trace.h 文件头对 burst 的分析。
// 这里的数据搬运语义与参考实现 core_mini_axi_simulator.cc 逐字节一致，只是把
// "写进 ddr_memory_" 换成"写进后端或私有 DDR"，并额外喂了 tap。

AxiRData ReadCallback(Device* d, const AxiAddr& addr) {
    AxiRData out;
    uint8_t* dst = reinterpret_cast<uint8_t*>(&out.read_data_bits_data[0]);

    if (IsDdr(addr.addr_bits_addr)) {
        if (d->tap != nullptr) d->tap->OnRead(addr);

        const uint32_t aligned = addr.addr_bits_addr & ~(uint32_t)15;
        if (d->has_backend()) {
            d->mem_read(d->mem_ctx, aligned, dst, coralnpu_gem5::kAxiBeatBytes);
        } else if (const uint8_t* src =
                       d->ddr_ptr(aligned, coralnpu_gem5::kAxiBeatBytes)) {
            std::memcpy(dst, src, coralnpu_gem5::kAxiBeatBytes);
        } else {
            std::memset(dst, 0, coralnpu_gem5::kAxiBeatBytes);
        }
    } else {
        // 窗口外 -> mailbox。这是寄存器握手，不是 DRAM 流量，所以**不进 trace**
        // （与 Vortex tap 过滤 req.flags.io 是同一条原则：三个源都只记
        //  访存流量，否则合并出来的带宽曲线里会混进控制面的噪声）。
        const CoralNPUMailbox& mb = d->wrapper.mailbox();
        std::memcpy(dst, mb.message, coralnpu_gem5::kAxiBeatBytes);
    }

    out.read_data_bits_id   = addr.addr_bits_id;
    out.read_data_bits_resp = 0;
    out.read_data_bits_last = 1;
    return out;
}

AxiWResp WriteCallback(Device* d, const AxiAddr& addr, const AxiWData& data) {
    const uint8_t* src =
        reinterpret_cast<const uint8_t*>(&data.write_data_bits_data[0]);
    const uint16_t strb = data.write_data_bits_strb;

    if (IsDdr(addr.addr_bits_addr)) {
        if (d->tap != nullptr) d->tap->OnWrite(addr, data);

        const uint32_t aligned = addr.addr_bits_addr & ~(uint32_t)15;
        // 逐字节按 strb 落地。后端路径上按连续区间合并成一次调用会更快，但
        // 正确性优先且这里不是热点（一拍最多 16 字节），保持与参考实现同形。
        for (uint32_t i = 0; i < coralnpu_gem5::kAxiBeatBytes; ++i) {
            if ((strb & (1u << i)) == 0) continue;
            if (d->has_backend()) {
                d->mem_write(d->mem_ctx, aligned + i, src + i, 1);
            } else if (uint8_t* dst = d->ddr_ptr(aligned + i, 1)) {
                *dst = src[i];
            }
        }
    } else {
        CoralNPUMailbox& mb = d->wrapper.mailbox();
        uint8_t* mb_bytes   = reinterpret_cast<uint8_t*>(mb.message);
        for (uint32_t i = 0; i < coralnpu_gem5::kAxiBeatBytes; ++i) {
            if (strb & (1u << i)) mb_bytes[i] = src[i];
        }
    }

    AxiWResp resp;
    resp.write_resp_bits_id   = addr.addr_bits_id;
    resp.write_resp_bits_resp = 0;
    return resp;
}

void AsyncReadCallback(Device* d, const AxiAddr& addr) {
    if (!IsDdr(addr.addr_bits_addr) || !d->has_timing_backend()) {
        d->wrapper.CompleteRead(ReadCallback(d, addr));
        return;
    }

    if (d->tap != nullptr) d->tap->OnRead(addr);
    const uint32_t aligned = addr.addr_bits_addr & ~(uint32_t)15;
    d->timing_read(d->timing_ctx, aligned, addr.addr_bits_id,
                   coralnpu_gem5::kAxiBeatBytes);
}

void AsyncWriteCallback(Device* d, const AxiAddr& addr, const AxiWData& data) {
    if (!IsDdr(addr.addr_bits_addr) || !d->has_timing_backend()) {
        d->wrapper.CompleteWrite(WriteCallback(d, addr, data));
        return;
    }

    if (d->tap != nullptr) d->tap->OnWrite(addr, data);
    const uint32_t aligned = addr.addr_bits_addr & ~(uint32_t)15;
    const uint8_t* src =
        reinterpret_cast<const uint8_t*>(&data.write_data_bits_data[0]);
    d->timing_write(d->timing_ctx, aligned, addr.addr_bits_id, src,
                    data.write_data_bits_strb,
                    coralnpu_gem5::kAxiBeatBytes);
}

}  // namespace

// ---- C ABI -----------------------------------------------------------------

extern "C" {

const char* coralnpu_gem5_build_info(void) {
#ifdef ENABLE_RVV
    return "libcoralnpu-gem5 (CoreMiniAxi, RVV=on)";
#else
    return "libcoralnpu-gem5 (CoreMiniAxi, RVV=off)";
#endif
}

coralnpu_gem5_handle_t coralnpu_gem5_create(void) {
    Device* d = new (std::nothrow) Device();
    if (d == nullptr) return nullptr;

    d->wrapper.RegisterAsyncReadCallback(
        [d](const AxiAddr& a) { AsyncReadCallback(d, a); });
    d->wrapper.RegisterAsyncWriteCallback(
        [d](const AxiAddr& a, const AxiWData& w) {
            AsyncWriteCallback(d, a, w);
        });

    d->wrapper.Reset();
    return d;
}

void coralnpu_gem5_destroy(coralnpu_gem5_handle_t h) {
    if (h == nullptr) return;
    coralnpu_gem5_trace_close(h);
    delete h;
}

void coralnpu_gem5_set_mem_backend(coralnpu_gem5_handle_t h,
                                   coralnpu_gem5_mem_read_t read_fn,
                                   coralnpu_gem5_mem_write_t write_fn,
                                   void* ctx) {
    if (h == nullptr) return;
    h->mem_read  = read_fn;
    h->mem_write = write_fn;
    h->mem_ctx   = ctx;
}

void coralnpu_gem5_set_timing_backend(
    coralnpu_gem5_handle_t h,
    coralnpu_gem5_timing_read_t read_fn,
    coralnpu_gem5_timing_write_t write_fn,
    void* ctx) {
    if (h == nullptr) return;
    h->timing_read  = read_fn;
    h->timing_write = write_fn;
    h->timing_ctx   = ctx;
}

void coralnpu_gem5_complete_read(coralnpu_gem5_handle_t h, uint8_t axi_id,
                                 const uint8_t* src, uint32_t size,
                                 uint8_t resp) {
    if (h == nullptr || src == nullptr) return;
    AxiRData out{};
    uint8_t* dst = reinterpret_cast<uint8_t*>(&out.read_data_bits_data[0]);
    const uint32_t copy_size =
        std::min(size, coralnpu_gem5::kAxiBeatBytes);
    std::memcpy(dst, src, copy_size);
    out.read_data_bits_id   = axi_id;
    out.read_data_bits_resp = resp;
    out.read_data_bits_last = 1;
    h->wrapper.CompleteRead(out);
}

void coralnpu_gem5_complete_write(coralnpu_gem5_handle_t h, uint8_t axi_id,
                                  uint8_t resp) {
    if (h == nullptr) return;
    AxiWResp out{};
    out.write_resp_bits_id   = axi_id;
    out.write_resp_bits_resp = resp;
    h->wrapper.CompleteWrite(out);
}

void coralnpu_gem5_ddr_write(coralnpu_gem5_handle_t h, uint64_t addr,
                             const uint8_t* src, uint32_t size) {
    if (h == nullptr || h->has_backend()) return;
    if (uint8_t* dst = h->ddr_ptr(static_cast<uint32_t>(addr), size)) {
        std::memcpy(dst, src, size);
    }
}

void coralnpu_gem5_ddr_read(coralnpu_gem5_handle_t h, uint64_t addr,
                            uint8_t* dst, uint32_t size) {
    if (h == nullptr || h->has_backend()) return;
    if (const uint8_t* src = h->ddr_ptr(static_cast<uint32_t>(addr), size)) {
        std::memcpy(dst, src, size);
    } else {
        std::memset(dst, 0, size);
    }
}

int coralnpu_gem5_load_elf(coralnpu_gem5_handle_t h, const char* path,
                           uint32_t* out_entry) {
    if (h == nullptr || path == nullptr || out_entry == nullptr) return -1;

    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "coralnpu_gem5: open('%s') failed\n", path);
        return -1;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        std::fprintf(stderr, "coralnpu_gem5: fstat('%s') failed\n", path);
        return -1;
    }
    void* mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                          MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        std::fprintf(stderr, "coralnpu_gem5: mmap('%s') failed\n", path);
        return -1;
    }

    // wrapper.Write() 内部 while(...) Step() 阻塞推进时钟 —— 只有在 gem5 还没
    // 开始 tick 之前调用才安全。见 coralnpu_gem5.h 文件头。
    CopyFn copy_fn = [h](void* dest, const void* src, size_t count) {
        h->wrapper.Write(static_cast<uint32_t>(reinterpret_cast<uint64_t>(dest)),
                         static_cast<uint32_t>(count),
                         static_cast<const char*>(src));
        return dest;
    };
    *out_entry = LoadElf(static_cast<uint8_t*>(mapped), copy_fn);

    ::munmap(mapped, static_cast<size_t>(st.st_size));
    return 0;
}

void coralnpu_gem5_start(coralnpu_gem5_handle_t h, uint32_t start_addr) {
    if (h == nullptr) return;
    h->startAddress = start_addr;
    h->startStage = 0;
    h->startWrite.reset();
}

bool coralnpu_gem5_tick(coralnpu_gem5_handle_t h) {
    if (h == nullptr) return false;
    if (h->startStage < 3) {
        if (h->startWrite && *h->startWrite) {
            h->startWrite.reset();
            ++h->startStage;
        }
        if (h->startStage < 3 && !h->startWrite) {
            const uint32_t addr = h->startStage == 0 ? kCtrlPcReg : kCtrlReg;
            const uint32_t value = h->startStage == 0 ? h->startAddress : (h->startStage == 1 ? 1 : 0);
            h->startWrite = h->wrapper.EnqueueWriteWord(addr, value);
        }
    }
    h->wrapper.Step();
    return h->startStage < 3 || (!h->wrapper.halted() && !h->wrapper.wfi());
}

bool coralnpu_gem5_halted(coralnpu_gem5_handle_t h) {
    return h != nullptr && h->startStage == 3 && h->wrapper.halted();
}

bool coralnpu_gem5_wfi(coralnpu_gem5_handle_t h) {
    return h != nullptr && h->startStage == 3 && h->wrapper.wfi();
}

uint32_t coralnpu_gem5_mailbox_read(coralnpu_gem5_handle_t h, uint32_t index) {
    if (h == nullptr || index >= 4) return 0;
    return h->wrapper.mailbox().message[index];
}

void coralnpu_gem5_mailbox_write(coralnpu_gem5_handle_t h, uint32_t index,
                                 uint32_t value) {
    if (h == nullptr || index >= 4) return;
    h->wrapper.mailbox().message[index] = value;
}

int coralnpu_gem5_trace_open(coralnpu_gem5_handle_t h,
                             coralnpu_gem5_tick_provider_t tick_fn,
                             void* tick_ctx,
                             int64_t addr_offset) {
    if (h == nullptr) return -1;
    if (h->trace_open) return 0;

    // coralnpu_gem5_tick_provider_t 与 coralnpu_gem5::TickProvider 是同一个
    // 函数指针类型，直接传，不需要转换。
    auto tap = std::make_unique<coralnpu_gem5::CoralNpuTraceTap>();
    if (!tap->Open(tick_fn, tick_ctx, addr_offset)) {
        // HETTRACE_DIR 没设，或者文件建不出来。不是致命错误。
        return -1;
    }
    // 先把 tap 完全建好再让回调看见它 —— 回调里只检查指针非空。
    h->tap        = tap.release();
    h->trace_open = true;
    return 0;
}

void coralnpu_gem5_trace_close(coralnpu_gem5_handle_t h) {
    if (h == nullptr || !h->trace_open) return;
    // 先摘掉回调里的指针，再关文件：Close() 之后 tap 就不能再被 Emit 了。
    coralnpu_gem5::CoralNpuTraceTap* tap = h->tap;
    h->tap        = nullptr;
    h->trace_open = false;
    if (tap != nullptr) {
        h->emitted_cache = tap->stats().emitted;
        h->burst_cache   = tap->unexpected_burst_count();
        tap->Close();
        delete tap;
    }
}

uint64_t coralnpu_gem5_trace_emitted(coralnpu_gem5_handle_t h) {
    if (h == nullptr) return 0;
    return (h->tap != nullptr) ? h->tap->stats().emitted : h->emitted_cache;
}

uint64_t coralnpu_gem5_trace_anomalies(coralnpu_gem5_handle_t h) {
    if (h == nullptr) return 0;
    const uint64_t burst = (h->tap != nullptr) ? h->tap->unexpected_burst_count()
                                               : h->burst_cache;
    return burst + h->oob;
}

}  // extern "C"
