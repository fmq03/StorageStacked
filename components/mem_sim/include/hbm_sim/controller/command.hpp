#pragma once

// 一条实际发出的命令事件。在线 Controller、CSV trace 和离线 Validator 都用同一格式。

#include <cstdint>
#include <vector>

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

// 一条实际发出的 DRAM 命令。测试、CSV trace 和 validation hook 都使用它
// 作为统一事件格式，避免把命令流接口绑定到 Controller 内部实现。
struct IssuedCommand {
  IssuedCommand() = default;
  IssuedCommand(Cycle issued_cycle,
                std::uint64_t issued_request_id,
                Command issued_command,
                BusClass issued_bus,
                DecodedAddress issued_decoded)
      : cycle(issued_cycle),
        request_id(issued_request_id),
        command(issued_command),
        bus(issued_bus),
        decoded(issued_decoded) {}

  // 命令实际发出的 controller cycle。
  Cycle cycle = 0;
  // 0 表示使用 legacy DFI latency 推导；Behavioral PHY 在真实数据完成时回填。
  Cycle data_cycle = 0;
  // 对应 Request::id，便于测试把命令追溯到原始请求。
  std::uint64_t request_id = 0;
  // 实际发出的抽象 DRAM 命令。
  Command command = Command::NOP;
  // 本命令占用的总线。HBM 下可为 Row/Column，LPDDR 下为 Unified。
  BusClass bus = BusClass::Unified;
  // 发命令时请求的 DRAM 坐标快照；sequence test 用它判断作用域。
  DecodedAddress decoded;
  // stack_id 与 decoded 分离：DecodedAddress 始终是 stack-local JEDEC 坐标。
  int stack_id = 0;
  Address system_address = 0;
  // DFI-oriented 轨迹使用的可选数据快照。数据感知导出在 RD/WR 完成后填写，
  // 使写合并和掩码写与实际进入 MemoryImage 的字节保持一致。
  Address address = 0;
  std::vector<std::uint8_t> payload;
  std::vector<std::uint8_t> expected_payload;
  std::vector<std::uint8_t> byte_mask;
  std::vector<std::uint8_t> initialized_mask;
  bool has_payload = false;
  bool has_expected_payload = false;
  bool has_byte_mask = false;
  bool payload_initialized = true;
};

}  // namespace hbm_sim
