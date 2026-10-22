#include "hbm_sim/core/stack_model.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace hbm_sim {
namespace {

bool is_read_command(Command command) {
  return command == Command::RD || command == Command::RDA ||
         command == Command::CASRD;
}

bool is_write_command(Command command) {
  return command == Command::WR || command == Command::WRA ||
         command == Command::CASWR;
}

std::size_t command_payload_bytes(const DramSpec &spec,
                                  const StackCommand &command) {
  if (command.payload_bytes != 0) {
    return command.payload_bytes;
  }
  if (!command.payload.empty()) {
    return command.payload.size();
  }
  if (is_read_command(command.command) || is_write_command(command.command)) {
    return static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
  }
  return 0;
}

StorageModelOptions options_for_stack(StorageModelOptions options,
                                      int stack_id) {
  options.stack_id = stack_id;
  return options;
}

} // namespace

StackModel::StackModel(int stack_id, DramSpec spec,
                       StorageModelOptions storage_options)
    : stack_id_(stack_id), spec_(std::move(spec)),
      memory_image_(spec_, 0,
                    options_for_stack(std::move(storage_options), stack_id_)) {
  if (stack_id_ < 0) {
    throw std::invalid_argument("stack_id must be non-negative");
  }
}

void StackModel::check_stack_id(int stack_id) const {
  if (stack_id != stack_id_) {
    throw std::out_of_range(
        "stack command targets stack " + std::to_string(stack_id) +
        ", but this StackModel is stack " + std::to_string(stack_id_));
  }
}

StackReadResult StackModel::read(Address address, std::size_t size,
                                 const DecodedAddress *decoded) {
  StackReadResult result;
  result.stack_id = stack_id_;
  result.address = address;
  result.data = memory_image_.read(address, size, &result.initialized, decoded);
  result.metadata = memory_image_.metadata(address, decoded);
  return result;
}

void StackModel::write(Address address, const ByteVector &data,
                       const ByteVector *mask, const DecodedAddress *decoded,
                       std::uint64_t request_id, Cycle cycle) {
  memory_image_.write(address, data, mask, decoded, request_id, cycle);
}

StackCommandResult StackModel::issue_command(const StackCommand &command) {
  check_stack_id(command.stack_id);

  StackCommandResult result;
  result.stack_id = stack_id_;
  const std::size_t bytes = command_payload_bytes(spec_, command);
  // 这里是被动器件事件入口：命令合法性和发令时机由外部 MC slice 保证，
  // 本模型只记录命令事件并更新数据、行缓冲、功耗和热状态。
  memory_image_.record_command_event(command.command, command.decoded,
                                     command.cycle, bytes);

  switch (command.command) {
  case Command::ACT:
  case Command::ACT2:
    memory_image_.activate_row(command.decoded, command.cycle);
    break;
  case Command::ACT1:
    // LPDDR split activate 的第一拍只记录事件；完整 row 打开在 ACT2。
    break;
  case Command::PREPB:
    memory_image_.precharge_bank(command.decoded, command.cycle);
    break;
  case Command::PREAB:
    memory_image_.precharge_all(command.decoded, command.cycle);
    break;
  default:
    break;
  }

  if (is_write_command(command.command) && command.has_address &&
      !command.payload.empty()) {
    const ByteVector *mask = command.has_mask ? &command.mask : nullptr;
    memory_image_.write(command.address, command.payload, mask,
                        &command.decoded, command.request_id, command.cycle);
  }

  if (is_read_command(command.command) && command.has_address) {
    result.read_data_valid = true;
    result.read_data = memory_image_.read(
        command.address, bytes, &result.initialized, &command.decoded);
    result.metadata = memory_image_.metadata(command.address, &command.decoded);
  }

  if ((command.command == Command::RDA || command.command == Command::WRA) &&
      command.has_address) {
    memory_image_.precharge_bank(command.decoded, command.cycle);
  }
  return result;
}

MultiStackMemoryModel::MultiStackMemoryModel(
    DramSpec spec, StorageModelOptions storage_options)
    : MultiStackMemoryModel(std::move(spec), kDefaultStackCount,
                            std::move(storage_options)) {}

MultiStackMemoryModel::MultiStackMemoryModel(
    DramSpec spec, int stack_count, StorageModelOptions storage_options)
    : spec_(std::move(spec)) {
  if (stack_count <= 0) {
    throw std::invalid_argument("stack_count must be positive");
  }
  stacks_.reserve(static_cast<std::size_t>(stack_count));
  for (int stack_id = 0; stack_id < stack_count; stack_id++) {
    stacks_.push_back(
        std::make_unique<StackModel>(stack_id, spec_, storage_options));
  }
}

StackModel &MultiStackMemoryModel::checked_stack(int stack_id) {
  if (stack_id < 0 || stack_id >= stack_count()) {
    throw std::out_of_range("stack_id out of range: " +
                            std::to_string(stack_id));
  }
  return *stacks_[static_cast<std::size_t>(stack_id)];
}

const StackModel &MultiStackMemoryModel::checked_stack(int stack_id) const {
  if (stack_id < 0 || stack_id >= stack_count()) {
    throw std::out_of_range("stack_id out of range: " +
                            std::to_string(stack_id));
  }
  return *stacks_[static_cast<std::size_t>(stack_id)];
}

StackModel &MultiStackMemoryModel::stack(int stack_id) {
  return checked_stack(stack_id);
}

const StackModel &MultiStackMemoryModel::stack(int stack_id) const {
  return checked_stack(stack_id);
}

StackReadResult MultiStackMemoryModel::read(int stack_id, Address address,
                                            std::size_t size,
                                            const DecodedAddress *decoded) {
  return checked_stack(stack_id).read(address, size, decoded);
}

void MultiStackMemoryModel::write(int stack_id, Address address,
                                  const ByteVector &data,
                                  const ByteVector *mask,
                                  const DecodedAddress *decoded,
                                  std::uint64_t request_id, Cycle cycle) {
  checked_stack(stack_id).write(address, data, mask, decoded, request_id,
                                cycle);
}

StackCommandResult
MultiStackMemoryModel::issue_command(const StackCommand &command) {
  return checked_stack(command.stack_id).issue_command(command);
}

std::vector<PhysicalStorageStats>
MultiStackMemoryModel::per_stack_storage_stats() const {
  std::vector<PhysicalStorageStats> stats;
  stats.reserve(stacks_.size());
  for (const auto &stack : stacks_) {
    stats.push_back(stack->storage_stats());
  }
  return stats;
}

PhysicalStorageStats MultiStackMemoryModel::storage_stats() const {
  return merge_physical_storage_stats(per_stack_storage_stats());
}

PhysicalStorageStats
merge_physical_storage_stats(const std::vector<PhysicalStorageStats> &stats) {
  PhysicalStorageStats total;
  bool saw_temp = false;
  double weighted_temp_sum = 0.0;
  std::uint64_t temp_weight = 0;

  for (const PhysicalStorageStats &src : stats) {
    total.lines_allocated += src.lines_allocated;
    total.unique_written_lines += src.unique_written_lines;
    total.bytes_allocated += src.bytes_allocated;
    total.topology_lines_scanned += src.topology_lines_scanned;
    total.topology_scan_skipped += src.topology_scan_skipped;
    total.stacks_touched += src.stacks_touched;
    total.dies_touched += src.dies_touched;
    total.layers_touched += src.layers_touched;
    total.channels_touched += src.channels_touched;
    total.pseudo_channels_touched += src.pseudo_channels_touched;
    total.sids_touched += src.sids_touched;
    total.ranks_touched += src.ranks_touched;
    total.bank_groups_touched += src.bank_groups_touched;
    total.banks_touched += src.banks_touched;
    total.rows_touched += src.rows_touched;
    total.columns_touched += src.columns_touched;
    total.subarrays_touched += src.subarrays_touched;
    total.mats_touched += src.mats_touched;
    total.cells_touched += src.cells_touched;
    total.microbumps_touched += src.microbumps_touched;
    total.floorplan_tiles_touched += src.floorplan_tiles_touched;
    total.thermal_tiles_touched += src.thermal_tiles_touched;
    total.thermal_grid_cells_touched += src.thermal_grid_cells_touched;
    total.read_line_accesses += src.read_line_accesses;
    total.write_line_accesses += src.write_line_accesses;
    total.row_buffer_activations += src.row_buffer_activations;
    total.row_buffer_precharges += src.row_buffer_precharges;
    total.row_buffer_dirty_writebacks += src.row_buffer_dirty_writebacks;
    total.row_buffer_clean_precharges += src.row_buffer_clean_precharges;
    total.row_buffer_hits += src.row_buffer_hits;
    total.row_buffer_misses += src.row_buffer_misses;
    total.row_buffer_lazy_loads += src.row_buffer_lazy_loads;
    total.row_buffer_reads += src.row_buffer_reads;
    total.row_buffer_writes += src.row_buffer_writes;
    total.row_buffer_forced_closes += src.row_buffer_forced_closes;
    total.row_buffer_open_rows += src.row_buffer_open_rows;
    total.row_buffer_dirty_rows += src.row_buffer_dirty_rows;
    total.power_events += src.power_events;
    total.thermal_updates += src.thermal_updates;
    total.power_energy_pj += src.power_energy_pj;
    total.power_act_energy_pj += src.power_act_energy_pj;
    total.power_pre_energy_pj += src.power_pre_energy_pj;
    total.power_read_energy_pj += src.power_read_energy_pj;
    total.power_write_energy_pj += src.power_write_energy_pj;
    total.power_refresh_energy_pj += src.power_refresh_energy_pj;
    total.power_rfm_energy_pj += src.power_rfm_energy_pj;
    total.power_control_energy_pj += src.power_control_energy_pj;
    total.thermal_lateral_transfers += src.thermal_lateral_transfers;
    total.thermal_vertical_transfers += src.thermal_vertical_transfers;
    total.thermal_tsv_transfers += src.thermal_tsv_transfers;
    total.thermal_coupled_delta_c += src.thermal_coupled_delta_c;
    total.ecc_shadow_updates += src.ecc_shadow_updates;
    total.ecc_checked_reads += src.ecc_checked_reads;
    total.ecc_corrected_errors += src.ecc_corrected_errors;
    total.ecc_uncorrectable_errors += src.ecc_uncorrectable_errors;
    total.ecc_injected_errors += src.ecc_injected_errors;
    total.ecc_parity_repairs += src.ecc_parity_repairs;

    if (!saw_temp || src.thermal_peak_temp_c > total.thermal_peak_temp_c) {
      saw_temp = true;
      total.thermal_peak_temp_c = src.thermal_peak_temp_c;
      total.thermal_hotspot_layer = src.thermal_hotspot_layer;
      total.thermal_hotspot_x = src.thermal_hotspot_x;
      total.thermal_hotspot_y = src.thermal_hotspot_y;
    }

    const std::uint64_t weight =
        std::max<std::uint64_t>(1, src.thermal_grid_cells_touched);
    weighted_temp_sum += src.thermal_avg_temp_c * static_cast<double>(weight);
    temp_weight += weight;
  }

  if (temp_weight != 0) {
    total.thermal_avg_temp_c =
        weighted_temp_sum / static_cast<double>(temp_weight);
  }
  return total;
}

} // namespace hbm_sim
