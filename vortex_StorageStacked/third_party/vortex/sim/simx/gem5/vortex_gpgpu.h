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

// libvortex-gem5 — C ABI for the gem5 VortexGPGPU SimObject.
//
// The device library hosts a vortex::Processor + vortex::CommandProcessor pair,
// exposes a 32-bit CP MMIO regfile (PIO_BASE_ADDR + 0x0 .. + 0x1FF), and
// provides two independently-tickable engines so the SimObject can drive CP and
// Vortex as separate gem5 event chains:
//
//     cpTickEvent_      -> vortex_gem5_cp_tick()
//     vortexTickEvent_  -> vortex_gem5_vortex_tick()
//
// Both engines self-report whether they still have work via
// vortex_gem5_cp_has_work() / vortex_gem5_vortex_busy(). The CP's vortex_start
// hook calls back into the SimObject via the start-handler registered at
// construction so a CMD_LAUNCH retirement schedules vortexTickEvent_ from
// inside cpTickEvent_'s execution.
//
// The ABI is C so the gem5 side does not depend on SimX's internal types.
//
// Concurrency: all calls are serialized on the gem5 event-loop thread.
// No internal locking. No re-entrancy.

#pragma once

#include <VX_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle. Owns a vortex::Processor, RAM, MemoryAllocator, and
// vortex::CommandProcessor.
typedef struct vortex_gem5_device_s* vortex_gem5_handle_t;

// Returns a printable description of the build config (cores, warps,
// threads, VX_CFG_XLEN). Returned pointer is static; do not free.
const char* vortex_gem5_build_info(void);

// Construct a Vortex device instance. Returns NULL on failure.
// VRAM is allocated lazily; no kernel is loaded until
// vortex_gem5_load_kernel is called.
vortex_gem5_handle_t vortex_gem5_create(void);

// Destroy the device. Safe to call with NULL.
void vortex_gem5_destroy(vortex_gem5_handle_t h);

// Register a callback the device library invokes from inside its CP
// vortex_start hook. The SimObject uses this to schedule its Vortex
// tick event when the CP launches a kernel. Pass NULL to clear.
// `ctx` is forwarded back unchanged.
typedef void (*vortex_gem5_start_handler_t)(void* ctx);
void vortex_gem5_set_start_handler(vortex_gem5_handle_t h,
                                   vortex_gem5_start_handler_t fn,
                                   void* ctx);

// Load a kernel image (.vxbin / .bin / .hex) into VRAM and prime KMU DCRs
// for a 1×1×1 CTA at the standalone load address. In hosted mode the
// dispatcher uploads kernels via mem_upload + CMD_DCR_WRITE through the CP.
// Returns 0 on success, -1 on file-not-found or unsupported format.
int vortex_gem5_load_kernel(vortex_gem5_handle_t h, const char* path);

// CP regfile MMIO. `off` is the CP-internal byte offset (0..0x13F for queue 0).
// All accesses are 32-bit. The gem5 device's PIO range maps directly to the
// CP regfile; the SimObject translates each PIO packet into one of these calls.
void     vortex_gem5_cp_mmio_write(vortex_gem5_handle_t h,
                                   uint32_t off, uint32_t value);
uint32_t vortex_gem5_cp_mmio_read (vortex_gem5_handle_t h, uint32_t off);

// Advance the embedded CommandProcessor by one functional cycle.
// Returns true if the CP has more work (ring non-empty, command in
// flight) and should be ticked again.
bool vortex_gem5_cp_tick(vortex_gem5_handle_t h);

// True iff the CP would benefit from being ticked: enabled and busy.
// The SimObject uses this from PIO write handlers (after a CP regfile
// update may have armed work) to decide whether to schedule
// cpTickEvent_.
bool vortex_gem5_cp_has_work(vortex_gem5_handle_t h);

// Advance the Vortex Processor by one cycle. Returns true while the
// processor is still running (clusters active or channels carrying
// packets); the SimObject's vortexTickEvent_ reschedules itself while
// this returns true and stops otherwise.
bool vortex_gem5_vortex_tick(vortex_gem5_handle_t h);

// True iff Vortex is currently executing a kernel (any cluster
// running, any in-flight memory transactions). Used by the CP's
// vortex_busy hook to know when to retire a CMD_LAUNCH.
bool vortex_gem5_vortex_busy(vortex_gem5_handle_t h);

// Direct device-VRAM access for the SimObject's DMA-path scratch buffers.
void vortex_gem5_vram_write(vortex_gem5_handle_t h,
                            uint64_t dev_addr, const uint8_t* src,
                            uint32_t size);
void vortex_gem5_vram_read (vortex_gem5_handle_t h,
                            uint64_t dev_addr, uint8_t* dst,
                            uint32_t size);

// ---- Shared timing-memory backend -----------------------------------------
// In hosted gem5 mode the BAR bytes live in gem5's unified sparse memory.
// CP reads/writes use the synchronous callbacks for data semantics, and the
// observer emits an equivalent timing transaction; gem5 pauses subsequent CP
// ticks until those transactions complete. Core requests are fully
// asynchronous: complete_core_memory() is what releases the pending MemRsp.
typedef void (*vortex_gem5_mem_read_t)(void* ctx, uint64_t addr,
                                       uint8_t* dst, uint32_t size);
typedef void (*vortex_gem5_mem_write_t)(void* ctx, uint64_t addr,
                                        const uint8_t* src, uint32_t size);
typedef void (*vortex_gem5_cp_timing_t)(void* ctx, uint64_t addr,
                                        bool is_write, const uint8_t* src,
                                        uint32_t size);
void vortex_gem5_set_memory_backend(vortex_gem5_handle_t h,
                                    vortex_gem5_mem_read_t read_fn,
                                    vortex_gem5_mem_write_t write_fn,
                                    vortex_gem5_cp_timing_t timing_fn,
                                    void* ctx);

typedef bool (*vortex_gem5_core_timing_t)(
    void* ctx, uint64_t token, uint64_t addr, bool is_write,
    const uint8_t* src, uint64_t byteen, uint32_t size);
void vortex_gem5_set_core_timing_backend(vortex_gem5_handle_t h,
                                         vortex_gem5_core_timing_t issue_fn,
                                         void* ctx);
void vortex_gem5_complete_core_memory(vortex_gem5_handle_t h,
                                      uint64_t token,
                                      const uint8_t* read_data,
                                      uint32_t size);

// ---- Memory-access tracing (hettrace) --------------------------------------
//
// Optional, additive ABI. The gem5 side resolves these with dlsym() and
// null-checks the result, so a libvortex-gem5.so built without them still
// loads and runs -- tracing is simply unavailable.
//
// The tap is a read-only observer on the post-LLC request stream
// (vortex::Memory's pre-send hook). It does not alter the data path, the
// latency, or the request order. See sim/simx/gem5/vortex_trace.h.

// Returns the current global gem5 Tick. The device library cannot call
// curTick() -- it is dlopen'd and does not link gem5 -- so the SimObject
// passes this in and every trace timestamp comes from gem5's event queue.
typedef uint64_t (*vortex_gem5_tick_provider_t)(void* ctx);

// Open the trace and install the tap. `addr_offset` is added to every
// device address to move it into the unified physical space described by
// addrmap.json; pass 0 when the workload already uses global addresses.
//
// Returns 0 on success, -1 if tracing is disabled (HETTRACE_DIR unset) or
// the trace file could not be created. -1 is not fatal: it means "no trace",
// and the caller should carry on simulating.
int vortex_gem5_trace_open(vortex_gem5_handle_t h,
                           vortex_gem5_tick_provider_t tick_fn,
                           void* tick_ctx,
                           int64_t addr_offset);

// Flush and close the trace, writing the .meta.json sidecar. Idempotent.
// Called from the SimObject's serialize/exit path; the destructor also
// covers the case where the simulation is killed mid-run.
void vortex_gem5_trace_close(vortex_gem5_handle_t h);

// Records written so far, for the SimObject's exit summary. 0 if inactive.
uint64_t vortex_gem5_trace_emitted(vortex_gem5_handle_t h);

#ifdef __cplusplus
} // extern "C"
#endif
