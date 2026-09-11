// libcoralnpu-gem5 — gem5 CoralNPU SimObject 的 C ABI。
//
// 结构照 Vortex 那条腿抄的（sim/simx/gem5/vortex_gpgpu.h）：gem5 侧 dlopen 这个
// .so，只通过下面这些扁平 C 函数说话。这样 Verilator 生成的类型、absl、
// CoralNPU 的 bazel 世界全都不会漏进 gem5 的 SCons 世界，反之亦然 —— 两棵树
// 用不同的构建系统和不同的编译器，能不共享 C++ 类型是这个设计的全部价值。
//
// ---- 时间由谁掌握 ----
//
// gem5 的事件队列是唯一时钟主。设备库自己**绝不**推进时钟：
// coralnpu_gem5_tick() 走一个周期然后立刻返回，由 gem5 的 tick 事件按
// CoralNPU 的时钟周期反复调用。
//
// 这是本文件里最重要的一条约束，因为 CoreMiniAxiWrapper 里好几个现成方法
// （Write / Read / WriteWord / WaitForTermination）是靠内部 while(...) Step()
// 阻塞的。在仿真跑起来之后调用它们，等于让 CoralNPU 的时钟脱离 gem5 偷跑若干
// 周期：那期间产生的所有 trace 记录会挤在同一个 gem5 tick 上，时间基准就废了。
// 所以：
//   - 程序加载与启动只在 coralnpu_gem5_load_elf() / _start() 里做，
//     且只允许在 gem5 开始 tick 之前调用（gem5 侧在 startup() 里调）；
//   - 跑起来之后，host 与 NPU 只通过共享内存和 mailbox 交互，
//     不再走阻塞的 AXI slave 事务。
//
// ---- 内存后端 ----
//
// CoralNPU 的 AXI master 端口是它访问外部 DDR 的唯一通路。默认情况下设备库
// 用自己的一块私有数组当 DDR（和参考实现 core_mini_axi_simulator.cc 一样），
// 这时"共享 buffer"只是地址上共享，数据并不真的共享。
//
// standalone 测试可用同步 set_mem_backend()。gem5 的生产路径使用下面的
// set_timing_backend()：请求先返回，等 timing memory 完成后再注入 AXI R/B，
// 因而下游排队与延迟会反压 NPU。
//
// ---- 并发 ----
//
// 所有调用都串行发生在 gem5 的事件循环线程上。库内没有任何锁，也不可重入。

#ifndef CORALNPU_GEM5_CORALNPU_GEM5_H_
#define CORALNPU_GEM5_CORALNPU_GEM5_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 不透明句柄。持有 VerilatedContext、CoreMiniAxiWrapper、trace tap，
// 以及（未接 gem5 后端时）一块私有 DDR 数组。
typedef struct coralnpu_gem5_device_s* coralnpu_gem5_handle_t;

// CoralNPU AXI master 侧看到的 DDR 窗口。写死在 RTL 的行为里，参考实现
// core_mini_axi_simulator.cc:50 的 IsDdrAddress() 就是这个范围；落在窗口外的
// master 访问会被路由到 mailbox。addrmap.json 的 dram_window 与之对齐，
// 所以三方共用一套物理地址时不需要改 RTL。
#define CORALNPU_GEM5_DDR_BASE 0x80000000u
#define CORALNPU_GEM5_DDR_SIZE 0x40000000u

// 构建信息（RVV 是否开启等）。返回静态指针，不要 free。
const char* coralnpu_gem5_build_info(void);

// 构造实例并复位。失败返回 NULL。
coralnpu_gem5_handle_t coralnpu_gem5_create(void);

// 销毁。传 NULL 安全。
void coralnpu_gem5_destroy(coralnpu_gem5_handle_t h);

// ---- 内存后端 --------------------------------------------------------------

// 把 AXI master 的读写接到同步外部内存，供 standalone/兼容测试使用。gem5
// timing 模式不要使用这个接口，应使用下面的事件驱动接口。
//
// 传 NULL 恢复为库内私有 DDR 数组。
typedef void (*coralnpu_gem5_mem_read_t)(void* ctx, uint64_t addr,
                                        uint8_t* dst, uint32_t size);
typedef void (*coralnpu_gem5_mem_write_t)(void* ctx, uint64_t addr,
                                         const uint8_t* src, uint32_t size);
void coralnpu_gem5_set_mem_backend(coralnpu_gem5_handle_t h,
                                   coralnpu_gem5_mem_read_t read_fn,
                                   coralnpu_gem5_mem_write_t write_fn,
                                   void* ctx);

// Event-driven timing backend.  Issue callbacks are invoked after a real AXI
// address/data handshake and return without a response.  The simulator must
// later call complete_read/complete_write; until then the RTL observes no
// RVALID/BVALID and stalls naturally.  This is the production path for gem5
// timing mode; the synchronous callbacks above remain for standalone use.
typedef void (*coralnpu_gem5_timing_read_t)(void* ctx, uint64_t addr,
                                            uint8_t axi_id, uint32_t size);
typedef void (*coralnpu_gem5_timing_write_t)(void* ctx, uint64_t addr,
                                             uint8_t axi_id,
                                             const uint8_t* src,
                                             uint16_t strb, uint32_t size);
void coralnpu_gem5_set_timing_backend(
    coralnpu_gem5_handle_t h,
    coralnpu_gem5_timing_read_t read_fn,
    coralnpu_gem5_timing_write_t write_fn,
    void* ctx);

void coralnpu_gem5_complete_read(coralnpu_gem5_handle_t h, uint8_t axi_id,
                                 const uint8_t* src, uint32_t size,
                                 uint8_t resp);
void coralnpu_gem5_complete_write(coralnpu_gem5_handle_t h, uint8_t axi_id,
                                  uint8_t resp);

// 直接读写库内私有 DDR 数组，供未接后端时预置/回读数据。接了 gem5 后端后
// 这两个函数就没有意义了（私有数组不再被使用），调用会被忽略。
void coralnpu_gem5_ddr_write(coralnpu_gem5_handle_t h, uint64_t addr,
                             const uint8_t* src, uint32_t size);
void coralnpu_gem5_ddr_read(coralnpu_gem5_handle_t h, uint64_t addr,
                            uint8_t* dst, uint32_t size);

// ---- 程序加载与启动 --------------------------------------------------------
//
// 两者都会内部推进 CoralNPU 的时钟若干周期（AXI slave 事务靠时钟完成），
// 所以**只能在 gem5 开始 tick 之前调用**。见文件头。

// 把 ELF 段搬进 TCM。返回 0 成功、-1 失败，入口 PC 从 out_entry 带出。
//
// 状态与入口 PC 分开返回，不用"返回 0 表示失败"那种写法：CoralNPU 的 ITCM 从
// 地址 0 开始，实测 tests/cocotb 里的内核入口 PC 就是 0x0，所以 0 是完全合法的
// 入口地址，拿它当错误码会把正常内核判成加载失败。
int coralnpu_gem5_load_elf(coralnpu_gem5_handle_t h, const char* path,
                           uint32_t* out_entry);

// 写 ctrl 寄存器启动内核：pc <- start_addr，然后 reset 脉冲。
void coralnpu_gem5_start(coralnpu_gem5_handle_t h, uint32_t start_addr);

// ---- 时钟推进 --------------------------------------------------------------

// 走**一个** CoralNPU 时钟周期后立刻返回。返回 true 表示核还在跑
// （既没 halted 也没 wfi），gem5 侧据此决定是否继续调度 tick 事件。
bool coralnpu_gem5_tick(coralnpu_gem5_handle_t h);

// 非阻塞状态查询。halted 是执行结束，wfi 是等中断。
bool coralnpu_gem5_halted(coralnpu_gem5_handle_t h);
bool coralnpu_gem5_wfi(coralnpu_gem5_handle_t h);

// ---- mailbox ---------------------------------------------------------------
// 4 × 32 bit。落在 DDR 窗口外的 master 访问会打到这里，是 host 与 NPU 之间
// 的轻量握手通路（跑起来之后唯一安全的双向通道）。
uint32_t coralnpu_gem5_mailbox_read(coralnpu_gem5_handle_t h, uint32_t index);
void     coralnpu_gem5_mailbox_write(coralnpu_gem5_handle_t h, uint32_t index,
                                     uint32_t value);

// ---- 访存 trace ------------------------------------------------------------

// 返回当前全局 gem5 Tick。设备库不链接 gem5，拿不到 curTick()，
// 只能由 gem5 侧递进来。三个源共用同一时间基准是归并的前提。
typedef uint64_t (*coralnpu_gem5_tick_provider_t)(void* ctx);

// 打开 trace 并挂上 AXI master tap。addr_offset 加在设备地址上，用于搬进
// addrmap.json 的统一物理空间；CoralNPU 的 DDR 窗口本来就与 addrmap 对齐，
// 所以正常情况传 0。
//
// 返回 0 成功；-1 表示 trace 未启用（HETTRACE_DIR 没设）或文件创建失败。
// -1 不是致命错误，调用方应照常仿真。
int coralnpu_gem5_trace_open(coralnpu_gem5_handle_t h,
                             coralnpu_gem5_tick_provider_t tick_fn,
                             void* tick_ctx,
                             int64_t addr_offset);

// 刷盘并写 .meta.json 侧车文件。幂等。
void coralnpu_gem5_trace_close(coralnpu_gem5_handle_t h);

// 已写出的记录数，供 gem5 侧打退出摘要。未启用时为 0。
uint64_t coralnpu_gem5_trace_emitted(coralnpu_gem5_handle_t h);

// 见 coralnpu_trace.h 文件头：非 0 说明核发出了多拍 master burst，此时
// tap 与参考实现的 DDR 后端解读都不成立，trace 不可信。gem5 侧应据此告警。
uint64_t coralnpu_gem5_trace_anomalies(coralnpu_gem5_handle_t h);

#ifdef __cplusplus
} // extern "C"
#endif

#endif  // CORALNPU_GEM5_CORALNPU_GEM5_H_
