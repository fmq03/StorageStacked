// libhettrace — 异构访存 trace 的记录格式。
//
// 本格式是 **AXI4 原生** 的：一条记录就是一次 AXI 通道事件（AW/W/B/AR/R），
// 而不是一次抽象的"访存"。这么定的理由是本项目的目标就是把 XPU 侧的 AXI4
// 事务喂给外部 DRAM 模拟器 —— 如果记录格式里没有 ID/LEN/SIZE/STRB/RESP，
// 那些字段就只能在别处再补一遍，于是同一份流量出现两种真值源。
//
// 旧的"每次访问一条记录"语义没有消失，它是本格式的一个**投影**：
// 取 chan ∈ {W, R} 的记录，其 (tick, addr, size, op) 与旧格式逐字段等价。
// 因此 stats/convert/merge 等工具的默认行为不变，见 IsDataChan()。
//
// 三个产生者（gem5 HetAxiMonitor、Vortex tap、CoralNPU AXI master tap）写同一
// 种格式，每个源一个独立文件。归并留给下游 Python 工具，理由见
// docs/02-trace-format.md：单文件交织写入在多时钟域下无法保证 tick 单调，
// 而排序错误一旦写进文件就不可恢复。
//
// header-only。gem5 用 SCons、CoralNPU 用 bazel、Vortex 用 make——
// 三套构建系统各自 include 即可，无需产出库文件。

#ifndef HETTRACE_RECORD_H_
#define HETTRACE_RECORD_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hettrace {

// ---------------------------------------------------------------------------
// 文件头：64 字节定长
// ---------------------------------------------------------------------------

constexpr char     kMagic[8]      = {'H', 'E', 'T', 'T', 'R', 'C', '\0', '\1'};

// v1 = 32 字节的纯访存记录；v2 = 本文件的 AXI4 记录。
// magic 保持不变是有意的：这样 v1 的文件被 v2 的工具读到时，报的是"版本
// 不受支持"而不是"这不是 hettrace 文件"，后者会让人往错的方向查。
constexpr uint32_t kFormatVersion = 2;
constexpr size_t   kHeaderSize    = 64;
constexpr size_t   kNameMax       = 24;

#pragma pack(push, 1)
struct FileHeader {
    char     magic[8];             // "HETTRC\0\1"
    uint32_t version;              // kFormatVersion
    uint32_t record_size;          // sizeof(Record) == 56
    uint64_t ticks_per_second;     // gem5 时间基准，1e12
    uint64_t clock_period_ticks;   // 本源一个时钟周期折算的 tick 数
    uint16_t src_id;               // 见 addrmap.h SrcId
    uint8_t  level;                // 见 addrmap.h TapLevel
    uint8_t  flags;                // bit0: 仅 DRAM 窗口（已过滤）
    uint16_t axi_data_bytes;       // AXI 数据总线宽度（字节）。解释 AxSIZE/AxLEN
                                   // 必须有它：单看 len 无法知道一拍多少字节。
    uint16_t axi_addr_bits;        // AXI 地址位宽
    char     name[kNameMax];       // 源名，NUL 补齐
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == kHeaderSize, "FileHeader 必须为 64 字节");

// 文件头 flags
constexpr uint8_t kHdrFilteredDram = 1u << 0;

// ---------------------------------------------------------------------------
// 记录：56 字节定长，小端
// ---------------------------------------------------------------------------

enum Op : uint8_t {
    kRead  = 0,
    kWrite = 1,
};

// AXI4 五通道。一次写事务写出 AW + (len+1) 条 W + B；一次读事务写出
// AR + (len+1) 条 R。同一事务的记录 txn 字段相同。
enum Chan : uint8_t {
    kChanAw = 0,
    kChanW  = 1,
    kChanB  = 2,
    kChanAr = 3,
    kChanR  = 4,
};

// 只有 W 和 R 真的搬字节。带宽、footprint、局部性这类统计必须只看这两个通道，
// 否则 AW/AR/B 会被算成额外流量。这也是"旧格式是本格式的投影"的确切含义。
inline bool IsDataChan(uint8_t chan) {
    return chan == kChanW || chan == kChanR;
}

inline const char* ChanName(uint8_t chan) {
    switch (chan) {
        case kChanAw: return "AW";
        case kChanW:  return "W";
        case kChanB:  return "B";
        case kChanAr: return "AR";
        case kChanR:  return "R";
        default:      return "??";
    }
}

enum Burst : uint8_t {
    kBurstFixed = 0,
    kBurstIncr  = 1,
    kBurstWrap  = 2,
};

inline const char* BurstName(uint8_t burst) {
    switch (burst) {
        case kBurstFixed: return "FIXED";
        case kBurstIncr:  return "INCR";
        case kBurstWrap:  return "WRAP";
        default:          return "?????";
    }
}

enum Resp : uint8_t {
    kRespOkay   = 0,
    kRespExOkay = 1,
    kRespSlvErr = 2,
    kRespDecErr = 3,
};

inline const char* RespName(uint8_t resp) {
    switch (resp) {
        case kRespOkay:   return "OKAY";
        case kRespExOkay: return "EXOKAY";
        case kRespSlvErr: return "SLVERR";
        case kRespDecErr: return "DECERR";
        default:          return "????";
    }
}

// 记录 flags
constexpr uint8_t kFlagBurstBeat = 1u << 0;  // burst 的非首拍
constexpr uint8_t kFlagPrefetch  = 1u << 1;  // 预取/推测，非程序序访问
constexpr uint8_t kFlagUnmapped  = 1u << 2;  // 地址不属于任何已声明区域
constexpr uint8_t kFlagInstr     = 1u << 3;  // 取指流量（可与数据流量分开分析）
constexpr uint8_t kFlagDma       = 1u << 4;  // 搬运引擎发出的，不是核发出的。
                                             // Vortex 的 CP 在暂存区与设备缓冲之间
                                             // 中转字节走的就是这条路：它不经过任何
                                             // cache，但确实占 DRAM 带宽。分析核的
                                             // 访存行为时要把它排掉，算带宽时不能排
constexpr uint8_t kFlagLast      = 1u << 5;  // WLAST / RLAST；B 上表示写事务完成
constexpr uint8_t kFlagSynth     = 1u << 6;  // 本条的 AXI 字段是**推导**出来的，
                                             // 不是从一组可直接采样的五通道
                                             // AXI 信号上读到的。统一 gem5 monitor
                                             // 看到的三个源都是 packet，因而都带
                                             // 这个位。CoralNPU 的原生 seam 可保留
                                             // 地址/ID/WSTRB，但五通道事件、时序与
                                             // 其余属性仍由 monitor 重构。任何拿本
                                             // trace 当"协议证据"的分析都必须先按
                                             // 这一位分开看。

#pragma pack(push, 1)
struct Record {
    uint64_t tick;      // 全局 tick。唯一时间基准，不是源内 cycle 数。
    uint64_t addr;      // 统一物理地址（addrmap.json）。AW/AR 是事务首地址；
                        // W/R 是该拍的地址；B 复制事务首地址，便于单行自解释。
    uint64_t strb;      // WSTRB，bit i = byte lane i。只对 chan==W 有意义。
                        // 读通道与地址通道恒为 0。
    uint32_t size;      // 本条覆盖的字节数。AW/AR = 整笔字节数；W/R = 一拍字节数。
    uint32_t ctx;       // 源内上下文：host=requestorId, vortex=hart_id, npu=AXI id。
                        // 与 axi_id 不是一回事 —— 保留它是因为多核 host 的
                        // requestorId 空间比 4 位 AXI ID 宽。
    uint32_t seq;       // 源内单调序号。用于稳定排序，并检测缓冲区溢出丢记录。
    uint32_t txn;       // 事务号。把 AW→W…→B 和 AR→R… 串起来；跨源不唯一。
    uint16_t src_id;
    uint16_t axi_id;    // AXI ID（AWID/WID/BID/ARID/RID）
    uint8_t  op;        // Op。AW/W/B => kWrite，AR/R => kRead。冗余但让旧工具
                        // 与只看单条记录的代码不必先解码 chan。
    uint8_t  chan;      // Chan
    uint8_t  axi_len;   // AxLEN，拍数 - 1
    uint8_t  axi_size;  // AxSIZE，log2(每拍字节数)
    uint8_t  burst;     // Burst
    uint8_t  resp;      // Resp。只对 chan ∈ {B, R} 有意义。
    uint8_t  user;      // AxUSER：发起方标识。当前与 src_id 同值，但它是**总线上
                        // 的信号**，src_id 是"哪个文件"，两者会在多源共用一条
                        // trace 时分开。
    uint8_t  flags;
    uint32_t reserved;  // 补齐到 56 并留给后续扩展（QoS/CACHE/PROT）
};
#pragma pack(pop)

static_assert(sizeof(Record) == 56, "Record 必须为 56 字节");

// ---------------------------------------------------------------------------
// 统计：随 trace 一起落盘为 <file>.meta.json，供下游校验
// ---------------------------------------------------------------------------

struct Stats {
    uint64_t emitted       = 0;  // 实际写出的记录数（含 AW/AR/B）
    uint64_t data_records  = 0;  // 其中 chan ∈ {W,R} 的条数 —— 与旧格式的
                                 // emitted 可直接对比
    uint64_t transactions  = 0;  // AW + AR 的条数，即 AXI 事务笔数
    uint64_t filtered      = 0;  // 被过滤掉的访问数（如 TCM 命中）
    uint64_t unmapped      = 0;  // 落在所有已声明区域之外
    uint64_t non_monotonic = 0;  // tick 小于上一条 —— 说明时间基准接错了
    uint64_t bytes         = 0;  // 数据通道字节总数（不是文件大小）
    uint64_t first_tick    = 0;
    uint64_t last_tick     = 0;
};

}  // namespace hettrace

#endif  // HETTRACE_RECORD_H_
