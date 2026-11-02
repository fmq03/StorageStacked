/**
 * AXI入口检查。只接受递增、按传输大小对齐、不跨4KB的突发，并核对写末拍标志。
 */
#pragma once
#include "axi_if.h"

inline const char* axi_request_error(const AxChannel& ax) {
    if (ax.burst != 1) return "只支持 AXI INCR burst";
    if (ax.size > AXI_SIZE_CODE) return "AxSIZE 超出本地 AXI 数据位宽";
    const uint64_t beat_bytes = 1ULL << ax.size;
    if (ax.addr & (beat_bytes - 1)) return "AxADDR 未按 AxSIZE 对齐";
    if ((ax.addr & 4095ULL) + (uint64_t(ax.len) + 1) * beat_bytes > 4096ULL)
        return "AXI burst 跨越 4KB 边界";
    return nullptr;
}

inline bool axi_wlast_matches(unsigned beats_remaining, bool last) {
    return beats_remaining > 0 && last == (beats_remaining == 1);
}
