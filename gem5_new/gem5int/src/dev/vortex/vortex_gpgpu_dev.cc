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

#include "dev/vortex/vortex_gpgpu_dev.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "mem/packet_access.hh"
#include "mem/port_proxy.hh"
#include <limits>
#include "sim/core.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

#include <algorithm>
#include <dlfcn.h>

namespace gem5
{

namespace {

template <typename T>
T dlsym_or_fatal(void* handle, const char* symbol, const char* libpath)
{
    void* p = dlsym(handle, symbol);
    if (p == nullptr) {
        fatal("VortexGPGPU: dlsym(%s) failed in %s: %s",
              symbol, libpath, dlerror());
    }
    return reinterpret_cast<T>(p);
}

// For the additive trace ABI: absence is a valid outcome (older library),
// so return nullptr instead of fatal-ing. dlerror() is cleared so a later
// failed dlsym_or_fatal reports its own error, not this one.
template <typename T>
T dlsym_optional(void* handle, const char* symbol)
{
    void* p = dlsym(handle, symbol);
    if (p == nullptr) {
        dlerror();
    }
    return reinterpret_cast<T>(p);
}

} // namespace

VortexGPGPU::VortexGPGPU(const Params &p)
  : DmaDevice(p),
    libHandle_(nullptr),
    deviceHandle_(nullptr),
    abi_{},
    traceAbi_{},
    libraryPath_(p.library),
    kernelPath_(p.kernel),
    pioAddr_(p.pio_addr),
    pioSize_(p.pio_size),
    pinAddr_(p.pin_addr),
    pinSize_(p.pin_size),
    pioLatency_(p.pio_latency),
    timingMemory_(p.timing_memory),
    exitOnFirstPio_(p.exit_on_first_pio),
    traceEnable_(p.trace_enable),
    traceAddrOffset_(p.trace_addr_offset),
    cpTickEvent_([this]{ this->cpTick(); }, name() + ".cpTickEvent"),
    vortexTickEvent_([this]{ this->vortexTick(); }, name() + ".vortexTickEvent"),
    standalone_(false),
    traceActive_(false),
    dmaDoneEvent_([this]{ this->dmaComplete(); }, name() + ".dmaDone")
{
    if (libraryPath_.empty()) {
        fatal("VortexGPGPU: 'library' parameter is required "
              "(path to libvortex-gem5.so)");
    }

    libHandle_ = dlopen(libraryPath_.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (libHandle_ == nullptr) {
        fatal("VortexGPGPU: dlopen('%s') failed: %s",
              libraryPath_, dlerror());
    }

    // Resolve the ABI surface. Any missing symbol is a hard build
    // mismatch — fatal at construction rather than mid-simulation.
    abi_.build_info        = dlsym_or_fatal<const char*(*)(void)>
                              (libHandle_, "vortex_gem5_build_info",        libraryPath_.c_str());
    abi_.create            = dlsym_or_fatal<void*(*)(void)>
                              (libHandle_, "vortex_gem5_create",            libraryPath_.c_str());
    abi_.destroy           = dlsym_or_fatal<void(*)(void*)>
                              (libHandle_, "vortex_gem5_destroy",           libraryPath_.c_str());
    abi_.set_start_handler = dlsym_or_fatal<void(*)(void*, void(*)(void*), void*)>
                              (libHandle_, "vortex_gem5_set_start_handler", libraryPath_.c_str());
    abi_.load_kernel       = dlsym_or_fatal<int(*)(void*, const char*)>
                              (libHandle_, "vortex_gem5_load_kernel",       libraryPath_.c_str());
    abi_.cp_mmio_write     = dlsym_or_fatal<void(*)(void*, uint32_t, uint32_t)>
                              (libHandle_, "vortex_gem5_cp_mmio_write",     libraryPath_.c_str());
    abi_.cp_mmio_read      = dlsym_or_fatal<uint32_t(*)(void*, uint32_t)>
                              (libHandle_, "vortex_gem5_cp_mmio_read",      libraryPath_.c_str());
    abi_.cp_tick           = dlsym_or_fatal<bool(*)(void*)>
                              (libHandle_, "vortex_gem5_cp_tick",           libraryPath_.c_str());
    abi_.cp_has_work       = dlsym_or_fatal<bool(*)(void*)>
                              (libHandle_, "vortex_gem5_cp_has_work",       libraryPath_.c_str());
    abi_.vortex_tick       = dlsym_or_fatal<bool(*)(void*)>
                              (libHandle_, "vortex_gem5_vortex_tick",       libraryPath_.c_str());
    abi_.vortex_busy       = dlsym_or_fatal<bool(*)(void*)>
                              (libHandle_, "vortex_gem5_vortex_busy",       libraryPath_.c_str());
    abi_.vram_write        = dlsym_or_fatal<void(*)(void*, uint64_t, const uint8_t*, uint32_t)>
                              (libHandle_, "vortex_gem5_vram_write",        libraryPath_.c_str());
    abi_.vram_read         = dlsym_or_fatal<void(*)(void*, uint64_t, uint8_t*, uint32_t)>
                              (libHandle_, "vortex_gem5_vram_read",         libraryPath_.c_str());
    abi_.set_memory_backend = dlsym_or_fatal<void(*)(
                              void*,
                              void(*)(void*, uint64_t, uint8_t*, uint32_t),
                              void(*)(void*, uint64_t, const uint8_t*, uint32_t),
                              void(*)(void*, uint64_t, bool, const uint8_t*, uint32_t),
                              void*)>
                              (libHandle_, "vortex_gem5_set_memory_backend", libraryPath_.c_str());
    abi_.set_core_timing_backend = dlsym_or_fatal<void(*)(
                              void*,
                              bool(*)(void*, uint64_t, uint64_t, bool,
                                     const uint8_t*, uint64_t, uint32_t),
                              void*)>
                              (libHandle_, "vortex_gem5_set_core_timing_backend", libraryPath_.c_str());
    abi_.complete_core_memory = dlsym_or_fatal<void(*)(
                              void*, uint64_t, const uint8_t*, uint32_t)>
                              (libHandle_, "vortex_gem5_complete_core_memory", libraryPath_.c_str());

    // Additive trace ABI — optional by design; see AbiTrace in the header.
    traceAbi_.open    = dlsym_optional<int(*)(void*, uint64_t(*)(void*), void*, int64_t)>
                          (libHandle_, "vortex_gem5_trace_open");
    traceAbi_.close   = dlsym_optional<void(*)(void*)>
                          (libHandle_, "vortex_gem5_trace_close");
    traceAbi_.emitted = dlsym_optional<uint64_t(*)(void*)>
                          (libHandle_, "vortex_gem5_trace_emitted");

    inform("VortexGPGPU: %s", abi_.build_info());
    inform("VortexGPGPU: library=%s", libraryPath_);
    inform("VortexGPGPU: pio[CP regfile]=[0x%llx,+0x%llx)",
           static_cast<unsigned long long>(pioAddr_),
           static_cast<unsigned long long>(pioSize_));
    if (pinSize_ != 0) {
        inform("VortexGPGPU: pin[BAR-mapped VRAM]=[0x%llx,+0x%llx)",
               static_cast<unsigned long long>(pinAddr_),
               static_cast<unsigned long long>(pinSize_));
    }

    deviceHandle_ = abi_.create();
    if (deviceHandle_ == nullptr) {
        fatal("VortexGPGPU: vortex_gem5_create returned NULL");
    }

    // Register the vortex_start trampoline so the CP can schedule
    // Vortex ticks from inside cp_tick.
    abi_.set_start_handler(deviceHandle_, &VortexGPGPU::onVortexStartTrampoline, this);
}

VortexGPGPU::~VortexGPGPU()
{
    // Before destroy(): the tap lives inside the device object.
    closeTrace();
    if (deviceHandle_ != nullptr && abi_.destroy != nullptr) {
        abi_.destroy(deviceHandle_);
    }
    if (libHandle_ != nullptr) {
        dlclose(libHandle_);
    }
}

void
VortexGPGPU::init()
{
    DmaDevice::init();
}

// ---- Shared timing-memory bridge ------------------------------------------

void
VortexGPGPU::memoryReadTrampoline(void* ctx, uint64_t addr, uint8_t* dst,
                                  uint32_t size)
{
    static_cast<VortexGPGPU*>(ctx)->memoryRead(addr, dst, size);
}

void
VortexGPGPU::memoryWriteTrampoline(void* ctx, uint64_t addr,
                                   const uint8_t* src, uint32_t size)
{
    static_cast<VortexGPGPU*>(ctx)->memoryWrite(addr, src, size);
}

void
VortexGPGPU::cpTimingTrampoline(void* ctx, uint64_t addr, bool is_write,
                                const uint8_t* src, uint32_t size)
{
    static_cast<VortexGPGPU*>(ctx)->enqueueCpTiming(
        addr, is_write, src, size);
}

bool
VortexGPGPU::coreTimingTrampoline(void* ctx, uint64_t token, uint64_t addr,
                                  bool is_write, const uint8_t* src,
                                  uint64_t byteen, uint32_t size)
{
    return static_cast<VortexGPGPU*>(ctx)->issueCoreTiming(
        token, addr, is_write, src, byteen, size);
}

void
VortexGPGPU::memoryRead(uint64_t addr, uint8_t* dst, uint32_t size)
{
    panic_if(addr > pinSize_ || size > pinSize_ - addr,
             "VortexGPGPU CP read outside BAR: addr=%#llx size=%u",
             static_cast<unsigned long long>(addr), size);
    panic_if(!cpYield_, "Vortex timing read outside a CP continuation");
    enqueueCpTiming(addr, false, nullptr, size);
    startNextCpDma();
    (*cpYield_)();
    panic_if(dmaData_.size() != size, "Vortex CP response size mismatch");
    std::copy(dmaData_.begin(), dmaData_.end(), dst);
}

void
VortexGPGPU::memoryWrite(uint64_t addr, const uint8_t* src, uint32_t size)
{
    panic_if(addr > pinSize_ || size > pinSize_ - addr,
             "VortexGPGPU CP write outside BAR: addr=%#llx size=%u",
             static_cast<unsigned long long>(addr), size);
    panic_if(!cpYield_, "Vortex timing write outside a CP continuation");
    enqueueCpTiming(addr, true, src, size);
    startNextCpDma();
    (*cpYield_)();
}

void
VortexGPGPU::enqueueCpTiming(uint64_t addr, bool is_write,
                             const uint8_t* src, uint32_t size)
{
    CpDmaRequest request{pinAddr_ + addr, is_write, {}, size, curTick()};
    if (is_write) {
        panic_if(src == nullptr, "VortexGPGPU CP timing write has no data");
        request.data.assign(src, src + size);
        ++cpTimingWrites_;
    } else {
        ++cpTimingReads_;
    }
    cpDmaQueue_.push_back(std::move(request));
}

bool
VortexGPGPU::issueCoreTiming(uint64_t token, uint64_t addr, bool is_write,
                             const uint8_t* src, uint64_t byteen,
                             uint32_t size)
{
    panic_if(addr > pinSize_ || size > pinSize_ - addr,
             "VortexGPGPU core access outside BAR: addr=%#llx size=%u",
             static_cast<unsigned long long>(addr), size);
    panic_if(size == 0 || size > 64,
             "VortexGPGPU core timing size %u is outside [1,64]", size);
    panic_if(token > std::numeric_limits<uint32_t>::max(),
             "VortexGPGPU core token exceeds gem5 stream-ID width");
    panic_if(coreDmas_.count(token) != 0,
             "VortexGPGPU duplicate outstanding core token %llu",
             static_cast<unsigned long long>(token));

    auto request = std::make_unique<CoreDmaRequest>();
    request->token = token;
    request->addr = pinAddr_ + addr;
    request->isWrite = is_write;
    request->size = size;
    request->issueTick = curTick();
    request->data.resize(size);
    if (is_write) {
        panic_if(src == nullptr, "VortexGPGPU core timing write has no data");
        ++coreTimingWrites_;
        std::copy_n(src, size, request->data.begin());
        request->byteEnable.resize(size);
        for (uint32_t byte = 0; byte < size; ++byte)
            request->byteEnable[byte] =
                (byteen & (uint64_t(1) << byte)) != 0;
    } else {
        ++coreTimingReads_;
    }

    const Addr physical_addr = request->addr;
    uint8_t *data = request->data.data();
    const auto &byte_enable = request->byteEnable;
    coreDmas_.emplace(token, std::move(request));
    maxCoreOutstanding_ = std::max<uint64_t>(maxCoreOutstanding_,
                                             coreDmas_.size());

    auto *done = new EventFunctionWrapper(
        [this, token] { coreDmaComplete(token); },
        name() + ".coreDmaDone", true);
    const uint32_t source_token = static_cast<uint32_t>(token);
    if (is_write) {
        dmaWrite(physical_addr, size, done, data, byte_enable,
                 source_token, source_token);
    } else {
        dmaRead(physical_addr, size, done, data,
                source_token, source_token);
    }
    return true;
}

void
VortexGPGPU::coreDmaComplete(uint64_t token)
{
    auto found = coreDmas_.find(token);
    panic_if(found == coreDmas_.end(),
             "VortexGPGPU completion for unknown core token %llu",
             static_cast<unsigned long long>(token));
    std::unique_ptr<CoreDmaRequest> request = std::move(found->second);
    coreDmas_.erase(found);

    const Tick observed = curTick() - request->issueTick;
    timingLatencyTotal_ += observed;
    timingLatencyMax_ = std::max(timingLatencyMax_, observed);
    ++timingCompletions_;
    abi_.complete_core_memory(deviceHandle_, request->token,
                              request->isWrite ? nullptr
                                               : request->data.data(),
                              request->isWrite ? 0 : request->size);
}

void
VortexGPGPU::startNextCpDma()
{
    if (dmaKind_ != DmaKind::None || cpDmaQueue_.empty())
        return;
    activeCpDma_ = std::make_unique<CpDmaRequest>(
        std::move(cpDmaQueue_.front()));
    cpDmaQueue_.pop_front();
    if (activeCpDma_->isWrite) {
        dmaKind_ = DmaKind::CpWrite;
        dmaWrite(activeCpDma_->addr, activeCpDma_->size, &dmaDoneEvent_,
                 activeCpDma_->data.data());
    } else {
        dmaKind_ = DmaKind::CpRead;
        dmaData_.assign(activeCpDma_->size, 0);
        dmaRead(activeCpDma_->addr, activeCpDma_->size, &dmaDoneEvent_,
                dmaData_.data());
    }
}

void
VortexGPGPU::dmaComplete()
{
    if (dmaKind_ == DmaKind::CpRead || dmaKind_ == DmaKind::CpWrite) {
        const Tick observed = curTick() - activeCpDma_->issueTick;
        timingLatencyTotal_ += observed;
        timingLatencyMax_ = std::max(timingLatencyMax_, observed);
        ++timingCompletions_;
        activeCpDma_.reset();
        dmaKind_ = DmaKind::None;
        startNextCpDma();
        if (cpCoroutine_ && !cpCoroutine_->finished())
            cpResumeNeeded_ = true;
        resumeCpIfReady();
        return;
    }
    panic("VortexGPGPU unexpected DMA completion");
}

void
VortexGPGPU::resumeCpIfReady()
{
    if (activeCpDma_ || !cpDmaQueue_.empty())
        return;
    startVortexIfReady();
    if (!cpResumeNeeded_)
        return;
    cpResumeNeeded_ = false;
    if (((cpCoroutine_ && !cpCoroutine_->finished()) || abi_.cp_has_work(deviceHandle_)) && !cpTickEvent_.scheduled())
        schedule(cpTickEvent_, clockEdge(Cycles(1)));
}

void
VortexGPGPU::startVortexIfReady()
{
    if (!vortexStartPending_ || activeCpDma_ || !cpDmaQueue_.empty())
        return;
    vortexStartPending_ = false;
    if (!vortexTickEvent_.scheduled())
        schedule(vortexTickEvent_, clockEdge(Cycles(1)));
}

uint64_t
VortexGPGPU::curTickTrampoline(void* ctx)
{
    // ctx is unused: curTick() is the global event-queue clock and there is
    // exactly one. The parameter is kept so the library's tick-provider
    // signature stays generic across the three trace sources.
    (void)ctx;
    return static_cast<uint64_t>(curTick());
}

void
VortexGPGPU::openTrace()
{
    if (!traceEnable_ || traceAbi_.open == nullptr) {
        // trace_enable=false, or the library predates the trace ABI.
        return;
    }
    const int rc = traceAbi_.open(deviceHandle_, &VortexGPGPU::curTickTrampoline,
                                  this, traceAddrOffset_);
    if (rc != 0) {
        // Overwhelmingly the normal case: HETTRACE_DIR is simply not set.
        // Not an error, and not worth an inform() on every run.
        return;
    }
    traceActive_ = true;
    inform("VortexGPGPU: memory trace active (post-LLC tap, addr_offset=%lld)",
           static_cast<long long>(traceAddrOffset_));

}

void
VortexGPGPU::closeTrace()
{
    if (traceActive_) {
        traceActive_ = false;
        const uint64_t n = (traceAbi_.emitted != nullptr)
                               ? traceAbi_.emitted(deviceHandle_) : 0;
        if (traceAbi_.close != nullptr) {
            traceAbi_.close(deviceHandle_);
        }
        inform("VortexGPGPU: memory trace closed, %llu records",
               static_cast<unsigned long long>(n));
    }
    if (timingMemory_ && !timingSummaryPrinted_) {
        timingSummaryPrinted_ = true;
        const Tick average = timingCompletions_ == 0 ? 0
            : timingLatencyTotal_ / timingCompletions_;
        inform("VortexGPGPU timing summary: core_read=%llu core_write=%llu "
               "cp_read=%llu cp_write=%llu completed=%llu "
               "latency_avg=%llu ticks latency_max=%llu ticks "
               "cp_cycles=%llu vortex_cycles=%llu",
               static_cast<unsigned long long>(coreTimingReads_),
               static_cast<unsigned long long>(coreTimingWrites_),
               static_cast<unsigned long long>(cpTimingReads_),
               static_cast<unsigned long long>(cpTimingWrites_),
               static_cast<unsigned long long>(timingCompletions_),
               static_cast<unsigned long long>(average),
               static_cast<unsigned long long>(timingLatencyMax_),
               static_cast<unsigned long long>(cpCycles_),
               static_cast<unsigned long long>(vortexCycles_));
    }
}

void
VortexGPGPU::startup()
{
    DmaDevice::startup();

    // gem5 may keep SimObjects alive past exit. Register independently of
    // tracing so the timing summary and any buffered trace always close.
    registerExitCallback([this]{ this->closeTrace(); });

    if (timingMemory_) {
        fatal_if(!kernelPath_.empty(),
                 "VortexGPGPU timing_memory currently requires hosted mode");
        abi_.set_memory_backend(deviceHandle_,
                                &VortexGPGPU::memoryReadTrampoline,
                                &VortexGPGPU::memoryWriteTrampoline,
                                nullptr, this);
        abi_.set_core_timing_backend(deviceHandle_,
                                     &VortexGPGPU::coreTimingTrampoline, this);
        inform("VortexGPGPU: core + CP DMA routed through gem5 timing memory");
    }

    // Before any ticks are scheduled, so no request can slip past the tap.
    openTrace();

    if (!kernelPath_.empty()) {
        // Standalone mode: preload a kernel and self-drive to completion.
        // No host CPU involvement; used as a smoke test for the device library.
        inform("VortexGPGPU: standalone mode (preload + auto-tick)");
        inform("VortexGPGPU: preloading kernel=%s", kernelPath_);
        if (abi_.load_kernel(deviceHandle_, kernelPath_.c_str()) != 0) {
            fatal("VortexGPGPU: vortex_gem5_load_kernel('%s') failed",
                  kernelPath_);
        }
        standalone_ = true;
        schedule(vortexTickEvent_, clockEdge(Cycles(1)));
    } else {
        // Hosted mode: the host runtime issues CP MMIO writes to configure
        // queues and commit commands; the CP schedules ticks via maybeWakeCp()
        // and the vortex tick via the start handler. Idle at boot.
        inform("VortexGPGPU: hosted mode (waiting for CP enable)");
        standalone_ = false;
    }
}

void
VortexGPGPU::cpTick()
{
    if (!timingMemory_) {
        ++cpCycles_;
        if (abi_.cp_tick(deviceHandle_))
            schedule(cpTickEvent_, clockEdge(Cycles(1)));
        return;
    }
    if (!cpCoroutine_ || cpCoroutine_->finished()) {
        ++cpCycles_;
        cpCoroutine_ = std::make_unique<CpCoroutine>(
            [this](CpCoroutine::CallerType& yield) {
                cpYield_ = &yield;
                cpBusy_ = abi_.cp_tick(deviceHandle_);
                cpYield_ = nullptr;
            }, false);
    }
    (*cpCoroutine_)();
    if (!cpCoroutine_->finished())
        return;
    cpResumeNeeded_ = false;
    if (cpBusy_)
        schedule(cpTickEvent_, clockEdge(Cycles(1)));

    // Idle drop-out: no reschedule. PIO writes that arm new work will
    // call maybeWakeCp() and reschedule us.
}

void
VortexGPGPU::vortexTick()
{
    ++vortexCycles_;
    const bool still_running = abi_.vortex_tick(deviceHandle_);
    if (still_running) {
        schedule(vortexTickEvent_, clockEdge(Cycles(1)));
        return;
    }
    if (standalone_) {
        inform("VortexGPGPU: standalone kernel complete — exiting sim loop");
        closeTrace();
        exitSimLoop("VortexGPGPU: kernel complete");
        return;
    }
    // Hosted mode: Vortex finished. The CP's launch FSM observes
    // vortex_busy() == false on its next tick and retires the
    // CMD_LAUNCH. If the CP is already idle (no scheduled tick) we
    // need to wake it so the retirement actually happens.
    maybeWakeCp();
}

void
VortexGPGPU::maybeWakeCp()
{
    if (abi_.cp_has_work(deviceHandle_) && !activeCpDma_
        && cpDmaQueue_.empty() && !cpTickEvent_.scheduled()) {
        schedule(cpTickEvent_, clockEdge(Cycles(1)));
    }
}

void
VortexGPGPU::onVortexStartTrampoline(void* ctx)
{
    static_cast<VortexGPGPU*>(ctx)->onVortexStart();
}

void
VortexGPGPU::onVortexStart()
{
    // QMD/draw setup can issue several CP reads in this same CP tick. Do not
    // let the core consume their data until those timing requests complete.
    vortexStartPending_ = true;
    startVortexIfReady();
}

Tick
VortexGPGPU::read(PacketPtr pkt)
{
    const Addr a = pkt->getAddr();
    if (a >= pioAddr_ && a < pioAddr_ + pioSize_) {
        // CP regfile access — 32-bit only.
        const uint32_t off = uint32_t(a - pioAddr_);
        const uint32_t value = abi_.cp_mmio_read(deviceHandle_, off);
        pkt->setUintX(static_cast<uint64_t>(value), ByteOrder::little);
        pkt->makeAtomicResponse();
        return pioLatency_;
    }
    // BAR-mapped VRAM access (CPU is reading device memory directly).
    // Variable-width packet (host load / cache-line fill).
    panic_if(timingMemory_,
             "VortexGPGPU received BAR PIO read while timing_memory owns BAR");
    const uint64_t dev_addr = a - pinAddr_;
    abi_.vram_read(deviceHandle_,
                   dev_addr,
                   pkt->getPtr<uint8_t>(),
                   uint32_t(pkt->getSize()));
    pkt->makeAtomicResponse();
    return pioLatency_;
}

Tick
VortexGPGPU::write(PacketPtr pkt)
{
    const Addr a = pkt->getAddr();
    if (a >= pioAddr_ && a < pioAddr_ + pioSize_) {
        // CP regfile write — 32-bit only.
        const uint32_t off = uint32_t(a - pioAddr_);
        const uint64_t raw = pkt->getUintX(ByteOrder::little);
        abi_.cp_mmio_write(deviceHandle_, off, uint32_t(raw));
        maybeWakeCp();
        pkt->makeAtomicResponse();
        if (exitOnFirstPio_ && !timingPhaseExitFired_) {
            timingPhaseExitFired_ = true;
            exitSimLoop("VortexGPGPU: timing phase");
        }
        return pioLatency_;
    }
    // BAR-mapped VRAM write — variable-width packet (host store /
    // cache writeback). Subsequent device reads at the same address
    // see the bytes written here.
    panic_if(timingMemory_,
             "VortexGPGPU received BAR PIO write while timing_memory owns BAR");
    const uint64_t dev_addr = a - pinAddr_;
    abi_.vram_write(deviceHandle_,
                    dev_addr,
                    pkt->getConstPtr<uint8_t>(),
                    uint32_t(pkt->getSize()));
    // Writes to device VRAM may seed CP ring entries; if the CP is
    // dormant, leave it dormant (the CP only wakes on a doorbell PIO
    // write, not on a ring-fill).
    pkt->makeAtomicResponse();
    return pioLatency_;
}

AddrRangeList
VortexGPGPU::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(RangeSize(pioAddr_, pioSize_));
    if (pinSize_ != 0 && !timingMemory_) {
        ranges.push_back(RangeSize(pinAddr_, pinSize_));
    }
    return ranges;
}

} // namespace gem5
