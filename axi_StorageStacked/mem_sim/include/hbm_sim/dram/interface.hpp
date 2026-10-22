#pragma once

// InterfaceModel 负责把 DramSpec 中的协议开关换算成接口开销记账量。
// DramSpec 只描述器件/模式配置；这里集中处理 request-level metadata/ECC
// 和 command-level CA parity 等开销，避免把带宽口径散落到 Controller 中。

#include <cstddef>

#include "hbm_sim/core/common.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

// These helpers account for configured payload/metadata demand, not all physical
// DQ slots. LPDDR6 BL24 x12 has 288 physical bits (256 payload + 32 non-payload);
// with protection accounting disabled, request_interface_bytes can still be 32.
// HBM ECC-pin overhead is separate from the storage backend's ECC shadow.

// LPDDR6 DBI 与 selectable Link ECC/EDC 复用 metadata lane。启用多个功能时，
// 接口占用按 lane 宽度取最大值，而不是把 DBI 和 Link ECC 位数简单相加。
int lpddr_metadata_lane_bits_per_request(const DramSpec& spec);

// 单个 DRAM transaction（不是拆分前 HostRequest）额外记账的协议 bit，不包含 payload，也不包含
// CA parity 这类 command/address bus 级开销。
int request_protocol_overhead_bits(const DramSpec& spec);

// 单个 DRAM transaction 的等效接口需求 byte 数，包含 payload 和
// request-level metadata/ECC，按 byte 向上取整。当前该数值用于统计，不会
// 自动延长命令/数据总线占用，因而不是 pin-level 实际传输拍数。
int request_interface_bytes(const DramSpec& spec, std::size_t payload_bytes);
int request_interface_bytes(const DramSpec& spec);

// 单条 DRAM command 在命令/地址接口上的额外 bit 开销。目前主要用于
// LPDDR6 CA parity；未来可继续扩展 CA CRC、command bus ECC 等特性。
int command_interface_overhead_bits(const DramSpec& spec, Command command);

}  // namespace hbm_sim
