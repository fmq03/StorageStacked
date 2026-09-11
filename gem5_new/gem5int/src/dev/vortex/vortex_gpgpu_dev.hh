// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// VortexGPGPU — gem5 SimObject wrapper for libvortex-gem5.so.
//
// Lives at $GEM5_HOME/src/dev/vortex/vortex_gpgpu_dev.{cc,hh} after
// sim/simx/gem5/install.sh runs. The host-side source of truth is the
// Vortex tree (sim/simx/gem5/) so API drift between gem5 and the Vortex
// C ABI surfaces as a build error in Vortex CI, not as a gem5
// integration mystery.
//
// Design (gem5_v2_cp_migration_proposal §2.3, §2.4):
//   - dlopen the Vortex library at construction; resolve all
//     vortex_gem5_* symbols up-front so the hot paths (cpTick,
//     vortexTick, PIO read/write) are direct indirect calls.
//   - PIO range is exactly the CP regfile (PIO_BASE_ADDR + 0..+0x1FF,
//     proposal §3); no legacy OPAE register window.
//   - cpTickEvent_ self-schedules only while the CP has work; goes
//     dormant otherwise (proposal §2.3). PIO writes that may have
//     armed work re-arm the schedule.
//   - vortexTickEvent_ self-schedules only while Vortex is running;
//     scheduled by the CP's vortex_start hook via the registered
//     start handler (proposal §2.4). Standalone mode skips the CP
//     and schedules vortexTickEvent_ directly at startup.
//   - DmaDevice base class kept for forward compatibility with the
//     v2 DMA-port seam (proposal §2.5) and for the standalone smoke
//     test path that still uses gem5's pio interface.

#ifndef __DEV_VORTEX_VORTEX_GPGPU_DEV_HH__
#define __DEV_VORTEX_VORTEX_GPGPU_DEV_HH__

#include "base/coroutine.hh"
#include "dev/dma_device.hh"
#include "dev/io_device.hh"
#include "params/VortexGPGPU.hh"
#include "sim/eventq.hh"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gem5
{

class VortexGPGPU : public DmaDevice
{
public:
    using Params = VortexGPGPUParams;

    VortexGPGPU(const Params &p);
    ~VortexGPGPU() override;

    // PioDevice interface
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    AddrRangeList getAddrRanges() const override;

    // SimObject lifecycle
    void init() override;
    void startup() override;

private:
    // CP tick — advances the embedded CommandProcessor one functional
    // cycle. Self-reschedules iff cp_tick reported still-busy.
    void cpTick();

    // Vortex tick — advances the Vortex Processor one cycle.
    // Self-reschedules iff vortex_tick reported still-running.
    // Standalone mode exits the sim loop when vortex_tick returns false.
    void vortexTick();

    // Called from a PIO write to schedule cpTickEvent_ if (a) the CP
    // reports new work and (b) the event isn't already pending.
    void maybeWakeCp();

    // Static trampoline registered with the device library so the CP's
    // vortex_start hook can schedule vortexTickEvent_ via the gem5
    // event scheduler. Passing `this` via the void* ctx avoids any
    // dependency on gem5 types in the library.
    static void onVortexStartTrampoline(void* ctx);
    void onVortexStart();

    // Memory-access tracing (hettrace) --------------------------------
    // Installs a read-only tap inside the device library on the post-LLC
    // request stream. The library cannot call curTick() -- it is dlopen'd
    // and does not link gem5 -- so it gets the clock through this
    // trampoline. That is what puts Vortex's records on the same time base
    // as the host and CoralNPU traces, which is the whole point.
    static uint64_t curTickTrampoline(void* ctx);

    // No-op if trace_enable=false, if the library predates the trace ABI,
    // or if HETTRACE_DIR is unset. None of those are errors.
    void openTrace();

    // Flush + write the .meta.json sidecar. Idempotent; called from the
    // standalone completion path, from an exit callback, and from the dtor.
    void closeTrace();

    // Shared timing-memory bridge ------------------------------------
    static void memoryReadTrampoline(void* ctx, uint64_t addr, uint8_t* dst,
                                     uint32_t size);
    static void memoryWriteTrampoline(void* ctx, uint64_t addr,
                                      const uint8_t* src, uint32_t size);
    static void cpTimingTrampoline(void* ctx, uint64_t addr, bool is_write,
                                   const uint8_t* src, uint32_t size);
    static bool coreTimingTrampoline(void* ctx, uint64_t token, uint64_t addr,
                                     bool is_write, const uint8_t* src,
                                     uint64_t byteen, uint32_t size);
    void memoryRead(uint64_t addr, uint8_t* dst, uint32_t size);
    void memoryWrite(uint64_t addr, const uint8_t* src, uint32_t size);
    void enqueueCpTiming(uint64_t addr, bool is_write, const uint8_t* src,
                         uint32_t size);
    bool issueCoreTiming(uint64_t token, uint64_t addr, bool is_write,
                         const uint8_t* src, uint64_t byteen, uint32_t size);
    void coreDmaComplete(uint64_t token);
    void startNextCpDma();
    void dmaComplete();
    void resumeCpIfReady();
    void startVortexIfReady();

    // Library binding ------------------------------------------------
    void* libHandle_;
    void* deviceHandle_;

    struct AbiV2 {
        const char* (*build_info)(void);
        void*       (*create)(void);
        void        (*destroy)(void* h);
        void        (*set_start_handler)(void* h, void (*fn)(void*), void* ctx);
        int         (*load_kernel)(void* h, const char* path);
        void        (*cp_mmio_write)(void* h, uint32_t off, uint32_t value);
        uint32_t    (*cp_mmio_read)(void* h, uint32_t off);
        bool        (*cp_tick)(void* h);
        bool        (*cp_has_work)(void* h);
        bool        (*vortex_tick)(void* h);
        bool        (*vortex_busy)(void* h);
        void        (*vram_write)(void* h, uint64_t addr,
                                  const uint8_t* src, uint32_t size);
        void        (*vram_read)(void* h, uint64_t addr,
                                 uint8_t* dst, uint32_t size);
        void        (*set_memory_backend)(
                         void* h,
                         void (*read_fn)(void*, uint64_t, uint8_t*, uint32_t),
                         void (*write_fn)(void*, uint64_t, const uint8_t*,
                                          uint32_t),
                         void (*timing_fn)(void*, uint64_t, bool,
                                           const uint8_t*, uint32_t),
                         void* ctx);
        void        (*set_core_timing_backend)(
                         void* h,
                         bool (*issue_fn)(void*, uint64_t, uint64_t, bool,
                                          const uint8_t*, uint64_t, uint32_t),
                         void* ctx);
        void        (*complete_core_memory)(void* h, uint64_t token,
                                            const uint8_t* read_data,
                                            uint32_t size);
    } abi_;

    // Optional trace ABI. Resolved with dlsym but deliberately *not*
    // fatal on absence, unlike AbiV2 above: a libvortex-gem5.so built
    // before the hettrace tap existed must still load and run. Missing
    // symbols mean "no tracing available", not "version mismatch".
    struct AbiTrace {
        int      (*open)(void* h, uint64_t (*tick_fn)(void*), void* ctx,
                         int64_t addr_offset);
        void     (*close)(void* h);
        uint64_t (*emitted)(void* h);
    } traceAbi_;

    // Configuration --------------------------------------------------
    const std::string libraryPath_;
    const std::string kernelPath_;
    const Addr        pioAddr_;
    const Addr        pioSize_;
    const Addr        pinAddr_;   // device VRAM, host-visible as BAR
    const Addr        pinSize_;
    const Tick        pioLatency_;
    const bool        timingMemory_;
    const bool        exitOnFirstPio_;
    const bool        traceEnable_;
    const int64_t     traceAddrOffset_;

    // Event scheduling
    using CpCoroutine = Coroutine<void, void>;
    std::unique_ptr<CpCoroutine> cpCoroutine_;
    CpCoroutine::CallerType* cpYield_ = nullptr;
    bool cpBusy_ = false;
    EventFunctionWrapper cpTickEvent_;
    EventFunctionWrapper vortexTickEvent_;

    // Standalone (Phase 3) vs. hosted mode. Set by startup() based on
    // whether the `kernel=` Python param was provided.
    bool standalone_;

    // True between a successful openTrace() and closeTrace(). Guards the
    // idempotency of closeTrace().
    bool traceActive_;

    enum class DmaKind { None, CpRead, CpWrite };
    struct CpDmaRequest {
        Addr addr;
        bool isWrite;
        std::vector<uint8_t> data;
        uint32_t size;
        Tick issueTick;
    };
    EventFunctionWrapper dmaDoneEvent_;
    DmaKind dmaKind_ = DmaKind::None;
    std::deque<CpDmaRequest> cpDmaQueue_;
    std::unique_ptr<CpDmaRequest> activeCpDma_;
    std::vector<uint8_t> dmaData_;
    struct CoreDmaRequest {
        uint64_t token;
        Addr addr;
        bool isWrite;
        uint32_t size;
        Tick issueTick;
        std::vector<uint8_t> data;
        std::vector<bool> byteEnable;
    };
    std::unordered_map<uint64_t, std::unique_ptr<CoreDmaRequest>> coreDmas_;
    uint64_t maxCoreOutstanding_ = 0;
    bool cpResumeNeeded_ = false;
    bool vortexStartPending_ = false;
    bool timingSummaryPrinted_ = false;
    bool timingPhaseExitFired_ = false;
    uint64_t cpCycles_ = 0;
    uint64_t vortexCycles_ = 0;
    uint64_t timingCompletions_ = 0;
    uint64_t coreTimingReads_ = 0;
    uint64_t coreTimingWrites_ = 0;
    uint64_t cpTimingReads_ = 0;
    uint64_t cpTimingWrites_ = 0;
    Tick timingLatencyTotal_ = 0;
    Tick timingLatencyMax_ = 0;
};

} // namespace gem5

#endif // __DEV_VORTEX_VORTEX_GPGPU_DEV_HH__
