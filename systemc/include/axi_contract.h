/**
 * @file axi_contract.h
 * @brief 桥接支持范围的入口检查：在消息和路由入队之前报告非法 AXI 请求。
 *
 * 支持 INCR、按 AxSIZE 对齐、SIZE 不超过本地数据总线、burst 不跨 4KB。
 * 窄传输保留 AXI 字节 lane 与 WSTRB 语义；完整内存端验证留给联调阶段。
 * 这里返回错误文字，便于独立负向测试；DUT 对错误采用 fail-fast，而不是
 * 吞掉已经握手的请求或构造没有规范依据的响应。WLAST 按 AWLEN 严格核对。
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
