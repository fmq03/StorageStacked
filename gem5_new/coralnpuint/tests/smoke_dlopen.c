// libcoralnpu-gem5.so 的独立冒烟测试。
//
//   coralnpuint/tests/run_smoke.sh
//
// 为什么要有这个东西：gem5 侧的 CoralNPU SimObject 是通过 dlopen + dlsym 用这个
// 库的，一旦符号缺失或时钟推进逻辑写错，症状会以"gem5 启动失败"或者"trace 是
// 空的"这种形式出现在很远的地方。这个测试用**纯 C**（不链接任何 C++、不链接
// gem5、不链接 bazel 世界）把 gem5 会做的事按同样顺序做一遍：
//
//   dlopen -> create -> set_mem_backend -> trace_open -> load_elf -> start
//          -> while (tick()) ...            <- 每次只走一个周期，和 gem5 一样
//          -> trace_close -> destroy
//
// 纯 C 也顺带证明了 ABI 真的是 C ABI：如果头里漏进了任何 C++ 东西，这个文件
// 编不过。
//
// 内存后端接的是本文件里的一小块数组，并统计回调次数 —— 这验证了
// set_mem_backend 那条路径真的被 AXI master 走到了，而不是悄悄回落到库内私有
// DDR（那样"共享 buffer"就是假的）。
//
// 更进一步：跑的内核是 gem5int/ddr_touch.cc，它从 in[] 读、往 out[] 写。本文件
// 先把已知图案写进 g_mem 的 in[] 区，跑完再核对 out[] 区。这条检查同时证明了
// 数据**两个方向**都真的通了 —— 如果后端只是被调用但读回的是零，校验和会是 0，
// out[] 也会全是 1。这就是"真共享"与"只是地址上共享"的区别。

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../coralnpu_gem5.h"

// 假装是 gem5 的事件队列：每调用一次 tick() 前推进一个 CoralNPU 周期的 tick 数。
// 数值取 addrmap.json 里 coralnpu 的时钟（gem5 Tick = 1ps，500 MHz
// -> 2000 ticks/cycle），和侧车文件里的 clock_period_ticks 对得上 —— 那个字段
// 是从生成的 addrmap.h 里来的，两边不一致的话 dump 出来的时间轴会自相矛盾。
// tap 记下来的时间戳应当严格随周期单调增。
#define TICKS_PER_CYCLE 2000
static uint64_t g_tick = 0;
static uint64_t TickProvider(void* ctx) { (void)ctx; return g_tick; }

// 与 gem5int/ddr_touch.cc 和 addrmap.json 必须一致：
//   shared_buffer 0x90000000（DDR 窗口内 -> 走 AXI master）
//   npu_mailbox   0xc0000000（窗口外 -> 由 RTL 路由到 mailbox 寄存器）
#define SHARED_IN    0x90000000u
#define SHARED_OUT   0x90001000u
#define SHARED_WORDS 64u
#define DONE_TAG     0x600du

// 假装是 gem5 的物理内存：只覆盖 shared_buffer 开头的一小段。窗口之外的地址
// 读作 0、写丢弃 —— 真跑 gem5 时这里是 PortProxy，覆盖整个物理空间。
#define FAKE_MEM_BASE SHARED_IN
#define FAKE_MEM_SIZE (1u << 20)
static uint8_t  g_mem[FAKE_MEM_SIZE];
static uint64_t g_reads  = 0;
static uint64_t g_writes = 0;

// 落在假内存之外的后端访问次数。不该发生：内核只碰 shared_buffer 头部。
static uint64_t g_outside = 0;

static int InFakeMem(uint64_t addr, uint32_t size) {
    return addr >= FAKE_MEM_BASE &&
           (addr - FAKE_MEM_BASE) + size <= FAKE_MEM_SIZE;
}

static void MemRead(void* ctx, uint64_t addr, uint8_t* dst, uint32_t size) {
    (void)ctx;
    ++g_reads;
    if (InFakeMem(addr, size)) memcpy(dst, g_mem + (addr - FAKE_MEM_BASE), size);
    else { ++g_outside; memset(dst, 0, size); }
}

static void MemWrite(void* ctx, uint64_t addr, const uint8_t* src, uint32_t size) {
    (void)ctx;
    ++g_writes;
    if (InFakeMem(addr, size)) memcpy(g_mem + (addr - FAKE_MEM_BASE), src, size);
    else ++g_outside;
}

static uint32_t MemWord(uint64_t addr) {
    uint32_t v;
    memcpy(&v, g_mem + (addr - FAKE_MEM_BASE), 4);
    return v;
}

static void SetMemWord(uint64_t addr, uint32_t v) {
    memcpy(g_mem + (addr - FAKE_MEM_BASE), &v, 4);
}

static int failures = 0;
static void Check(int ok, const char* what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++failures;
}

#define SYM(var, name)                                                       \
    do {                                                                     \
        *(void**)(&var) = dlsym(lib, name);                                   \
        if (var == NULL) {                                                    \
            fprintf(stderr, "dlsym(%s) 失败: %s\n", name, dlerror());          \
            return 2;                                                         \
        }                                                                     \
    } while (0)

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <libcoralnpu-gem5.so> <kernel.elf>\n", argv[0]);
        return 2;
    }
    const char* so_path  = argv[1];
    const char* elf_path = argv[2];

    void* lib = dlopen(so_path, RTLD_LAZY | RTLD_LOCAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen(%s) 失败: %s\n", so_path, dlerror());
        return 2;
    }

    // gem5 侧解析的是同一批符号（见 gem5int/src/dev/coralnpu/coralnpu_dev.cc）。
    const char* (*build_info)(void);
    coralnpu_gem5_handle_t (*create)(void);
    void (*destroy)(coralnpu_gem5_handle_t);
    void (*set_mem_backend)(coralnpu_gem5_handle_t, coralnpu_gem5_mem_read_t,
                            coralnpu_gem5_mem_write_t, void*);
    int (*load_elf)(coralnpu_gem5_handle_t, const char*, uint32_t*);
    void (*start)(coralnpu_gem5_handle_t, uint32_t);
    bool (*tick)(coralnpu_gem5_handle_t);
    bool (*halted)(coralnpu_gem5_handle_t);
    bool (*wfi)(coralnpu_gem5_handle_t);
    uint32_t (*mailbox_read)(coralnpu_gem5_handle_t, uint32_t);
    void (*mailbox_write)(coralnpu_gem5_handle_t, uint32_t, uint32_t);
    int (*trace_open)(coralnpu_gem5_handle_t, coralnpu_gem5_tick_provider_t,
                      void*, int64_t);
    void (*trace_close)(coralnpu_gem5_handle_t);
    uint64_t (*trace_emitted)(coralnpu_gem5_handle_t);
    uint64_t (*trace_anomalies)(coralnpu_gem5_handle_t);

    SYM(build_info,      "coralnpu_gem5_build_info");
    SYM(create,          "coralnpu_gem5_create");
    SYM(destroy,         "coralnpu_gem5_destroy");
    SYM(set_mem_backend, "coralnpu_gem5_set_mem_backend");
    SYM(load_elf,        "coralnpu_gem5_load_elf");
    SYM(start,           "coralnpu_gem5_start");
    SYM(tick,            "coralnpu_gem5_tick");
    SYM(halted,          "coralnpu_gem5_halted");
    SYM(wfi,             "coralnpu_gem5_wfi");
    SYM(mailbox_read,    "coralnpu_gem5_mailbox_read");
    SYM(mailbox_write,   "coralnpu_gem5_mailbox_write");
    SYM(trace_open,      "coralnpu_gem5_trace_open");
    SYM(trace_close,     "coralnpu_gem5_trace_close");
    SYM(trace_emitted,   "coralnpu_gem5_trace_emitted");
    SYM(trace_anomalies, "coralnpu_gem5_trace_anomalies");
    printf("  info  %s\n", build_info());

    coralnpu_gem5_handle_t h = create();
    Check(h != NULL, "create() 返回非 NULL");
    if (h == NULL) return 1;

    set_mem_backend(h, MemRead, MemWrite, NULL);

    // 冒名 host CPU：把已知图案写进共享 buffer 的 in[] 区。gem5 里这一步是
    // host 程序的普通 store，经 PortProxy 落到同一段物理内存。
    uint32_t expect_sum = 0;
    for (uint32_t i = 0; i < SHARED_WORDS; ++i) {
        const uint32_t v = 0x1000u + i * 3u;
        SetMemWord(SHARED_IN + i * 4u, v);
        expect_sum += v;
    }

    // gem5 在 startup() 里的顺序：先开 trace，再加载 ELF —— 加载走的是 AXI
    // slave，不经过 master tap，所以不会污染 trace，但顺序照抄以免掩盖问题。
    const int trc = trace_open(h, TickProvider, NULL, 0);
    printf("  info  trace_open -> %d (%s)\n", trc,
           trc == 0 ? "已启用" : "未启用/HETTRACE_DIR 未设");

    // mailbox 是跑起来之后 host 与 NPU 唯一安全的双向通路，先验证它可读可写。
    mailbox_write(h, 1, 0xdeadbeefu);
    Check(mailbox_read(h, 1) == 0xdeadbeefu, "mailbox 回读一致");
    Check(mailbox_read(h, 7) == 0, "mailbox 越界索引返回 0 而不是崩");
    mailbox_write(h, 1, 0);

    // 入口 PC 用出参带回来，返回值只表示成败。CoralNPU 的 ITCM 从 0 开始，
    // wfi_slot_0.elf 的入口 PC 实测就是 0x0 —— 用 0 当错误码会把它判成失败。
    uint32_t entry = 0xffffffffu;
    Check(load_elf(h, "/nonexistent.elf", &entry) == -1,
          "load_elf 对不存在的文件返回 -1");
    const int lrc = load_elf(h, elf_path, &entry);
    printf("  info  load_elf -> %d, 入口 PC = 0x%08x\n", lrc, entry);
    Check(lrc == 0, "load_elf 成功");
    if (lrc != 0) { destroy(h); return 1; }

    Check(!halted(h), "启动前未 halted");
    start(h, entry);

    // 核心：每次 tick() 只走一个周期，时钟由这里（冒名 gem5）推进。
    const uint64_t kMaxCycles = 200000;
    uint64_t cycles = 0;
    while (cycles < kMaxCycles) {
        g_tick += TICKS_PER_CYCLE;   // gem5 会在 tick 事件之间自己走时间
        ++cycles;
        if (!tick(h)) break;         // halted 或 wfi
    }
    printf("  info  跑了 %llu 周期后停下 (halted=%d wfi=%d)\n",
           (unsigned long long)cycles, (int)halted(h), (int)wfi(h));
    Check(cycles < kMaxCycles, "在周期上限内结束（没有跑飞）");
    Check(halted(h) || wfi(h), "停下来的原因是 halted 或 wfi");

    const uint64_t emitted = trace_emitted(h);
    const uint64_t anomalies = trace_anomalies(h);
    printf("  info  trace 记录 %llu 条, AXI master 后端回调 %llu 读 / %llu 写\n",
           (unsigned long long)emitted, (unsigned long long)g_reads,
           (unsigned long long)g_writes);
    Check(anomalies == 0, "无异常（多拍 burst / 越界）");
    Check(g_reads > 0 && g_writes > 0,
          "AXI master 真的走了外部内存后端（读写都有）");
    Check(g_outside == 0, "后端访问全部落在 shared_buffer 内（地址映射正确）");

    // 数据双向连通性：out[i] 必须等于 2*in[i]+1。NPU 读到的若是零，这里全错。
    int data_ok = 1;
    for (uint32_t i = 0; i < SHARED_WORDS; ++i) {
        const uint32_t in  = 0x1000u + i * 3u;
        const uint32_t got = MemWord(SHARED_OUT + i * 4u);
        if (got != in * 2u + 1u) {
            if (data_ok) {
                printf("  info  首个不符: out[%u] = 0x%08x, 期望 0x%08x\n",
                       i, got, in * 2u + 1u);
            }
            data_ok = 0;
        }
    }
    Check(data_ok, "共享 buffer 数据双向连通 (out[i] == 2*in[i]+1)");

    // mailbox 是 NPU 主动写的，走的是 DDR 窗口外那条路。校验和对上说明 NPU
    // 读到的确实是 host 写进去的字节。
    const uint32_t mb = mailbox_read(h, 0);
    printf("  info  mailbox[0] = 0x%08x (期望 tag=0x%04x sum&0xffff=0x%04x)\n",
           mb, DONE_TAG, expect_sum & 0xffffu);
    Check((mb >> 16) == DONE_TAG, "NPU 写了完成标志到 mailbox");
    Check((mb & 0xffffu) == (expect_sum & 0xffffu),
          "mailbox 校验和与 host 写入的数据一致");

    // trace 记录数与后端回调数必须自洽：读是一对一；写侧一次 AXI 回调可能拆成
    // 多条 trace 记录（strb 不连续）也可能拆成多次后端调用（逐字节），所以只
    // 能要求"有 master 流量 <=> 有记录"。
    if (trc == 0) {
        Check(emitted > 0, "tap 记下了 master 流量");
        Check(emitted >= g_reads,
              "trace 记录数不少于后端读次数（读是一对一）");
    }

    trace_close(h);
    Check(trace_emitted(h) == emitted, "trace_close 之后记录数仍可查（走缓存）");
    trace_close(h);  // 幂等
    Check(1, "trace_close 可重复调用");

    destroy(h);
    destroy(NULL);   // 必须安全
    Check(1, "destroy(NULL) 安全");

    dlclose(lib);
    printf(failures == 0 ? "\n全部通过\n" : "\n%d 项失败\n", failures);
    return failures == 0 ? 0 : 1;
}
