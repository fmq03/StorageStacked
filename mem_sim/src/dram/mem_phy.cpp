#include "hbm_sim/dram/mem_phy.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "hbm_sim/dram/interface.hpp"
#include "hbm_sim/dram/semantics.hpp"

namespace hbm_sim {
namespace {

bool is_read(Command command) { return command == Command::RD || command == Command::RDA; }
bool is_write(Command command) { return command == Command::WR || command == Command::WRA; }

std::size_t transfer_size(const DramSpec& spec, const Request& request) {
  if (request.transfer_bytes > 0) return request.transfer_bytes;
  if (request.has_expected_payload && !request.expected_payload.empty()) {
    return request.expected_payload.size();
  }
  if (request.has_payload && !request.payload.empty()) return request.payload.size();
  return static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
}

std::string normalized(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace

const char* to_string(MemPhyMode mode) {
  return mode == MemPhyMode::Behavioral ? "behavioral" : "direct";
}

const char* to_string(MemPhyProtocol protocol) {
  return protocol == MemPhyProtocol::Lpddr ? "lpddr" : "hbm";
}

const char* to_string(MemPhyState state) {
  switch (state) {
    case MemPhyState::Reset: return "reset";
    case MemPhyState::Initialization: return "initialization";
    case MemPhyState::Training: return "training";
    case MemPhyState::Ready: return "ready";
    case MemPhyState::LowPower: return "low_power";
    case MemPhyState::Error: return "error";
  }
  return "error";
}

MemPhyMode parse_mem_phy_mode(std::string value) {
  value = normalized(std::move(value));
  if (value == "direct" || value == "bypass") return MemPhyMode::Direct;
  if (value == "behavioral" || value == "behavioural" || value == "phy") {
    return MemPhyMode::Behavioral;
  }
  throw std::invalid_argument("invalid mem_phy_mode: " + value +
                              " (implemented: direct, behavioral)");
}

PhyCommandEncoding HbmPhyAdapter::encode(const DramSpec& spec,
                                         Command command,
                                         BusClass bus) const {
  PhyCommandEncoding result;
  result.row_path = bus == BusClass::Row || command_meta(command).row_command;
  result.column_path = bus == BusClass::Column || command_meta(command).column_command;
  // HBM 的行为级 CA 摘要按 edge-pairing 模式表达半拍，否则每命令一条 CA edge。
  result.ca_edges = spec.hbm_edge_pairing && result.row_path ? 2 : 1;
  return result;
}

PhyCommandEncoding LpddrPhyAdapter::encode(const DramSpec&,
                                           Command command,
                                           BusClass bus) const {
  (void)bus;
  PhyCommandEncoding result;
  result.row_path = command_meta(command).row_command;
  result.column_path = command_meta(command).column_command;
  // LPDDR 的 CA 命令按 double-data-rate edge 摘要；split ACT 明确保留 ACT1/ACT2。
  result.ca_edges = 2;
  result.wck_event = command == Command::CASRD || command == Command::CASWR ||
                     command == Command::WCKSYNC || command == Command::WCKTRAIN ||
                     command == Command::DVFS;
  result.split_activate = command == Command::ACT1 || command == Command::ACT2;
  return result;
}

MemPhy::MemPhy(DramSpec spec,
               MemPhyOptions options,
               std::shared_ptr<MemoryImage> memory_image)
    : spec_(std::move(spec)), options_(std::move(options)), memory_image_(std::move(memory_image)) {
  validate_spec(spec_);
  if (!memory_image_) throw std::invalid_argument("MemPhy requires a MemoryImage backend");
  if (options_.command_fifo_depth == 0 || options_.read_fifo_depth == 0 ||
      options_.write_fifo_depth == 0) {
    throw std::invalid_argument("MemPhy FIFO depths must be positive");
  }
  if (options_.command_pipeline_cycles < 0 ||
      options_.read_return_pipeline_cycles < 0 ||
      options_.write_data_pipeline_cycles < 0 || options_.reset_cycles < 0 ||
      options_.initialization_cycles < 0 || options_.training_cycles < 0) {
    throw std::invalid_argument(
        "MemPhy lifecycle and pipeline cycles must be non-negative");
  }
  if (spec_.lpddr_family) {
    adapter_ = std::make_unique<LpddrPhyAdapter>();
  } else {
    adapter_ = std::make_unique<HbmPhyAdapter>();
  }
  if (behavioral() && options_.reset_cycles > 0) {
    state_ = MemPhyState::Reset;
    state_deadline_ = timing_delay(options_.reset_cycles);
  } else if (behavioral() && options_.initialization_cycles > 0) {
    state_ = MemPhyState::Initialization;
    state_deadline_ = timing_delay(options_.initialization_cycles);
  } else if (behavioral() && options_.auto_train && options_.training_cycles > 0) {
    state_ = MemPhyState::Training;
    state_deadline_ = timing_delay(options_.training_cycles);
  }
}

Cycle MemPhy::timing_delay(int nck) const {
  return static_cast<Cycle>(std::max(0, nck)) * static_cast<Cycle>(std::max(1, spec_.tick_multiplier));
}

bool MemPhy::idle() const {
  return command_fifo_.empty() && read_fifo_.empty() && write_fifo_.empty() && completions_.empty();
}

bool MemPhy::can_accept_command(Command command) const {
  if (!behavioral()) return true;
  if (command_fifo_.size() >= options_.command_fifo_depth) return false;
  if (state_ == MemPhyState::Reset || state_ == MemPhyState::Initialization) return false;
  if (state_ == MemPhyState::Training) {
    // deadline!=0 是 PHY-independent auto training；deadline==0 是 DVFS 后等待
    // controller 发显式 WCKTRAIN 的训练债务。
    return state_deadline_ == 0 && command == Command::WCKTRAIN;
  }
  if (state_ == MemPhyState::LowPower) {
    return command == Command::PDX || command == Command::SREFEX;
  }
  return state_ == MemPhyState::Ready;
}

bool MemPhy::can_accept_data(Command command) const {
  if (!behavioral()) return true;
  if (!ready_for_data()) return false;
  if (is_read(command)) return read_fifo_.size() < options_.read_fifo_depth;
  if (is_write(command)) return write_fifo_.size() < options_.write_fifo_depth;
  return true;
}

void MemPhy::note_command_backpressure() { stats_.command_backpressure++; }
void MemPhy::note_data_backpressure() { stats_.data_backpressure++; }

void MemPhy::accept_command(const Request&, Command command, BusClass bus, Cycle cycle) {
  if (!behavioral()) return;
  if (!can_accept_command(command)) throw std::runtime_error("MemPhy command rejected by state/FIFO");
  PhyCommandEncoding encoding = adapter_->encode(spec_, command, bus);
  stats_.commands++;
  stats_.ca_edges += static_cast<std::uint64_t>(std::max(0, encoding.ca_edges));
  stats_.hbm_row_commands += encoding.row_path && protocol() == MemPhyProtocol::Hbm ? 1 : 0;
  stats_.hbm_column_commands += encoding.column_path && protocol() == MemPhyProtocol::Hbm ? 1 : 0;
  stats_.lpddr_wck_events += encoding.wck_event ? 1 : 0;
  stats_.lpddr_split_activate_events += encoding.split_activate ? 1 : 0;
  // 零周期命令路径是组合直通，不应占用 FIFO 到下一次 tick。否则 depth=1
  // 会错误阻止 HBM row/column 双发射，即使配置明确要求零流水延迟。
  if (options_.command_pipeline_cycles > 0) {
    command_fifo_.push_back(
        {cycle + timing_delay(options_.command_pipeline_cycles)});
    stats_.max_command_fifo =
        std::max<std::uint64_t>(stats_.max_command_fifo,
                                command_fifo_.size());
  }
  if (command == Command::PDE || command == Command::SREFEN) {
    state_ = MemPhyState::LowPower;
  } else if (command == Command::PDX || command == Command::SREFEX) {
    state_ = MemPhyState::Ready;
  } else if (command == Command::DVFS && protocol() == MemPhyProtocol::Lpddr &&
             spec_.lpddr_requires_wck_retrain_after_dvfs()) {
    state_ = MemPhyState::Training;
    state_deadline_ = 0;  // 显式 WCKTRAIN 才能清偿训练债务。
  } else if (command == Command::WCKTRAIN) {
    state_ = MemPhyState::Ready;
  }
}

Cycle MemPhy::submit_data(const Request& request, Command command, Cycle cycle) {
  if (!behavioral()) return cycle;
  if (!can_accept_data(command)) throw std::runtime_error("MemPhy data FIFO overflow or PHY not trained");
  DataSlot slot{request, command, cycle, cycle};
  if (is_read(command)) {
    const int device_latency = spec_.dfi_read_latency_nck > 0
                                   ? spec_.dfi_read_latency_nck + spec_.timing.nBL
                                   : spec_.timing.read_latency();
    slot.ready_cycle += timing_delay(device_latency + options_.read_return_pipeline_cycles);
    read_fifo_.push_back(slot);
    stats_.read_requests++;
    stats_.max_read_fifo = std::max<std::uint64_t>(stats_.max_read_fifo, read_fifo_.size());
  } else if (is_write(command)) {
    const int device_latency = spec_.dfi_write_latency_nck > 0
                                   ? spec_.dfi_write_latency_nck
                                   : spec_.timing.nCWL;
    slot.ready_cycle += timing_delay(device_latency + options_.write_data_pipeline_cycles);
    write_fifo_.push_back(slot);
    stats_.write_requests++;
    stats_.max_write_fifo = std::max<std::uint64_t>(stats_.max_write_fifo, write_fifo_.size());
  } else {
    throw std::invalid_argument("MemPhy submit_data requires RD/RDA/WR/WRA");
  }
  return slot.ready_cycle;
}

void MemPhy::advance_lifecycle(Cycle cycle) {
  if (!behavioral()) return;
  if (state_ == MemPhyState::Reset) {
    stats_.reset_cycles++;
    if (cycle >= state_deadline_) {
      if (options_.initialization_cycles > 0) {
        state_ = MemPhyState::Initialization;
        state_deadline_ = cycle + timing_delay(options_.initialization_cycles);
      } else if (options_.auto_train && options_.training_cycles > 0) {
        state_ = MemPhyState::Training;
        state_deadline_ = cycle + timing_delay(options_.training_cycles);
      } else {
        state_ = MemPhyState::Ready;
      }
    }
  } else if (state_ == MemPhyState::Initialization) {
    stats_.initialization_cycles++;
    if (cycle >= state_deadline_) {
      if (options_.auto_train && options_.training_cycles > 0) {
        state_ = MemPhyState::Training;
        state_deadline_ = cycle + timing_delay(options_.training_cycles);
      } else {
        state_ = MemPhyState::Ready;
      }
    }
  } else if (state_ == MemPhyState::Training && state_deadline_ != 0) {
    stats_.training_cycles++;
    if (cycle >= state_deadline_) state_ = MemPhyState::Ready;
  }
}

void MemPhy::complete_slot(const DataSlot& slot, Cycle cycle) {
  MemPhyCompletion completion;
  completion.request_id = slot.request.id;
  completion.controller_sequence = slot.request.controller_sequence;
  completion.type = slot.request.type;
  completion.command = slot.command;
  completion.issued_cycle = slot.issued_cycle;
  completion.completion_cycle = cycle;
  const DecodedAddress* decoded = slot.request.has_storage_decoded
                                      ? &slot.request.storage_decoded
                                      : &slot.request.decoded;
  if (slot.request.type == RequestType::Read) {
    const EccStatusCounters before = memory_image_->ecc_status_counters();
    const std::size_t size = transfer_size(spec_, slot.request);
    completion.data = memory_image_->read(slot.request.address, size, &completion.initialized, decoded);
    completion.initialized_mask =
        memory_image_->read_initialized_mask(slot.request.address, completion.data.size(), decoded);
    const EccStatusCounters after = memory_image_->ecc_status_counters();
    completion.ecc_corrected =
        after.ecc_corrected_errors > before.ecc_corrected_errors;
    completion.ecc_uncorrectable =
        after.ecc_uncorrectable_errors > before.ecc_uncorrectable_errors;
    stats_.read_completions++;
    stats_.total_read_service_cycles += cycle - slot.issued_cycle;
  } else {
    const ByteVector* mask = slot.request.has_byte_mask ? &slot.request.byte_mask : nullptr;
    memory_image_->write(slot.request.address,
                         slot.request.payload,
                         mask,
                         decoded,
                         slot.request.id,
                         cycle);
    completion.initialized = true;
    stats_.write_completions++;
    stats_.total_write_service_cycles += cycle - slot.issued_cycle;
  }
  completions_.push_back(std::move(completion));
}

void MemPhy::tick(Cycle cycle) {
  advance_lifecycle(cycle);
  while (!command_fifo_.empty() && command_fifo_.front().release_cycle <= cycle) {
    command_fifo_.pop_front();
  }
  while (!read_fifo_.empty() && read_fifo_.front().ready_cycle <= cycle) {
    complete_slot(read_fifo_.front(), cycle);
    read_fifo_.pop_front();
  }
  while (!write_fifo_.empty() && write_fifo_.front().ready_cycle <= cycle) {
    complete_slot(write_fifo_.front(), cycle);
    write_fifo_.pop_front();
  }
}

std::optional<MemPhyCompletion> MemPhy::take_completion(std::uint64_t request_id,
                                                        std::uint64_t controller_sequence) {
  auto found = std::find_if(completions_.begin(), completions_.end(), [&](const auto& completion) {
    return completion.request_id == request_id &&
           completion.controller_sequence == controller_sequence;
  });
  if (found == completions_.end()) return std::nullopt;
  MemPhyCompletion result = std::move(*found);
  completions_.erase(found);
  return result;
}

}  // namespace hbm_sim
