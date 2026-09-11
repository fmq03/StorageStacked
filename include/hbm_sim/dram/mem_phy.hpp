#pragma once

// MC 与 Mem Stack 之间的行为级 PHY。
//
// Controller 保留 JEDEC 命令调度和 timing legality；MemPhy 建模 DFI 侧生命周期、
// command/data FIFO、协议编码和数据返回流水线；MemoryImage 仍是 stack/device 后端。

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include "hbm_sim/controller/command.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/request.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

enum class MemPhyMode { Direct, Behavioral };
enum class MemPhyProtocol { Hbm, Lpddr };
enum class MemPhyState { Reset, Initialization, Training, Ready, LowPower, Error };

const char* to_string(MemPhyMode mode);
const char* to_string(MemPhyProtocol protocol);
const char* to_string(MemPhyState state);
MemPhyMode parse_mem_phy_mode(std::string value);

struct MemPhyOptions {
  // Direct 保持历史完成语义；Behavioral 才启用在线 FIFO/训练/返回流水线。
  MemPhyMode mode = MemPhyMode::Direct;
  // 接口语义/审计标签；当前实现不会按版本切换 DFI 行为。
  std::string dfi_version = "6.0.1";
  std::size_t command_fifo_depth = 16;
  std::size_t read_fifo_depth = 16;
  std::size_t write_fifo_depth = 16;
  int command_pipeline_cycles = 1;
  int read_return_pipeline_cycles = 1;
  int write_data_pipeline_cycles = 1;
  // 生命周期长度与 pipeline 一样以 nCK 配置；内部会乘 tick_multiplier。
  int reset_cycles = 0;
  int initialization_cycles = 0;
  int training_cycles = 0;
  bool auto_train = true;
};

struct MemPhyStats {
  std::uint64_t commands = 0;
  std::uint64_t read_requests = 0;
  std::uint64_t write_requests = 0;
  std::uint64_t read_completions = 0;
  std::uint64_t write_completions = 0;
  std::uint64_t command_backpressure = 0;
  std::uint64_t data_backpressure = 0;
  std::uint64_t reset_cycles = 0;
  std::uint64_t initialization_cycles = 0;
  std::uint64_t training_cycles = 0;
  std::uint64_t ca_edges = 0;
  std::uint64_t hbm_row_commands = 0;
  std::uint64_t hbm_column_commands = 0;
  std::uint64_t lpddr_wck_events = 0;
  std::uint64_t lpddr_split_activate_events = 0;
  std::uint64_t max_command_fifo = 0;
  std::uint64_t max_read_fifo = 0;
  std::uint64_t max_write_fifo = 0;
  std::uint64_t total_read_service_cycles = 0;
  std::uint64_t total_write_service_cycles = 0;
};

struct MemPhyCompletion {
  std::uint64_t request_id = 0;
  std::uint64_t controller_sequence = 0;
  RequestType type = RequestType::Read;
  Command command = Command::NOP;
  Cycle issued_cycle = 0;
  Cycle completion_cycle = 0;
  ByteVector data;
  ByteVector initialized_mask;
  bool initialized = false;
  // MemoryImage 本次读触发的 SECDED 结果，继续传到 Controller/System response。
  bool ecc_corrected = false;
  bool ecc_uncorrectable = false;
};

// 协议适配器输出的是行为级 CA 编码摘要，而不是厂商 pin-accurate 波形。
struct PhyCommandEncoding {
  int ca_edges = 1;
  bool row_path = false;
  bool column_path = false;
  bool wck_event = false;
  bool split_activate = false;
};

class MemPhyProtocolAdapter {
 public:
  virtual ~MemPhyProtocolAdapter() = default;
  virtual MemPhyProtocol protocol() const = 0;
  virtual PhyCommandEncoding encode(const DramSpec& spec, Command command, BusClass bus) const = 0;
};

class HbmPhyAdapter final : public MemPhyProtocolAdapter {
 public:
  MemPhyProtocol protocol() const override { return MemPhyProtocol::Hbm; }
  PhyCommandEncoding encode(const DramSpec& spec, Command command, BusClass bus) const override;
};

class LpddrPhyAdapter final : public MemPhyProtocolAdapter {
 public:
  MemPhyProtocol protocol() const override { return MemPhyProtocol::Lpddr; }
  PhyCommandEncoding encode(const DramSpec& spec, Command command, BusClass bus) const override;
};

class MemPhy {
 public:
  MemPhy(DramSpec spec, MemPhyOptions options, std::shared_ptr<MemoryImage> memory_image);

  const MemPhyOptions& options() const { return options_; }
  const MemPhyStats& stats() const { return stats_; }
  MemPhyState state() const { return state_; }
  MemPhyProtocol protocol() const { return adapter_->protocol(); }
  bool behavioral() const { return options_.mode == MemPhyMode::Behavioral; }
  bool ready_for_data() const { return !behavioral() || state_ == MemPhyState::Ready; }
  bool idle() const;

  bool can_accept_command(Command command) const;
  bool can_accept_data(Command command) const;
  void note_command_backpressure();
  void note_data_backpressure();
  void accept_command(const Request& request, Command command, BusClass bus, Cycle cycle);
  // 返回预测完成拍；真正完成必须通过 take_completion() 取得。
  Cycle submit_data(const Request& request, Command command, Cycle cycle);
  void tick(Cycle cycle);
  std::optional<MemPhyCompletion> take_completion(std::uint64_t request_id,
                                                  std::uint64_t controller_sequence);

 private:
  struct CommandSlot { Cycle release_cycle = 0; };
  struct DataSlot {
    Request request;
    Command command = Command::NOP;
    Cycle issued_cycle = 0;
    Cycle ready_cycle = 0;
  };

  DramSpec spec_;
  MemPhyOptions options_;
  std::shared_ptr<MemoryImage> memory_image_;
  std::unique_ptr<MemPhyProtocolAdapter> adapter_;
  MemPhyState state_ = MemPhyState::Ready;
  Cycle state_deadline_ = 0;
  std::deque<CommandSlot> command_fifo_;
  std::deque<DataSlot> read_fifo_;
  std::deque<DataSlot> write_fifo_;
  std::deque<MemPhyCompletion> completions_;
  MemPhyStats stats_;

  Cycle timing_delay(int nck) const;
  void advance_lifecycle(Cycle cycle);
  void complete_slot(const DataSlot& slot, Cycle cycle);
};

}  // namespace hbm_sim
