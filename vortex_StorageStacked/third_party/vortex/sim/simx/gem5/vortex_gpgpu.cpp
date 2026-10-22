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

#include "vortex_gpgpu.h"

#include "constants.h"
#include "dev_mem.h"
#include "processor.h"
#include "vortex_trace.h"
#include <cmd_processor.h>
#include <mem.h>
#include <util.h>
#include <VX_config.h>
#include <VX_types.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using namespace vortex;

// Matches GLOBAL_MEM_SIZE from the host runtime for vram_{read,write} bounds checks.
// Inlined to avoid pulling in the full runtime ABI.
#if (VX_CFG_XLEN == 64)
static constexpr uint64_t GEM5_GLOBAL_MEM_SIZE = 0x200000000ull;  // 8 GB
#else
static constexpr uint64_t GEM5_GLOBAL_MEM_SIZE = 0x100000000ull;  // 4 GB
#endif

namespace {

// Gem5Device — owns the Vortex Processor + RAM + CommandProcessor.
// The CP's hooks call back into proc_/dev_mem_, and the SimObject
// drives cp_tick / vortex_tick on independent gem5 events.
class Gem5Device {
public:
    Gem5Device()
        : ram_(0, VX_VM_PAGE_SIZE),
          proc_(std::make_unique<Processor>()),
          dev_mem_(std::make_unique<vortex_gem5::InProcessDevMem>(ram_)),
          cp_(make_cp_hooks()) {
        proc_->attach_ram(&ram_);
    }

    ~Gem5Device() = default;

    // ---------------- Standalone kernel preload -------------------------
    // Primes the KMU DCRs for a 1×1×1 CTA and loads the ELF/bin/hex into VRAM.
    // vortex_tick then drives execution to completion (ProcessorImpl::cycle
    // does lazy init and calls kmu_->start() on first tick).
    // The CP-driven path never calls this — kernels land via mem_upload
    // and KMU programming goes through CMD_DCR_*.
    bool load_kernel(const std::string& path) {
        const uint64_t startup_addr = 0x80000000;  // flat-image (vxbin/bin/hex) load address; the ELF path uses img.entry
        proc_->dcr_write(VX_DCR_KMU_STARTUP_ADDR0, startup_addr & 0xffffffff);
    #if (VX_CFG_XLEN == 64)
        proc_->dcr_write(VX_DCR_KMU_STARTUP_ADDR1, startup_addr >> 32);
    #endif
        proc_->dcr_write(VX_DCR_KMU_STARTUP_ARG0, 0);
        proc_->dcr_write(VX_DCR_KMU_STARTUP_ARG1, 0);
        proc_->dcr_write(VX_DCR_KMU_GRID_DIM_X,   1);
        proc_->dcr_write(VX_DCR_KMU_GRID_DIM_Y,   1);
        proc_->dcr_write(VX_DCR_KMU_GRID_DIM_Z,   1);
        proc_->dcr_write(VX_DCR_KMU_BLOCK_DIM_X,  1);
        proc_->dcr_write(VX_DCR_KMU_BLOCK_DIM_Y,  1);
        proc_->dcr_write(VX_DCR_KMU_BLOCK_DIM_Z,  1);
        proc_->dcr_write(VX_DCR_KMU_LMEM_SIZE,    0);
        proc_->dcr_write(VX_DCR_KMU_BLOCK_SIZE,   1);
        proc_->dcr_write(VX_DCR_KMU_WARP_STEP_X,  VX_CFG_NUM_THREADS);
        proc_->dcr_write(VX_DCR_KMU_WARP_STEP_Y,  0);
        proc_->dcr_write(VX_DCR_KMU_WARP_STEP_Z,  0);

        std::string ext(fileExtension(path.c_str()));
        if (ext == "vxbin") {
            ram_.loadVxImage(path.c_str());
        } else if (ext == "bin") {
            ram_.loadBinImage(path.c_str(), startup_addr);
        } else if (ext == "hex") {
            ram_.loadHexImage(path.c_str());
        } else {
            std::cerr << "vortex_gem5: unsupported kernel extension '" << ext
                      << "' (need .vxbin, .bin, or .hex)" << std::endl;
            return false;
        }
        // Mark device running; the SimObject advances vortexTickEvent_
        // until cycle() reports done (hosted launches use vortex_start instead).
        vortex_running_ = true;
        return true;
    }

    // ---------------- VRAM direct access --------------------------------
    void vram_write(uint64_t addr, const uint8_t* src, uint32_t size) {
        if (addr + size > GEM5_GLOBAL_MEM_SIZE) {
        #ifndef NDEBUG
            std::cerr << "vortex_gem5: vram_write overflow addr=0x"
                      << std::hex << addr << " size=" << std::dec << size
                      << std::endl;
        #endif
            return;
        }
        if (mem_write_fn_)
            mem_write_fn_(mem_ctx_, addr, src, size);
        else
            dev_mem_->write(addr, src, size);
    }
    void vram_read(uint64_t addr, uint8_t* dst, uint32_t size) {
        if (addr + size > GEM5_GLOBAL_MEM_SIZE) {
        #ifndef NDEBUG
            std::cerr << "vortex_gem5: vram_read overflow addr=0x"
                      << std::hex << addr << " size=" << std::dec << size
                      << std::endl;
        #endif
            return;
        }
        if (mem_read_fn_)
            mem_read_fn_(mem_ctx_, addr, dst, size);
        else
            dev_mem_->read(addr, dst, size);
    }

    // ---------------- CP regfile MMIO -----------------------------------
    // The SimObject's PIO handlers route MMIO reads/writes here.
    // CP regfile is 32-bit; address map is in sim/common/cmd_processor.h.
    void cp_mmio_write(uint32_t off, uint32_t value) { cp_.mmio_write(off, value); }
    uint32_t cp_mmio_read (uint32_t off) const       { return cp_.mmio_read(off); }

    // ---------------- CP tick / introspection ---------------------------
    // Advances the CP one functional cycle; returns true while the CP has work.
    // The SimObject reschedules cpTickEvent_ while true, sleeps otherwise.
    bool cp_tick() {
        cp_.tick();
        return cp_.busy();
    }
    bool cp_has_work() const { return cp_.enabled() && cp_.busy(); }

    // ---------------- Vortex tick / introspection -----------------------
    // Advances ProcessorImpl::cycle() one step. cycle() does lazy init
    // (resets SimPlatform + calls kmu_->start()) on first call.
    // For back-to-back launches, the CP's vortex_start hook calls
    // start_kmu() explicitly to re-arm the KMU (kmu_->start is idempotent).
    bool vortex_tick() {
        bool still_running = proc_->cycle();
        if (!still_running) {
            vortex_running_ = false;
        }
        return vortex_running_;
    }
    bool vortex_busy() const { return vortex_running_; }

    // ---------------- vortex_start handler registration -----------------
    // The CP fires this callback on CMD_LAUNCH retirement. The SimObject
    // uses it to schedule vortexTickEvent_, decoupling CP and Vortex tick chains.
    void set_start_handler(vortex_gem5_start_handler_t fn, void* ctx) {
        start_fn_  = fn;
        start_ctx_ = ctx;
    }

    void set_memory_backend(vortex_gem5_mem_read_t read_fn,
                            vortex_gem5_mem_write_t write_fn,
                            vortex_gem5_cp_timing_t timing_fn, void* ctx) {
        mem_read_fn_ = read_fn;
        mem_write_fn_ = write_fn;
        cp_timing_fn_ = timing_fn;
        mem_ctx_ = ctx;
    }

    void set_core_timing_backend(vortex_gem5_core_timing_t issue_fn,
                                 void* ctx) {
        core_timing_fn_ = issue_fn;
        core_timing_ctx_ = ctx;
        if (issue_fn == nullptr) {
            proc_->set_mem_timing_hook(nullptr);
            return;
        }
        proc_->set_mem_timing_hook(
            [this](uint64_t token, const MemReq& request,
                   const uint8_t* write_data, uint64_t byteen,
                   uint32_t size) -> bool {
                return core_timing_fn_(core_timing_ctx_, token, request.addr,
                                       request.is_write(), write_data,
                                       byteen, size);
            });
    }

    void complete_core_memory(uint64_t token, const uint8_t* read_data,
                              uint32_t size) {
        proc_->complete_mem_timing(token, read_data, size);
    }

    // ---------------- Memory-access trace tap ---------------------------
    // Read-only observer on the post-LLC request stream. Installing it
    // does not change simulation behaviour, so a failure to open the
    // trace is reported but never fatal.
    bool trace_open(vortex_gem5_tick_provider_t fn, void* ctx,
                    int64_t addr_offset) {
        return trace_.Install(proc_.get(), fn, ctx, addr_offset);
    }
    void trace_close() { trace_.Close(); }
    uint64_t trace_emitted() const {
        return trace_.is_open() ? trace_.stats().emitted : 0;
    }

private:
    // Declared before proc_ on purpose: members are destroyed in reverse
    // declaration order, so the tap outlives the Processor whose telemetry
    // hook holds a pointer to it.
    vortex_gem5::VortexTraceTap trace_;

    vortex::CommandProcessor::Hooks make_cp_hooks() {
        vortex::CommandProcessor::Hooks h;
        h.dram_read = [this](uint64_t addr, void* dst, std::size_t bytes) {
            if (mem_read_fn_)
                mem_read_fn_(mem_ctx_, addr, static_cast<uint8_t*>(dst), bytes);
            else
                dev_mem_->read(addr, dst, bytes);
            // CP 的 DMA 不经过 vortex::Memory，pre_send hook 看不到它 —— 但它是
            // 真实的设备内存流量（镜像上传 + 载荷中转），而且是把 host 的暂存区
            // 与设备缓冲连起来的那一步。详见 vortex_trace.h 的 OnDma。
            trace_.OnDma(addr, bytes, hettrace::kRead);
            if (cp_timing_fn_)
                cp_timing_fn_(mem_ctx_, addr, false, nullptr, bytes);
        };
        h.dram_write = [this](uint64_t addr, const void* src, std::size_t bytes) {
            // With a timing observer installed, that transaction is the
            // authoritative write. Applying the functional write here too
            // would expose data before gem5 accepts the timing DMA request.
            if (mem_write_fn_ && !cp_timing_fn_)
                mem_write_fn_(mem_ctx_, addr,
                              static_cast<const uint8_t*>(src), bytes);
            else
                dev_mem_->write(addr, src, bytes);
            trace_.OnDma(addr, bytes, hettrace::kWrite);
            if (cp_timing_fn_)
                cp_timing_fn_(mem_ctx_, addr, true,
                              static_cast<const uint8_t*>(src), bytes);
        };
        h.vortex_dcr_write = [this](uint32_t addr, uint32_t value) {
            proc_->dcr_write(addr, value);
        };
        h.vortex_dcr_read = [this](uint32_t addr, uint32_t tag) -> uint32_t {
            uint32_t v = 0;
            proc_->dcr_read(addr, tag, &v);
            return v;
        };
        h.vortex_start = [this]() {
            // Mark Vortex as in-flight, re-arm the KMU, and notify the SimObject.
            vortex_running_ = true;
            proc_->start_kmu();
            if (start_fn_) start_fn_(start_ctx_);
        };
        h.vortex_busy = [this]() -> bool { return vortex_running_; };
        return h;
    }

    RAM ram_;
    std::unique_ptr<Processor> proc_;
    std::unique_ptr<vortex_gem5::DevMemAccessor> dev_mem_;
    vortex::CommandProcessor cp_;
    bool vortex_running_ = false;
    vortex_gem5_start_handler_t start_fn_  = nullptr;
    void* start_ctx_ = nullptr;
    vortex_gem5_mem_read_t mem_read_fn_ = nullptr;
    vortex_gem5_mem_write_t mem_write_fn_ = nullptr;
    vortex_gem5_cp_timing_t cp_timing_fn_ = nullptr;
    void* mem_ctx_ = nullptr;
    vortex_gem5_core_timing_t core_timing_fn_ = nullptr;
    void* core_timing_ctx_ = nullptr;
};

} // namespace

// ----- C ABI -----------------------------------------------------------------

extern "C" {

const char* vortex_gem5_build_info(void) {
    static char info[384];
    std::snprintf(info, sizeof(info),
                  "vortex-gem5 (XLEN=%d, threads=%d, warps=%d, cores=%d, clusters=%d) "
                  "cache(ICACHE=%d, DCACHE=%d, L2_ENABLED=%d, L2_SIZE=%d) "
                  "cache_geometry(I_WAYS=%d, D_WAYS=%d, L2_WAYS=%d, I_CYCLES=%d, D_CYCLES=%d, L2_CYCLES=%d)",
                  VX_CFG_XLEN, VX_CFG_NUM_THREADS, VX_CFG_NUM_WARPS, VX_CFG_NUM_CORES, VX_CFG_NUM_CLUSTERS,
                  VX_CFG_ICACHE_SIZE, VX_CFG_DCACHE_SIZE, VX_CFG_L2_ENABLED, VX_CFG_L2_SIZE,
                  VX_CFG_ICACHE_NUM_WAYS, VX_CFG_DCACHE_NUM_WAYS, VX_CFG_L2_NUM_WAYS,
                  VX_CFG_ICACHE_LATENCY, VX_CFG_DCACHE_LATENCY, VX_CFG_L2_LATENCY);
    return info;
}

vortex_gem5_handle_t vortex_gem5_create(void) {
    try {
        return reinterpret_cast<vortex_gem5_handle_t>(new Gem5Device());
    } catch (const std::exception& e) {
        std::cerr << "vortex_gem5_create: " << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cerr << "vortex_gem5_create: unknown exception" << std::endl;
        return nullptr;
    }
}

void vortex_gem5_destroy(vortex_gem5_handle_t h) {
    if (h == nullptr) return;
    delete reinterpret_cast<Gem5Device*>(h);
}

void vortex_gem5_set_start_handler(vortex_gem5_handle_t h,
                                   vortex_gem5_start_handler_t fn,
                                   void* ctx) {
    if (h == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->set_start_handler(fn, ctx);
}

int vortex_gem5_load_kernel(vortex_gem5_handle_t h, const char* path) {
    if (h == nullptr || path == nullptr) return -1;
    return reinterpret_cast<Gem5Device*>(h)->load_kernel(path) ? 0 : -1;
}

void vortex_gem5_cp_mmio_write(vortex_gem5_handle_t h,
                               uint32_t off, uint32_t value) {
    if (h == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->cp_mmio_write(off, value);
}

uint32_t vortex_gem5_cp_mmio_read(vortex_gem5_handle_t h, uint32_t off) {
    if (h == nullptr) return 0;
    return reinterpret_cast<Gem5Device*>(h)->cp_mmio_read(off);
}

bool vortex_gem5_cp_tick(vortex_gem5_handle_t h) {
    if (h == nullptr) return false;
    return reinterpret_cast<Gem5Device*>(h)->cp_tick();
}

bool vortex_gem5_cp_has_work(vortex_gem5_handle_t h) {
    if (h == nullptr) return false;
    return reinterpret_cast<Gem5Device*>(h)->cp_has_work();
}

bool vortex_gem5_vortex_tick(vortex_gem5_handle_t h) {
    if (h == nullptr) return false;
    return reinterpret_cast<Gem5Device*>(h)->vortex_tick();
}

bool vortex_gem5_vortex_busy(vortex_gem5_handle_t h) {
    if (h == nullptr) return false;
    return reinterpret_cast<Gem5Device*>(h)->vortex_busy();
}

void vortex_gem5_vram_write(vortex_gem5_handle_t h,
                            uint64_t dev_addr, const uint8_t* src,
                            uint32_t size) {
    if (h == nullptr || src == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->vram_write(dev_addr, src, size);
}

void vortex_gem5_vram_read(vortex_gem5_handle_t h,
                           uint64_t dev_addr, uint8_t* dst,
                           uint32_t size) {
    if (h == nullptr || dst == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->vram_read(dev_addr, dst, size);
}

void vortex_gem5_set_memory_backend(vortex_gem5_handle_t h,
                                    vortex_gem5_mem_read_t read_fn,
                                    vortex_gem5_mem_write_t write_fn,
                                    vortex_gem5_cp_timing_t timing_fn,
                                    void* ctx) {
    if (h == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->set_memory_backend(
        read_fn, write_fn, timing_fn, ctx);
}

void vortex_gem5_set_core_timing_backend(vortex_gem5_handle_t h,
                                         vortex_gem5_core_timing_t issue_fn,
                                         void* ctx) {
    if (h == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->set_core_timing_backend(issue_fn, ctx);
}

void vortex_gem5_complete_core_memory(vortex_gem5_handle_t h,
                                      uint64_t token,
                                      const uint8_t* read_data,
                                      uint32_t size) {
    if (h == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->complete_core_memory(
        token, read_data, size);
}

int vortex_gem5_trace_open(vortex_gem5_handle_t h,
                           vortex_gem5_tick_provider_t tick_fn,
                           void* tick_ctx,
                           int64_t addr_offset) {
    if (h == nullptr) return -1;
    return reinterpret_cast<Gem5Device*>(h)->trace_open(tick_fn, tick_ctx,
                                                        addr_offset)
               ? 0
               : -1;
}

void vortex_gem5_trace_close(vortex_gem5_handle_t h) {
    if (h == nullptr) return;
    reinterpret_cast<Gem5Device*>(h)->trace_close();
}

uint64_t vortex_gem5_trace_emitted(vortex_gem5_handle_t h) {
    if (h == nullptr) return 0;
    return reinterpret_cast<Gem5Device*>(h)->trace_emitted();
}

} // extern "C"
