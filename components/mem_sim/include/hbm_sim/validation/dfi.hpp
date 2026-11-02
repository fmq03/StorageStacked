#pragma once

// DFI 6.x-oriented 轨迹视图。模块保持在命令和数据 beat 层，记录控制器/PHY
// 边界事件及在线控制器提供的真实数据，不宣称覆盖全部 dfi_* 信号。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hbm_sim/controller/command.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

enum class DfiEventKind {
  Command,
  ReadData,
  WriteData,
};

struct DfiEvent {
  Cycle cycle = 0;
  // 产生本事件的 DRAM 命令发出周期。命令事件与 cycle 相同，数据事件保留它
  // 以建立无歧义索引。
  Cycle issued_cycle = 0;
  std::uint64_t request_id = 0;
  DfiEventKind kind = DfiEventKind::Command;
  Command command = Command::NOP;
  BusClass bus = BusClass::Unified;
  int phase = 0;
  int beat = 0;
  int beat_count = 0;
  std::size_t beat_bytes = 0;
  Address address = 0;
  Address system_address = 0;
  int stack_id = 0;
  // 常用 DFI 控制和数据观察信号。这里只是轨迹模型，不是完整 PHY 训练或
  // 引脚精确波形仿真器。
  bool dfi_reset_n = true;
  bool dfi_cs_n = true;
  bool dfi_cke = true;
  bool dfi_odt = false;
  bool dfi_rddata_en = false;
  bool dfi_wrdata_en = false;
  bool dfi_rddata_valid = false;
  std::uint64_t dfi_address = 0;
  std::uint64_t dfi_bank = 0;
  std::string dfi_wrdata;
  std::string dfi_rddata;
  std::string dfi_wrdata_mask;
  std::string payload_source = "none";
  std::string payload_init_mask;
  bool payload_initialized = true;
  DecodedAddress decoded;
};

struct DfiValidationReport {
  std::size_t checked_events = 0;
  std::size_t command_checks = 0;
  std::size_t data_beat_checks = 0;
  std::size_t latency_checks = 0;
  std::size_t phase_checks = 0;
  std::size_t signal_checks = 0;
  std::size_t payload_checks = 0;
  std::size_t expected_payload_checks = 0;
  std::vector<std::string> errors;

  bool ok() const { return errors.empty(); }
};

const char* to_string(DfiEventKind kind);

int dfi_phase_count(const DramSpec& spec);
std::size_t dfi_payload_beat_bytes(const DramSpec& spec);
std::uint64_t encode_dfi_address(const IssuedCommand& command);
std::vector<DfiEvent> build_dfi_trace(const DramSpec& spec,
                                      const std::vector<IssuedCommand>& commands);
DfiValidationReport validate_dfi_trace(const DramSpec& spec,
                                       const std::vector<IssuedCommand>& commands,
                                       const std::vector<DfiEvent>& events);
void write_dfi_trace_csv(const std::string& path, const std::vector<DfiEvent>& events);
void write_dfi_signal_trace_csv(const std::string& path, const std::vector<DfiEvent>& events);

}  // namespace hbm_sim
