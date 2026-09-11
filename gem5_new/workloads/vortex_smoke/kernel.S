// Vortex 侧 tap 的最小验证内核。裸机 rv32im，不链 libc、不用 Vortex SDK。
//
// 存在的唯一理由：证明 Vortex 的 trace tap 真的会产出记录，且时间戳来自 gem5 的
// curTick()。本机没有 Vortex 的 LLVM 工具链（llvm-vortex + libc32 + libcrt32），
// 因此编不出正常的 .vxbin；但 vortex_gem5_load_kernel 的 flat-image 路径接受
// .bin，固定装载到设备内部地址 0x80000000 并由 KMU 从那里启动 —— 于是一个手写的
// 平坦镜像就够了。
//
// ---- 这个内核不是协同负载 ----
//
// 它只往设备内部地址空间里读写，不与 host 交换字节。host <-> Vortex 的字节共享要
// 走 CP 的 mem_upload + CMD_DCR_* 路径，需要真正的 .vxbin。见
// docs/03-limitations.md，那里如实写了哪些结论成立、哪些不成立。
//
// ---- 地址的选择 ----
//
// 数据放在 0xa0000000（addrmap.json 的 vortex_vram），于是记录落在"Vortex 应该访
// 问的那个区域"里。取指流量则不由我们决定：flat-image 的装载地址在设备库里写死成
// 0x80000000，所以指令的 miss 会被记成 host_heap 区域的访问。这是本 smoke test 的
// 产物，不是设计如此 —— CP 驱动的正常启动会把内核放进 VRAM。
//
// ---- 步长 64 字节 ----
//
// 一条 cache line 一个字，让每次访问都成为一次 miss，从而穿过 LLC 到 DRAM 被 tap
// 看到。若按 4 字节连续走，16 个字才产生 1 条记录，条数少到看不出规律。

    .section .text
    .globl _start

_start:
    li      t0, 0xa0000000          // 数据基址 = vortex_vram
    li      t1, 64                  // 64 次
    li      t2, 0                   // i
    li      t3, 0x1000              // 值的种子

    // ---- 写 64 个字，跨 64 条 line ----
write_loop:
    slli    t4, t2, 6               // i * 64
    add     t5, t0, t4
    add     t6, t3, t2
    sw      t6, 0(t5)
    addi    t2, t2, 1
    blt     t2, t1, write_loop

    // ---- 读回来求和 ----
    li      t2, 0
    li      a0, 0
read_loop:
    slli    t4, t2, 6
    add     t5, t0, t4
    lw      t6, 0(t5)
    add     a0, a0, t6
    addi    t2, t2, 1
    blt     t2, t1, read_loop

    // ---- 把校验和写到一个单独的 line 上 ----
    // 放在 +0x10000 而不是紧跟数据后面：单独一条 line 在 trace 里一眼可辨，
    // 用来确认内核真的跑到了最后一步而不是中途停了。
    li      t5, 0xa0010000
    sw      a0, 0(t5)

    // ---- 结束 ----
    // vx_tmc(0)：关掉本 warp 的所有线程。这是 Vortex 的终止方式，等价于
    // sw/kernel/include/vx_intrinsics.h 里的 vx_tmc_zero()。手写编码是因为不能
    // include 那个头（它要 Vortex 的编译器）。
    .insn r 0x0b, 0, 0, x0, x0, x0

    // 走不到这里。留着是为了万一 tmc 的语义与预期不符时，仿真停在一个死循环上
    // 而不是跑飞去执行 VRAM 里的数据 —— 后者会产生一堆莫名其妙的取指记录，
    // 比一个明确的超时难查得多。
hang:
    j       hang
