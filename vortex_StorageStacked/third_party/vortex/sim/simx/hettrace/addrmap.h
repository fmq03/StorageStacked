// Generated from config/addrmap.json; do not edit.
#pragma once
#include <cstdint>
#include <cstddef>
namespace hettrace {
constexpr int kMapAddrBits = 64;
constexpr uint64_t kTicksPerSecond = 1000000000000000ull;
enum SrcId : uint16_t {
    kSrcHost = 0,
    kSrcVortex = 1,
    kSrcCount = 2
};
enum TapLevel : uint8_t {
    kLevelPostLlc = 0,
    kLevelPreCache = 1,
    kLevelAxiMaster = 2,
    kLevelInterconnect = 3,
};
constexpr uint64_t kClockPeriodTicks_host = 500000ull;
constexpr uint64_t kClockPeriodTicks_vortex = 1000000ull;
constexpr uint64_t kVortexCpBase = 536870912ull;
constexpr uint64_t kVortexCpSize = 512ull;
constexpr uint64_t kHostHeapBase = 2147483648ull;
constexpr uint64_t kHostHeapSize = 268435456ull;
constexpr uint64_t kVortexBarBase = 4294967296ull;
constexpr uint64_t kVortexBarSize = 4294967296ull;
constexpr uint64_t kDramWindowBase = 2147483648ull;
constexpr uint64_t kDramWindowSize = 1073741824ull;
struct Region { const char* name; uint64_t base, size; bool is_dram; };
constexpr Region kRegions[] = {
    {"vortex_cp", 536870912ull, 512ull, false},
    {"host_heap", 2147483648ull, 268435456ull, true},
    {"vortex_bar", 4294967296ull, 4294967296ull, false},
};
constexpr size_t kNumRegions = sizeof(kRegions)/sizeof(kRegions[0]);
inline const char* RegionOf(uint64_t addr) { for (const auto& r:kRegions) if (addr>=r.base && addr<r.base+r.size) return r.name; return nullptr; }
inline bool IsDram(uint64_t addr) { return addr>=kDramWindowBase && addr<kDramWindowBase+kDramWindowSize; }
struct TraceWindow { const char* name; uint64_t base, size; };
constexpr TraceWindow kTraceWindows[] = {
    {"vortex_bar", 4294967296ull, 4294967296ull},
};
constexpr size_t kNumTraceWindows = sizeof(kTraceWindows)/sizeof(kTraceWindows[0]);
inline bool IsTraced(uint64_t addr) { for (const auto& w:kTraceWindows) if (addr>=w.base && addr<w.base+w.size) return true; return false; }
} // namespace hettrace
