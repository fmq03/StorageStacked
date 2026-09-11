// CLI 层：负责把命令行参数、配置文件和默认 preset 组装成一次完整仿真。
// 这里刻意不实现 DRAM 协议细节；所有行为差异都通过 DramSpec、ControllerOptions
// 和 TrafficOptions 传给下层模块，便于后续把 CLI 替换成脚本/GUI/CI frontend。
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "help.hpp"

#include "hbm_sim/config/document.hpp"
#include "hbm_sim/config/model.hpp"
#include "hbm_sim/config/parse.hpp"
#include "hbm_sim/config/fields.hpp"
#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/dram/interface.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/dram/profiles.hpp"
#include "hbm_sim/frontend/traffic.hpp"
#include "hbm_sim/stats/stats.hpp"
#include "hbm_sim/stats/result.hpp"
#include "hbm_sim/validation/dfi.hpp"
#include "hbm_sim/validation/trace.hpp"
#include "hbm_sim/validation/validator.hpp"

namespace {

using hbm_sim::config::is_spec_override_key;

// CLI 参数先收集到一个小结构体，再转换为 DramSpec 与 TrafficOptions。
struct Cli {
  // 默认跑 HBM4 stream，让用户直接执行二进制即可看到 HBM 双总线路径。
  std::string standard = "hbm4";
  // base standard 选择命令/状态/timing 语义；用户覆盖后模型可以是 custom，
  // 因此 model_name、preset 和 validation_mode 必须独立记录。
  std::string model_name = "default";
  std::string preset;
  std::string parameter_reference;
  std::string phy_protocol = "auto";
  hbm_sim::config::ValidationMode validation_mode =
      hbm_sim::config::ValidationMode::Exploratory;
  int config_schema_version = 1;
  bool check_config = false;
  bool list_presets = false;
  bool list_schedulers = false;
  bool list_row_policies = false;
  bool list_backends = false;
  bool list_phy_modes = false;
  bool compare_preset = false;
  std::string dump_resolved_config_path;
  std::string dump_config_diff_path;
  std::string explain_config_key;
  std::vector<hbm_sim::config::ConfigDocument> config_documents;
  std::vector<hbm_sim::config::ConfigEntry> resolved_config_entries;
  std::vector<hbm_sim::config::ParameterDerivation> derived_parameters;
  std::string pattern = "stream";
  std::string trace_path;
  std::uint64_t requests = 10000;
  int read_ratio = 100;
  std::uint64_t seed = 1;
  std::uint64_t random_address_space_bytes = 0;
  std::uint64_t addr_stride = 64;
  hbm_sim::Cycle inject_interval = 0;
  std::string init_sequence = "none";
  hbm_sim::Cycle init_sequence_interval = 1;
  hbm_sim::Cycle max_cycles = 100000000;
  hbm_sim::Cycle progress_interval = 0;
  // summary is the compact public report; diagnostic is an explicit audit view.
  std::string stats_view = "summary";
  std::string stats_json_path;
  bool single_controller = false;
  bool strict_timing_table = false;
  std::string cmd_trace_path;
  std::string response_trace_path;
  std::string transaction_response_trace_path;
  hbm_sim::ResponseDeliveryMode response_delivery_mode =
      hbm_sim::ResponseDeliveryMode::Disabled;
  bool response_delivery_mode_explicit = false;
  std::size_t host_response_queue_capacity = 0;
  std::size_t transaction_response_queue_capacity = 0;
  std::string dfi_trace_path;
  std::string dfi_signal_trace_path;
  std::string timing_table_path;
  bool validate_cmd_trace = false;
  bool validate_dfi_trace = false;
  bool fail_on_data_mismatch = true;
  std::string memory_image_path;
  std::string dump_memory_image_path;
  std::string dump_memory_csv_path;
  std::string mismatch_report_path;
  std::string thermal_map_path;
  std::string verify_golden_path;
  hbm_sim::StorageModelOptions storage_model;
  hbm_sim::ChannelMapperKind channel_mapper = hbm_sim::ChannelMapperKind::Decoded;
  int stack_count = 1;
  hbm_sim::StackMappingKind stack_mapping = hbm_sim::StackMappingKind::Interleaved;
  std::uint64_t stack_interleave_bytes = 256;
  std::size_t stack_ingress_buffer_size = 256;
  std::size_t stack_dispatch_width = 4;
  hbm_sim::StackQosPolicy stack_qos_policy = hbm_sim::StackQosPolicy::StrictPriority;
  hbm_sim::ControllerOptions controller;
  std::vector<std::pair<std::string, std::string>> spec_overrides;
};

using hbm_sim::config::parse_u64;
using hbm_sim::config::parse_int;
using hbm_sim::config::parse_double;
using hbm_sim::config::parse_bool;
using hbm_sim::config::lower_value;

hbm_sim::ResponseDeliveryMode parse_response_delivery_mode(const std::string& value);

hbm_sim::SchedulerKind parse_scheduler(std::string value) {
  value = lower_value(std::move(value));
  if (value == "frfcfs" || value == "fr_fcfs") return hbm_sim::SchedulerKind::FRFCFS;
  if (value == "fcfs") return hbm_sim::SchedulerKind::FCFS;
  throw std::invalid_argument("invalid scheduler: " + value +
                              " (implemented: fcfs, frfcfs)");
}

hbm_sim::RowPolicyKind parse_row_policy(std::string value) {
  value = lower_value(std::move(value));
  if (value == "open" || value == "open_page") return hbm_sim::RowPolicyKind::OpenPage;
  if (value == "closed" || value == "closed_page") return hbm_sim::RowPolicyKind::ClosedPage;
  if (value == "closed_cap" || value == "closedcap") return hbm_sim::RowPolicyKind::ClosedCap;
  throw std::invalid_argument("invalid row_policy: " + value +
                              " (implemented: open_page, closed_page, closed_cap)");
}

hbm_sim::ChannelMapperKind parse_channel_mapper(std::string value) {
  value = lower_value(std::move(value));
  if (value == "decoded" || value == "address") return hbm_sim::ChannelMapperKind::Decoded;
  if (value == "round_robin" || value == "rr") return hbm_sim::ChannelMapperKind::RoundRobin;
  if (value == "xor" || value == "xor_lowbits") return hbm_sim::ChannelMapperKind::Xor;
  throw std::invalid_argument("invalid channel_mapper: " + value);
}

hbm_sim::StackMappingKind parse_stack_mapping(std::string value) {
  value = lower_value(std::move(value));
  if (value == "interleaved" || value == "stripe") return hbm_sim::StackMappingKind::Interleaved;
  if (value == "blocked" || value == "contiguous") return hbm_sim::StackMappingKind::Blocked;
  throw std::invalid_argument("invalid stack_mapping: " + value);
}

const char* stack_mapping_name(hbm_sim::StackMappingKind kind) {
  return kind == hbm_sim::StackMappingKind::Blocked ? "blocked" : "interleaved";
}

hbm_sim::StackQosPolicy parse_stack_qos_policy(std::string value) {
  value = lower_value(std::move(value));
  if (value == "fcfs") return hbm_sim::StackQosPolicy::Fcfs;
  if (value == "strict_priority" || value == "priority") {
    return hbm_sim::StackQosPolicy::StrictPriority;
  }
  throw std::invalid_argument("invalid stack_qos_policy: " + value);
}

const char* stack_qos_policy_name(hbm_sim::StackQosPolicy policy) {
  return policy == hbm_sim::StackQosPolicy::Fcfs ? "fcfs" : "strict_priority";
}

std::string stack_path(const std::string& path, int stack, int stack_count) {
  if (path.empty()) return path;
  const std::string marker = "{stack}";
  std::string result = path;
  std::size_t pos = result.find(marker);
  if (pos != std::string::npos) {
    result.replace(pos, marker.size(), std::to_string(stack));
    return result;
  }
  if (stack_count == 1) return path;
  std::size_t slash = result.find_last_of("/\\");
  std::size_t dot = result.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return result + ".stack" + std::to_string(stack);
  }
  return result.substr(0, dot) + ".stack" + std::to_string(stack) + result.substr(dot);
}

void ensure_parent_directory(const std::string& path) {
  if (path.empty()) return;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error) {
    throw std::runtime_error("failed to create output directory " +
                             parent.string() + ": " + error.message());
  }
}

void prepare_stack_output_directories(const std::string& path,
                                      int stack_count) {
  for (int stack = 0; stack < std::max(1, stack_count); stack++) {
    ensure_parent_directory(stack_path(path, stack, stack_count));
  }
}

std::string normalize_key(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return key;
}

bool apply_storage_model_option(hbm_sim::StorageModelOptions& options,
                                const std::string& key,
                                const std::string& value) {
  if (key == "memory_backend" || key == "storage_backend") {
    options.memory_backend.kind = hbm_sim::parse_memory_backend_kind(value);
  } else if (key == "memory_capacity_bytes" || key == "storage_capacity_bytes") {
    options.memory_backend.capacity_bytes = parse_u64(value);
  } else if (key == "memory_data_file") {
    options.memory_backend.data_file = value;
  } else if (key == "memory_init_file") {
    options.memory_backend.init_file = value;
  } else if (key == "memory_meta_file") {
    options.memory_backend.meta_file = value;
  } else if (key == "memory_presence_file") {
    options.memory_backend.presence_file = value;
  } else if (key == "memory_chunk_size" || key == "memory_chunk_size_bytes") {
    options.memory_backend.chunk_size_bytes = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "memory_chunk_cache_entries") {
    options.memory_backend.chunk_cache_entries = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "sparse_density_warning_pct") {
    options.sparse_density_warning_pct = parse_double(value);
  } else if (key == "topology_stats_scan_limit") {
    options.topology_stats_scan_limit = parse_u64(value);
  } else if (key == "floorplan" || key == "floorplan_enabled" || key == "storage_floorplan") {
    options.floorplan_enabled = parse_bool(value);
  } else if (key == "power_model" || key == "power_enabled") {
    options.power_enabled = parse_bool(value);
  } else if (key == "thermal_model" || key == "thermal_enabled") {
    options.thermal_enabled = parse_bool(value);
  } else if (key == "power_source" || key == "power_calibration") {
    options.power_source = lower_value(value);
  } else if (key == "power_scale") {
    options.power_scale = parse_double(value);
  } else if (key == "thermal_ambient_c") {
    options.thermal_ambient_c = parse_double(value);
  } else if (key == "thermal_cooling_per_cycle") {
    options.thermal_cooling_per_cycle = parse_double(value);
  } else if (key == "thermal_rise_c_per_pj") {
    options.thermal_rise_c_per_pj = parse_double(value);
  } else if (key == "thermal_grid_cols_per_tile" || key == "thermal_num_x_grids") {
    options.thermal_grid_cols_per_tile = parse_int(value);
  } else if (key == "thermal_grid_rows_per_tile" || key == "thermal_num_y_grids") {
    options.thermal_grid_rows_per_tile = parse_int(value);
  } else if (key == "thermal_coupling" || key == "thermal_coupling_enabled") {
    options.thermal_coupling_enabled = parse_bool(value);
  } else if (key == "thermal_lateral_coupling") {
    options.thermal_lateral_coupling = parse_double(value);
  } else if (key == "thermal_vertical_coupling") {
    options.thermal_vertical_coupling = parse_double(value);
  } else if (key == "thermal_tsv_coupling_scale") {
    options.thermal_tsv_coupling_scale = parse_double(value);
  } else if (key == "thermal_tsvs_per_grid" || key == "tsvs_per_grid") {
    options.thermal_tsvs_per_grid = parse_int(value);
  } else if (key == "thermal_chip_dim_x_m" || key == "chip_dim_x") {
    options.thermal_chip_dim_x_m = parse_double(value);
  } else if (key == "thermal_chip_dim_y_m" || key == "chip_dim_y") {
    options.thermal_chip_dim_y_m = parse_double(value);
  } else if (key == "thermal_tsv_radius_m" || key == "r_tsv") {
    options.thermal_tsv_radius_m = parse_double(value);
  } else if (key == "thermal_k_silicon" || key == "ksi") {
    options.thermal_k_silicon = parse_double(value);
  } else if (key == "thermal_k_copper" || key == "kcu") {
    options.thermal_k_copper = parse_double(value);
  } else if (key == "subarrays_per_bank") {
    options.subarrays_per_bank = parse_int(value);
  } else if (key == "mats_per_subarray_x") {
    options.mats_per_subarray_x = parse_int(value);
  } else if (key == "mats_per_subarray_y") {
    options.mats_per_subarray_y = parse_int(value);
  } else if (key == "cells_per_mat_x") {
    options.cells_per_mat_x = parse_int(value);
  } else if (key == "cells_per_mat_y") {
    options.cells_per_mat_y = parse_int(value);
  } else if (key == "microbumps_x") {
    options.microbumps_x = parse_int(value);
  } else if (key == "microbumps_y") {
    options.microbumps_y = parse_int(value);
  } else if (key == "ecc_shadow" || key == "ecc_shadow_enabled") {
    options.ecc_shadow_enabled = parse_bool(value);
  } else if (key == "ecc_check_on_read") {
    options.ecc_check_on_read = parse_bool(value);
  } else if (key == "ecc_correct_single_bit") {
    options.ecc_correct_single_bit = parse_bool(value);
  } else if (key == "ecc_inject_period") {
    options.ecc_inject_period = parse_int(value);
  } else if (key == "power_vdd" || key == "vdd") {
    options.idd_vdd = parse_double(value);
  } else if (key == "idd0") {
    options.idd0_ma = parse_double(value);
  } else if (key == "idd2n") {
    options.idd2n_ma = parse_double(value);
  } else if (key == "idd3n") {
    options.idd3n_ma = parse_double(value);
  } else if (key == "idd4r") {
    options.idd4r_ma = parse_double(value);
  } else if (key == "idd4w") {
    options.idd4w_ma = parse_double(value);
  } else if (key == "idd5ab") {
    options.idd5ab_ma = parse_double(value);
  } else if (key == "idd5pb") {
    options.idd5pb_ma = parse_double(value);
  } else if (key == "idd6x") {
    options.idd6x_ma = parse_double(value);
  } else if (key == "idd_devices_per_rank" || key == "devices_per_rank") {
    options.idd_devices_per_rank = parse_double(value);
  } else if (key == "idd_burst_cycles" || key == "burst_cycle") {
    options.idd_burst_cycles = parse_double(value);
  } else if (key == "power_act_pj") {
    options.act_energy_pj = parse_double(value);
  } else if (key == "power_act1_pj") {
    options.act1_energy_pj = parse_double(value);
  } else if (key == "power_act2_pj") {
    options.act2_energy_pj = parse_double(value);
  } else if (key == "power_pre_pj") {
    options.pre_energy_pj = parse_double(value);
  } else if (key == "power_preab_pj") {
    options.preab_energy_pj = parse_double(value);
  } else if (key == "power_cas_pj") {
    options.cas_energy_pj = parse_double(value);
  } else if (key == "power_read_pj") {
    options.read_energy_pj = parse_double(value);
  } else if (key == "power_read_per_byte_pj") {
    options.read_energy_per_byte_pj = parse_double(value);
  } else if (key == "power_write_pj") {
    options.write_energy_pj = parse_double(value);
  } else if (key == "power_write_per_byte_pj") {
    options.write_energy_per_byte_pj = parse_double(value);
  } else if (key == "power_refpb_pj") {
    options.refpb_energy_pj = parse_double(value);
  } else if (key == "power_refdb_pj") {
    options.refdb_energy_pj = parse_double(value);
  } else if (key == "power_refab_pj") {
    options.refab_energy_pj = parse_double(value);
  } else if (key == "power_rfmpb_pj") {
    options.rfmpb_energy_pj = parse_double(value);
  } else if (key == "power_rfmab_pj") {
    options.rfmab_energy_pj = parse_double(value);
  } else if (key == "power_control_pj") {
    options.control_energy_pj = parse_double(value);
  } else {
    return false;
  }
  return true;
}

void apply_option(Cli& cli, const std::string& raw_key, const std::string& value) {
  std::string key = normalize_key(raw_key);
  // apply_option() 同时服务 CLI 和 key=value 配置文件。为了让二者语义一致，
  // CLI 解析层只把 "--foo value" 转换成 ("foo", value)，所有真正赋值都放在这里。
  //
  // DramSpec 相关字段不会立即改 spec，因为 spec 需要先由 --standard 选择 preset。
  // 这些覆盖项先进入 cli.spec_overrides，等 make_spec() 后再统一应用。
  if (key == "standard") {
    cli.standard = value;
  } else if (key == "model_name") {
    cli.model_name = value;
  } else if (key == "preset") {
    cli.preset = value;
  } else if (key == "parameter_reference") {
    cli.parameter_reference = value;
  } else if (key == "validation_mode") {
    cli.validation_mode = hbm_sim::config::parse_validation_mode(value);
  } else if (key == "schema_version") {
    cli.config_schema_version = parse_int(value);
    if (cli.config_schema_version < 1 || cli.config_schema_version > 3) {
      throw std::invalid_argument("unsupported config schema_version: " + value);
    }
  } else if (key == "phy_protocol") {
    cli.phy_protocol = lower_value(value);
  } else if (key == "pattern") {
    cli.pattern = value;
  } else if (key == "trace" || key == "trace_path") {
    cli.trace_path = value;
  } else if (key == "requests") {
    cli.requests = parse_u64(value);
  } else if (key == "read_ratio") {
    cli.read_ratio = parse_int(value);
  } else if (key == "seed") {
    cli.seed = parse_u64(value);
  } else if (key == "random_address_space_bytes") {
    cli.random_address_space_bytes = parse_u64(value);
  } else if (key == "addr_stride") {
    cli.addr_stride = parse_u64(value);
  } else if (key == "inject_interval") {
    cli.inject_interval = parse_u64(value);
  } else if (key == "init_sequence") {
    cli.init_sequence = value;
  } else if (key == "init_sequence_interval") {
    cli.init_sequence_interval = parse_u64(value);
  } else if (key == "read_buffer_size") {
    cli.controller.read_buffer_size = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "write_buffer_size") {
    cli.controller.write_buffer_size = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "priority_buffer_size") {
    cli.controller.priority_buffer_size = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "mem_phy_mode" || key == "phy_mode") {
    cli.controller.phy.mode = hbm_sim::parse_mem_phy_mode(value);
  } else if (key == "dfi_version" || key == "phy_dfi_version") {
    cli.controller.phy.dfi_version = value;
  } else if (key == "phy_command_fifo_depth") {
    cli.controller.phy.command_fifo_depth = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "phy_read_fifo_depth") {
    cli.controller.phy.read_fifo_depth = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "phy_write_fifo_depth") {
    cli.controller.phy.write_fifo_depth = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "phy_command_pipeline_cycles") {
    cli.controller.phy.command_pipeline_cycles = parse_int(value);
  } else if (key == "phy_read_return_pipeline_cycles") {
    cli.controller.phy.read_return_pipeline_cycles = parse_int(value);
  } else if (key == "phy_write_data_pipeline_cycles") {
    cli.controller.phy.write_data_pipeline_cycles = parse_int(value);
  } else if (key == "phy_reset_cycles") {
    cli.controller.phy.reset_cycles = parse_int(value);
  } else if (key == "phy_initialization_cycles") {
    cli.controller.phy.initialization_cycles = parse_int(value);
  } else if (key == "phy_training_cycles") {
    cli.controller.phy.training_cycles = parse_int(value);
  } else if (key == "phy_auto_train") {
    cli.controller.phy.auto_train = parse_bool(value);
  } else if (key == "write_low_watermark") {
    cli.controller.write_low_watermark = parse_double(value);
  } else if (key == "write_high_watermark") {
    cli.controller.write_high_watermark = parse_double(value);
  } else if (key == "max_cycles") {
    cli.max_cycles = parse_u64(value);
  } else if (key == "stats_json") {
    cli.stats_json_path = value;
  } else if (key == "dump_resolved_config") {
    cli.dump_resolved_config_path = value;
  } else if (key == "stats_view") {
    cli.stats_view = lower_value(value);
    if (cli.stats_view != "summary" && cli.stats_view != "diagnostic") {
      throw std::invalid_argument(
          "invalid stats_view: " + value + " (expected summary or diagnostic)");
    }
  } else if (key == "progress_interval") {
    cli.progress_interval = parse_u64(value);
  } else if (key == "single_controller") {
    cli.single_controller = parse_bool(value);
  } else if (key == "scheduler") {
    cli.controller.scheduler = parse_scheduler(value);
  } else if (key == "row_policy") {
    cli.controller.row_policy = parse_row_policy(value);
  } else if (key == "row_policy_cap") {
    cli.controller.row_policy_cap = parse_int(value);
  } else if (key == "channel_mapper") {
    cli.channel_mapper = parse_channel_mapper(value);
  } else if (key == "stack_count") {
    cli.stack_count = parse_int(value);
  } else if (key == "stack_mapping") {
    cli.stack_mapping = parse_stack_mapping(value);
  } else if (key == "stack_interleave_bytes") {
    cli.stack_interleave_bytes = parse_u64(value);
  } else if (key == "stack_ingress_buffer_size") {
    cli.stack_ingress_buffer_size = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "stack_dispatch_width") {
    cli.stack_dispatch_width = static_cast<std::size_t>(parse_u64(value));
  } else if (key == "stack_qos_policy") {
    cli.stack_qos_policy = parse_stack_qos_policy(value);
  } else if (key == "strict_timing_table") {
    cli.strict_timing_table = parse_bool(value);
  } else if (key == "cmd_trace" || key == "cmd_trace_path") {
    cli.cmd_trace_path = value;
  } else if (key == "response_trace" || key == "response_trace_path" ||
             key == "host_response_trace") {
    cli.response_trace_path = value;
  } else if (key == "transaction_response_trace" ||
             key == "transaction_response_trace_path") {
    cli.transaction_response_trace_path = value;
  } else if (key == "response_delivery_mode") {
    cli.response_delivery_mode = parse_response_delivery_mode(value);
    cli.response_delivery_mode_explicit = true;
  } else if (key == "host_response_queue_capacity") {
    cli.host_response_queue_capacity =
        static_cast<std::size_t>(parse_u64(value));
  } else if (key == "transaction_response_queue_capacity") {
    cli.transaction_response_queue_capacity =
        static_cast<std::size_t>(parse_u64(value));
  } else if (key == "dfi_trace" || key == "dfi_trace_path" || key == "dump_dfi_trace") {
    cli.dfi_trace_path = value;
  } else if (key == "dfi_signal_trace" || key == "dfi_signal_trace_path" ||
             key == "dump_dfi_signal_trace") {
    cli.dfi_signal_trace_path = value;
  } else if (key == "dump_timing_table" || key == "timing_table_path") {
    cli.timing_table_path = value;
  } else if (key == "validate_cmd_trace" || key == "validate_command_trace") {
    cli.validate_cmd_trace = parse_bool(value);
  } else if (key == "validate_dfi_trace") {
    cli.validate_dfi_trace = parse_bool(value);
  } else if (key == "fail_on_data_mismatch") {
    cli.fail_on_data_mismatch = parse_bool(value);
  } else if (key == "memory_image" || key == "memory_image_path") {
    cli.memory_image_path = value;
  } else if (key == "dump_memory_image" || key == "dump_memory_image_path" ||
             key == "final_memory" || key == "final_memory_path") {
    cli.dump_memory_image_path = value;
  } else if (key == "dump_memory_csv" || key == "dump_memory_csv_path" ||
             key == "final_memory_csv" || key == "final_memory_csv_path") {
    cli.dump_memory_csv_path = value;
  } else if (key == "mismatch_report" || key == "mismatch_report_path") {
    cli.mismatch_report_path = value;
  } else if (key == "verify_golden" || key == "verify_golden_path" || key == "golden") {
    cli.verify_golden_path = value;
  } else if (key == "dump_thermal_map" || key == "thermal_map" || key == "thermal_map_path") {
    cli.thermal_map_path = value;
  } else if (apply_storage_model_option(cli.storage_model, key, value)) {
    return;
  } else if (is_spec_override_key(key)) {
    cli.spec_overrides.emplace_back(key, value);
  } else {
    throw std::invalid_argument("unknown config key: " + raw_key);
  }
}

void apply_config_documents(Cli& cli,
                            const std::vector<hbm_sim::config::ConfigDocument>& documents,
                            const hbm_sim::config::Selection& selection) {
  const auto entries = hbm_sim::config::resolve_documents(documents, selection);
  for (const auto& entry : entries) {
    try {
      apply_option(cli, entry.key, entry.value);
      if (hbm_sim::config::is_timing_override_key(entry.key))
        cli.spec_overrides.emplace_back("timing_source." + entry.key, entry.timing_source);
    } catch (const std::exception& error) {
      const std::string section = entry.section.empty() ? "" : " [" + entry.section + "]";
      throw std::runtime_error(entry.path + ":" + std::to_string(entry.line) + section +
                               ": " + error.what());
    }
    cli.resolved_config_entries.push_back(entry);
  }
}

const char* response_delivery_mode_name(hbm_sim::ResponseDeliveryMode mode) {
  switch (mode) {
    case hbm_sim::ResponseDeliveryMode::Disabled:
      return "disabled";
    case hbm_sim::ResponseDeliveryMode::HostOnly:
      return "host";
    case hbm_sim::ResponseDeliveryMode::TransactionOnly:
      return "transaction";
    case hbm_sim::ResponseDeliveryMode::Both:
      return "both";
  }
  return "disabled";
}

hbm_sim::ResponseDeliveryMode parse_response_delivery_mode(
    const std::string& value) {
  const std::string normalized = lower_value(value);
  if (normalized == "disabled" || normalized == "off" ||
      normalized == "none") {
    return hbm_sim::ResponseDeliveryMode::Disabled;
  }
  if (normalized == "host" || normalized == "host_only" ||
      normalized == "hostonly") {
    return hbm_sim::ResponseDeliveryMode::HostOnly;
  }
  if (normalized == "transaction" || normalized == "transaction_only" ||
      normalized == "transactiononly") {
    return hbm_sim::ResponseDeliveryMode::TransactionOnly;
  }
  if (normalized == "both") return hbm_sim::ResponseDeliveryMode::Both;
  throw std::invalid_argument(
      "invalid response_delivery_mode: " + value +
      " (expected disabled, host, transaction, or both)");
}

bool response_mode_has_host(hbm_sim::ResponseDeliveryMode mode) {
  return mode == hbm_sim::ResponseDeliveryMode::HostOnly ||
         mode == hbm_sim::ResponseDeliveryMode::Both;
}

bool response_mode_has_transaction(hbm_sim::ResponseDeliveryMode mode) {
  return mode == hbm_sim::ResponseDeliveryMode::TransactionOnly ||
         mode == hbm_sim::ResponseDeliveryMode::Both;
}

void resolve_response_delivery(Cli& cli) {
  const bool wants_host_trace = !cli.response_trace_path.empty();
  const bool wants_transaction_trace =
      !cli.transaction_response_trace_path.empty();
  // disabled 是零成本默认值；一旦用户要求导出 trace，就按 trace 推导实际
  // 视图。这也保证 dump-resolved-config 中的 disabled 不会阻止后续追加 trace。
  if (!cli.response_delivery_mode_explicit ||
      cli.response_delivery_mode == hbm_sim::ResponseDeliveryMode::Disabled) {
    if (wants_host_trace && wants_transaction_trace) {
      cli.response_delivery_mode = hbm_sim::ResponseDeliveryMode::Both;
    } else if (wants_host_trace) {
      cli.response_delivery_mode = hbm_sim::ResponseDeliveryMode::HostOnly;
    } else if (wants_transaction_trace) {
      cli.response_delivery_mode =
          hbm_sim::ResponseDeliveryMode::TransactionOnly;
    }
  }
  if (wants_host_trace &&
      !response_mode_has_host(cli.response_delivery_mode)) {
    throw std::invalid_argument(
        "host response_trace requires response_delivery_mode=host or both");
  }
  if (wants_transaction_trace &&
      !response_mode_has_transaction(cli.response_delivery_mode)) {
    throw std::invalid_argument(
        "transaction_response_trace requires "
        "response_delivery_mode=transaction or both");
  }
}

std::string csv_escape(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string bytes_to_hex(const hbm_sim::ByteVector& bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::uint8_t byte : bytes) {
    out << std::setw(2) << static_cast<unsigned>(byte);
  }
  return out.str();
}

std::string address_to_hex(hbm_sim::Address address) {
  std::ostringstream out;
  out << "0x" << std::hex << address;
  return out.str();
}

void write_host_response_csv_header(std::ostream& out) {
  out << "host_request_id,type,system_address,transaction_count,stack,channel,"
         "arrival_cycle,first_issued_cycle,completion_cycle,latency_cycles,status,"
         "initialized,ecc_corrected,ecc_uncorrectable,forwarded,coalesced,"
         "data_hex,initialized_mask_hex\n";
}

void write_host_response_csv_row(std::ostream& out,
                                 const hbm_sim::HostResponse& response) {
  const hbm_sim::Cycle latency =
      response.completion_cycle >= response.arrival_cycle
          ? response.completion_cycle - response.arrival_cycle
          : 0;
  out << response.host_request_id << ','
      << hbm_sim::to_string(response.type) << ','
      << address_to_hex(response.system_address) << ','
      << response.transaction_count << ',' << response.stack << ','
      << response.channel << ',' << response.arrival_cycle << ','
      << response.first_issued_cycle << ',' << response.completion_cycle << ','
      << latency << ',' << hbm_sim::response_status_name(response.status) << ','
      << (response.initialized ? "true" : "false") << ','
      << (response.ecc_corrected ? "true" : "false") << ','
      << (response.ecc_uncorrectable ? "true" : "false") << ','
      << (response.forwarded ? "true" : "false") << ','
      << (response.coalesced ? "true" : "false") << ','
      << bytes_to_hex(response.data) << ','
      << bytes_to_hex(response.initialized_mask) << '\n';
  if (!out) {
    throw std::runtime_error("failed while writing host response trace");
  }
}

void write_transaction_response_csv_header(std::ostream& out) {
  out << "request_id,host_request_id,transaction_index,transaction_count,type,"
         "system_address,local_address,stack,channel,arrival_cycle,issued_cycle,"
         "completion_cycle,latency_cycles,status,initialized,ecc_corrected,"
         "ecc_uncorrectable,forwarded,coalesced,data_hex,initialized_mask_hex\n";
}

void write_transaction_response_csv_row(
    std::ostream& out, const hbm_sim::TransactionResponse& response) {
  const hbm_sim::Cycle latency =
      response.completion_cycle >= response.arrival_cycle
          ? response.completion_cycle - response.arrival_cycle
          : 0;
  out << response.request_id << ',' << response.host_request_id << ','
      << response.transaction_index << ',' << response.transaction_count << ','
      << hbm_sim::to_string(response.type) << ','
      << address_to_hex(response.system_address) << ','
      << address_to_hex(response.local_address) << ',' << response.stack << ','
      << response.channel << ',' << response.arrival_cycle << ','
      << response.issued_cycle << ',' << response.completion_cycle << ','
      << latency << ',' << hbm_sim::response_status_name(response.status) << ','
      << (response.initialized ? "true" : "false") << ','
      << (response.ecc_corrected ? "true" : "false") << ','
      << (response.ecc_uncorrectable ? "true" : "false") << ','
      << (response.forwarded ? "true" : "false") << ','
      << (response.coalesced ? "true" : "false") << ','
      << bytes_to_hex(response.data) << ','
      << bytes_to_hex(response.initialized_mask) << '\n';
  if (!out) {
    throw std::runtime_error(
        "failed while writing transaction response trace");
  }
}

void write_timing_table_csv(const std::string& path, const hbm_sim::TimingTable& table) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open timing table output: " + path);
  }
  out << "preset,name,value_nck,source,vendor_required_for_numeric,required_for_model,note\n";
  for (const auto& entry : table.entries) {
    // CSV 导出保留 source 和 note，是为了让后续“标准手册/供应商表补齐”变成
    // 可检查的流程：grep research_default 即可看到还不能用于数值级对比的项。
    out << csv_escape(table.preset_name) << ','
        << csv_escape(entry.name) << ','
        << entry.value_nck << ','
        << csv_escape(hbm_sim::to_string(entry.source)) << ','
        << (entry.vendor_required_for_numeric ? "true" : "false") << ','
        << (entry.required_for_model ? "true" : "false") << ','
        << csv_escape(entry.note) << '\n';
  }
}

std::vector<std::string> validate_protocol_config(const hbm_sim::DramSpec& spec) {
  std::vector<std::string> errors;
  if (spec.org.line_size <= 0) {
    errors.push_back("line_size must be > 0");
  }
  if (spec.org.dram_transaction_bytes < 0) {
    errors.push_back("dram_transaction_bytes must be >= 0");
  }
  if (spec.org.dram_transaction_bytes > 0 &&
      spec.org.line_size % spec.org.dram_transaction_bytes != 0) {
    errors.push_back("line_size must be a multiple of dram_transaction_bytes");
  }
  if (spec.dfi_phase_count < 0) {
    errors.push_back("dfi_phase_count must be >= 0");
  }
  if (spec.dfi_data_lane_bytes < 0) {
    errors.push_back("dfi_data_lane_bytes must be >= 0");
  }
  if (spec.dfi_read_latency_nck < 0) {
    errors.push_back("dfi_read_latency_nck must be >= 0");
  }
  if (spec.dfi_write_latency_nck < 0) {
    errors.push_back("dfi_write_latency_nck must be >= 0");
  }
  if (spec.split_activate) {
    if (spec.timing.nAADMin <= 0) {
      errors.push_back("nAADMin must be > 0 for split activate");
    }
    if (spec.timing.nAADMax < spec.timing.nAADMin) {
      errors.push_back("nAADMax must be >= nAADMin for split activate");
    }
  }
  if (spec.lpddr_dual_bank_refresh &&
      (spec.org.bank_groups < 2 || (spec.org.bank_groups % 2) != 0)) {
    errors.push_back(
        "LPDDR REFdb adjacent-BG pair table requires an even bank_groups >= 2");
  }
  if (spec.lpddr_wck_mode == hbm_sim::LpddrWckMode::BurstSync) {
    errors.push_back(
        "lpddr_wck_mode=burst_sync is reserved and has no executable semantics");
  }
  if (spec.lpddr_ca_parity_enabled) {
    // JESD209-6 CA Parity Check Mode 是 LPDDR6 命令/地址总线保护特性：
    // 启用时要求 WCK Always ON，且 mode 不能 on-the-fly 切换。本模拟器把
    // CA parity 建模成仿真开始前的静态 mode，因此这里做强校验。
    if (!spec.lpddr_family) {
      errors.push_back("lpddr_ca_parity_enabled requires an LPDDR-family standard");
    }
    if (spec.lpddr_wck_mode != hbm_sim::LpddrWckMode::AlwaysOn) {
      errors.push_back("LPDDR6 CA parity requires lpddr_wck_mode=always_on");
    }
    if (spec.lpddr_ca_parity_bits_per_command <= 0) {
      errors.push_back("lpddr_ca_parity_bits_per_command must be > 0 when CA parity is enabled");
    }
  }
  return errors;
}

std::vector<std::string> validate_storage_model_config(const hbm_sim::StorageModelOptions& options) {
  std::vector<std::string> errors;
  auto require_non_negative = [&](double value, const char* name) {
    if (value < 0.0) {
      errors.push_back(std::string(name) + " must be >= 0");
    }
  };
  if (options.memory_backend.kind != hbm_sim::MemoryBackendKind::Sparse &&
      options.memory_backend.data_file.empty()) {
    errors.push_back("mmap_sparse/chunk_file memory backend requires memory_data_file");
  }
  if (options.memory_backend.chunk_size_bytes == 0) {
    errors.push_back("memory_chunk_size must be > 0");
  }
  if (options.memory_backend.chunk_cache_entries == 0) {
    errors.push_back("memory_chunk_cache_entries must be > 0");
  }
  if (options.sparse_density_warning_pct < 0.0 ||
      options.sparse_density_warning_pct > 100.0) {
    errors.push_back("sparse_density_warning_pct must be in [0, 100]");
  }

  std::string power_source = lower_value(options.power_source);
  if (power_source != "configured_pj" && power_source != "manual" &&
      power_source != "idd" && power_source != "dramsim3_idd" && power_source != "dramsim3") {
    errors.push_back("power_source must be configured_pj or dramsim3_idd");
  }
  if (options.thermal_grid_cols_per_tile <= 0) {
    errors.push_back("thermal_grid_cols_per_tile must be > 0");
  }
  if (options.thermal_grid_rows_per_tile <= 0) {
    errors.push_back("thermal_grid_rows_per_tile must be > 0");
  }
  auto require_positive_int = [&](int value, const char* name) {
    if (value <= 0) {
      errors.push_back(std::string(name) + " must be > 0");
    }
  };

  require_non_negative(options.power_scale, "power_scale");
  require_non_negative(options.thermal_cooling_per_cycle, "thermal_cooling_per_cycle");
  require_non_negative(options.thermal_rise_c_per_pj, "thermal_rise_c_per_pj");
  require_non_negative(options.thermal_lateral_coupling, "thermal_lateral_coupling");
  require_non_negative(options.thermal_vertical_coupling, "thermal_vertical_coupling");
  require_non_negative(options.thermal_tsv_coupling_scale, "thermal_tsv_coupling_scale");
  require_positive_int(options.thermal_tsvs_per_grid, "thermal_tsvs_per_grid");
  require_non_negative(options.thermal_chip_dim_x_m, "thermal_chip_dim_x_m");
  require_non_negative(options.thermal_chip_dim_y_m, "thermal_chip_dim_y_m");
  require_non_negative(options.thermal_tsv_radius_m, "thermal_tsv_radius_m");
  require_non_negative(options.thermal_k_silicon, "thermal_k_silicon");
  require_non_negative(options.thermal_k_copper, "thermal_k_copper");
  require_positive_int(options.subarrays_per_bank, "subarrays_per_bank");
  require_positive_int(options.mats_per_subarray_x, "mats_per_subarray_x");
  require_positive_int(options.mats_per_subarray_y, "mats_per_subarray_y");
  require_positive_int(options.cells_per_mat_x, "cells_per_mat_x");
  require_positive_int(options.cells_per_mat_y, "cells_per_mat_y");
  require_positive_int(options.microbumps_x, "microbumps_x");
  require_positive_int(options.microbumps_y, "microbumps_y");
  if (options.ecc_inject_period < 0) {
    errors.push_back("ecc_inject_period must be >= 0");
  }
  require_non_negative(options.idd_vdd, "power_vdd");
  require_non_negative(options.idd0_ma, "IDD0");
  require_non_negative(options.idd2n_ma, "IDD2N");
  require_non_negative(options.idd3n_ma, "IDD3N");
  require_non_negative(options.idd4r_ma, "IDD4R");
  require_non_negative(options.idd4w_ma, "IDD4W");
  require_non_negative(options.idd5ab_ma, "IDD5AB");
  require_non_negative(options.idd5pb_ma, "IDD5PB");
  require_non_negative(options.idd6x_ma, "IDD6x");
  require_non_negative(options.idd_devices_per_rank, "idd_devices_per_rank");
  if ((power_source == "idd" || power_source == "dramsim3_idd" ||
       power_source == "dramsim3") &&
      options.idd_devices_per_rank <= 0.0) {
    errors.push_back("idd_devices_per_rank must be > 0 for IDD power mode");
  }
  require_non_negative(options.idd_burst_cycles, "idd_burst_cycles");
  require_non_negative(options.act_energy_pj, "power_act_pj");
  require_non_negative(options.act1_energy_pj, "power_act1_pj");
  require_non_negative(options.act2_energy_pj, "power_act2_pj");
  require_non_negative(options.pre_energy_pj, "power_pre_pj");
  require_non_negative(options.preab_energy_pj, "power_preab_pj");
  require_non_negative(options.cas_energy_pj, "power_cas_pj");
  require_non_negative(options.read_energy_pj, "power_read_pj");
  require_non_negative(options.read_energy_per_byte_pj, "power_read_per_byte_pj");
  require_non_negative(options.write_energy_pj, "power_write_pj");
  require_non_negative(options.write_energy_per_byte_pj, "power_write_per_byte_pj");
  require_non_negative(options.refpb_energy_pj, "power_refpb_pj");
  require_non_negative(options.refdb_energy_pj, "power_refdb_pj");
  require_non_negative(options.refab_energy_pj, "power_refab_pj");
  require_non_negative(options.rfmpb_energy_pj, "power_rfmpb_pj");
  require_non_negative(options.rfmab_energy_pj, "power_rfmab_pj");
  require_non_negative(options.control_energy_pj, "power_control_pj");
  return errors;
}

std::vector<std::string> validate_phy_config(const hbm_sim::MemPhyOptions& phy) {
  std::vector<std::string> errors;
  if (phy.dfi_version.empty()) errors.push_back("dfi_version must not be empty");
  if (phy.command_fifo_depth == 0 || phy.read_fifo_depth == 0 || phy.write_fifo_depth == 0) {
    errors.push_back("PHY FIFO depths must be > 0");
  }
  if (phy.command_pipeline_cycles < 0 || phy.read_return_pipeline_cycles < 0 ||
      phy.write_data_pipeline_cycles < 0 || phy.reset_cycles < 0 ||
      phy.initialization_cycles < 0 || phy.training_cycles < 0) {
    errors.push_back("PHY pipeline/lifecycle cycles must be >= 0");
  }
  return errors;
}

Cli parse_args(int argc, char** argv) {
  Cli cli;

  // 配置先整体读取，再统一确定 standard/preset 并选择 active section。随后才处理
  // 其余 CLI 参数，所以 CLI 的优先级不再依赖它出现在 --config 前还是后。
  std::vector<std::string> config_paths;
  std::string cli_standard;
  std::string cli_preset;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto inspect_value = [&](const std::string& name) -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + name);
      return argv[i + 1];
    };
    if (arg == "--config") config_paths.push_back(inspect_value(arg));
    else if (arg == "--standard") cli_standard = inspect_value(arg);
    else if (arg == "--preset") cli_preset = inspect_value(arg);
  }
  for (const auto& path : config_paths) {
    auto documents = hbm_sim::config::load_document_tree(path);
    cli.config_documents.insert(cli.config_documents.end(),
                                std::make_move_iterator(documents.begin()),
                                std::make_move_iterator(documents.end()));
  }
  const hbm_sim::config::Selection selection =
      hbm_sim::config::discover_selection(cli.config_documents, cli_standard, cli_preset);
  apply_config_documents(cli, cli.config_documents, selection);
  const std::size_t config_spec_override_count = cli.spec_overrides.size();

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    // 本工具只支持 "--key value" 形式。缺值立即报错，避免误用默认值。
    // 例如 "--requests --seed 3" 会被识别为 requests 缺少有效数字，而不是
    // 悄悄把 --seed 当作 requests 的值继续运行。
    auto need_value = [&](const std::string& name) -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument("missing value for " + name);
      }
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      hbm_sim::cli::print_help(argv[0]);
      std::exit(0);
    } else if (arg == "--config") {
      (void)need_value(arg);  // 已在本函数开头解析并应用。
    } else if (arg == "--standard") {
      apply_option(cli, "standard", need_value(arg));
    } else if (arg == "--preset") {
      apply_option(cli, "preset", need_value(arg));
    } else if (arg == "--model-name") {
      apply_option(cli, "model_name", need_value(arg));
    } else if (arg == "--validation-mode") {
      apply_option(cli, "validation_mode", need_value(arg));
    } else if (arg == "--check-config") {
      cli.check_config = true;
    } else if (arg == "--dump-resolved-config") {
      cli.dump_resolved_config_path = need_value(arg);
    } else if (arg == "--stats-json") {
      apply_option(cli, "stats_json", need_value(arg));
    } else if (arg == "--compare-preset") {
      cli.compare_preset = true;
    } else if (arg == "--dump-config-diff") {
      cli.dump_config_diff_path = need_value(arg);
    } else if (arg == "--explain-config") {
      cli.explain_config_key = need_value(arg);
    } else if (arg == "--list-presets") {
      cli.list_presets = true;
    } else if (arg == "--list-schedulers") {
      cli.list_schedulers = true;
    } else if (arg == "--list-row-policies") {
      cli.list_row_policies = true;
    } else if (arg == "--list-backends") {
      cli.list_backends = true;
    } else if (arg == "--list-phy-modes") {
      cli.list_phy_modes = true;
    } else if (arg == "--timing-profile") {
      cli.spec_overrides.emplace_back("timing_profile", need_value(arg));
    } else if (arg == "--vendor-profile") {
      cli.spec_overrides.emplace_back("vendor_profile", need_value(arg));
    } else if (arg == "--speed-bin-mbps") {
      cli.spec_overrides.emplace_back("speed_bin_mbps", need_value(arg));
    } else if (arg == "--density-gb") {
      cli.spec_overrides.emplace_back("density_gb", need_value(arg));
    } else if (arg == "--stack-height") {
      cli.spec_overrides.emplace_back("stack_height", need_value(arg));
    } else if (arg == "--pattern") {
      apply_option(cli, "pattern", need_value(arg));
    } else if (arg == "--trace") {
      apply_option(cli, "trace", need_value(arg));
    } else if (arg == "--requests") {
      apply_option(cli, "requests", need_value(arg));
    } else if (arg == "--read-ratio") {
      apply_option(cli, "read_ratio", need_value(arg));
    } else if (arg == "--seed") {
      apply_option(cli, "seed", need_value(arg));
    } else if (arg == "--random-address-space-bytes") {
      apply_option(cli, "random_address_space_bytes", need_value(arg));
    } else if (arg == "--addr-stride") {
      apply_option(cli, "addr_stride", need_value(arg));
    } else if (arg == "--inject-interval") {
      apply_option(cli, "inject_interval", need_value(arg));
    } else if (arg == "--init-sequence") {
      apply_option(cli, "init_sequence", need_value(arg));
    } else if (arg == "--init-sequence-interval") {
      apply_option(cli, "init_sequence_interval", need_value(arg));
    } else if (arg == "--read-buffer-size") {
      apply_option(cli, "read_buffer_size", need_value(arg));
    } else if (arg == "--write-buffer-size") {
      apply_option(cli, "write_buffer_size", need_value(arg));
    } else if (arg == "--priority-buffer-size") {
      apply_option(cli, "priority_buffer_size", need_value(arg));
    } else if (arg == "--mem-phy" || arg == "--phy-mode") {
      apply_option(cli, "mem_phy_mode", need_value(arg));
    } else if (arg == "--dfi-version") {
      apply_option(cli, "dfi_version", need_value(arg));
    } else if (arg == "--max-cycles") {
      apply_option(cli, "max_cycles", need_value(arg));
    } else if (arg == "--stats-view") {
      apply_option(cli, "stats_view", need_value(arg));
    } else if (arg == "--progress-interval") {
      apply_option(cli, "progress_interval", need_value(arg));
    } else if (arg == "--single-controller") {
      apply_option(cli, "single_controller", "true");
    } else if (arg == "--scheduler") {
      apply_option(cli, "scheduler", need_value(arg));
    } else if (arg == "--row-policy") {
      apply_option(cli, "row_policy", need_value(arg));
    } else if (arg == "--row-policy-cap") {
      apply_option(cli, "row_policy_cap", need_value(arg));
    } else if (arg == "--addr-mapping" || arg == "--address-mapping") {
      // address mapping 属于 DramSpec，而不是 ControllerOptions；它影响 row hit、
      // bank/channel 分布，是与 Ramulator2.1 对齐实验时必须显式记录的配置。
      cli.spec_overrides.emplace_back("address_mapping", need_value(arg));
    } else if (arg == "--dram-transaction-bytes" || arg == "--transaction-size") {
      cli.spec_overrides.emplace_back("dram_transaction_bytes", need_value(arg));
    } else if (arg == "--channel-mapper") {
      apply_option(cli, "channel_mapper", need_value(arg));
    } else if (arg == "--stack-count") {
      apply_option(cli, "stack_count", need_value(arg));
    } else if (arg == "--stack-mapping") {
      apply_option(cli, "stack_mapping", need_value(arg));
    } else if (arg == "--stack-interleave-bytes") {
      apply_option(cli, "stack_interleave_bytes", need_value(arg));
    } else if (arg == "--stack-ingress-buffer-size") {
      apply_option(cli, "stack_ingress_buffer_size", need_value(arg));
    } else if (arg == "--stack-dispatch-width") {
      apply_option(cli, "stack_dispatch_width", need_value(arg));
    } else if (arg == "--stack-qos-policy") {
      apply_option(cli, "stack_qos_policy", need_value(arg));
    } else if (arg == "--lpddr-efficiency") {
      cli.spec_overrides.emplace_back("lpddr_efficiency_mode", need_value(arg));
    } else if (arg == "--lpddr-dvfs") {
      cli.spec_overrides.emplace_back("lpddr_dvfs_mode", need_value(arg));
    } else if (arg == "--lpddr-wck") {
      cli.spec_overrides.emplace_back("lpddr_wck_mode", need_value(arg));
    } else if (arg == "--dfi-phase-count") {
      cli.spec_overrides.emplace_back("dfi_phase_count", need_value(arg));
    } else if (arg == "--dfi-data-lane-bytes") {
      cli.spec_overrides.emplace_back("dfi_data_lane_bytes", need_value(arg));
    } else if (arg == "--dfi-read-latency-nck") {
      cli.spec_overrides.emplace_back("dfi_read_latency_nck", need_value(arg));
    } else if (arg == "--dfi-write-latency-nck") {
      cli.spec_overrides.emplace_back("dfi_write_latency_nck", need_value(arg));
    } else if (arg == "--metadata-bits-per-request") {
      cli.spec_overrides.emplace_back("metadata_bits_per_request", need_value(arg));
    } else if (arg == "--ecc-bits-per-request") {
      cli.spec_overrides.emplace_back("ecc_bits_per_request", need_value(arg));
    } else if (arg == "--rfm-act-threshold") {
      cli.spec_overrides.emplace_back("rfm_act_threshold", need_value(arg));
    } else if (arg == "--rfm-decrement") {
      cli.spec_overrides.emplace_back("rfm_decrement", need_value(arg));
    } else if (arg == "--refresh-policy") {
      cli.spec_overrides.emplace_back("refresh_policy", need_value(arg));
    } else if (arg == "--refresh-temperature") {
      cli.spec_overrides.emplace_back("refresh_temperature_mode", need_value(arg));
    } else if (arg == "--refresh-postpone-limit") {
      cli.spec_overrides.emplace_back("refresh_postpone_limit", need_value(arg));
    } else if (arg == "--refresh-pullin-limit") {
      cli.spec_overrides.emplace_back("refresh_pullin_limit", need_value(arg));
    } else if (arg == "--refresh-credit-limit") {
      cli.spec_overrides.emplace_back("refresh_credit_limit", need_value(arg));
    } else if (arg == "--rfm-policy") {
      cli.spec_overrides.emplace_back("rfm_policy", need_value(arg));
    } else if (arg == "--hbm-link-crc") {
      cli.spec_overrides.emplace_back("hbm_link_crc_mode", need_value(arg));
    } else if (arg == "--hbm-link-crc-bits") {
      cli.spec_overrides.emplace_back("hbm_link_crc_bits_per_request", need_value(arg));
    } else if (arg == "--hbm-ras-metadata-bits") {
      cli.spec_overrides.emplace_back("hbm_ras_metadata_bits_per_request", need_value(arg));
    } else if (arg == "--hbm-ecc-bits") {
      cli.spec_overrides.emplace_back("hbm_ecc_bits_per_request", need_value(arg));
    } else if (arg == "--hbm-link-retry") {
      cli.spec_overrides.emplace_back("hbm_link_retry_enabled", need_value(arg));
    } else if (arg == "--lpddr-link-protection") {
      cli.spec_overrides.emplace_back("lpddr_link_protection", need_value(arg));
    } else if (arg == "--lpddr-dbi") {
      cli.spec_overrides.emplace_back("lpddr_dbi_enabled", need_value(arg));
    } else if (arg == "--lpddr-dbi-bits") {
      cli.spec_overrides.emplace_back("lpddr_dbi_bits_per_request", need_value(arg));
    } else if (arg == "--lpddr-link-ecc") {
      cli.spec_overrides.emplace_back("lpddr_link_ecc_enabled", need_value(arg));
    } else if (arg == "--lpddr-link-ecc-bits") {
      cli.spec_overrides.emplace_back("lpddr_link_ecc_bits_per_request", need_value(arg));
    } else if (arg == "--lpddr-ca-parity") {
      cli.spec_overrides.emplace_back("lpddr_ca_parity_enabled", need_value(arg));
    } else if (arg == "--lpddr-ca-parity-bits") {
      cli.spec_overrides.emplace_back("lpddr_ca_parity_bits_per_command", need_value(arg));
    } else if (arg == "--low-power") {
      cli.spec_overrides.emplace_back("low_power_mode", need_value(arg));
    } else if (arg == "--low-power-entry-cycles") {
      cli.spec_overrides.emplace_back("low_power_entry_cycles", need_value(arg));
    } else if (arg == "--low-power-exit-cycles") {
      cli.spec_overrides.emplace_back("low_power_exit_cycles", need_value(arg));
    } else if (arg == "--self-refresh-exit-cycles") {
      cli.spec_overrides.emplace_back("self_refresh_exit_cycles", need_value(arg));
    } else if (arg == "--strict-timing-table") {
      apply_option(cli, "strict_timing_table", "true");
    } else if (arg == "--cmd-trace") {
      apply_option(cli, "cmd_trace", need_value(arg));
    } else if (arg == "--response-trace" || arg == "--host-response-trace") {
      apply_option(cli, "response_trace", need_value(arg));
    } else if (arg == "--transaction-response-trace") {
      apply_option(cli, "transaction_response_trace", need_value(arg));
    } else if (arg == "--response-delivery-mode") {
      apply_option(cli, "response_delivery_mode", need_value(arg));
    } else if (arg == "--host-response-queue-capacity") {
      apply_option(cli, "host_response_queue_capacity", need_value(arg));
    } else if (arg == "--transaction-response-queue-capacity") {
      apply_option(cli, "transaction_response_queue_capacity", need_value(arg));
    } else if (arg == "--dfi-trace" || arg == "--dump-dfi-trace") {
      apply_option(cli, "dfi_trace", need_value(arg));
    } else if (arg == "--dfi-signal-trace" || arg == "--dump-dfi-signal-trace") {
      apply_option(cli, "dfi_signal_trace", need_value(arg));
    } else if (arg == "--dump-timing-table") {
      apply_option(cli, "dump_timing_table", need_value(arg));
    } else if (arg == "--validate-cmd-trace") {
      apply_option(cli, "validate_cmd_trace", "true");
    } else if (arg == "--validate-dfi-trace") {
      apply_option(cli, "validate_dfi_trace", "true");
    } else if (arg == "--fail-on-data-mismatch") {
      apply_option(cli, "fail_on_data_mismatch", "true");
    } else if (arg == "--allow-data-mismatch") {
      apply_option(cli, "fail_on_data_mismatch", "false");
    } else if (arg == "--memory-image") {
      apply_option(cli, "memory_image", need_value(arg));
    } else if (arg == "--dump-memory-image") {
      apply_option(cli, "dump_memory_image", need_value(arg));
    } else if (arg == "--dump-memory-csv") {
      apply_option(cli, "dump_memory_csv", need_value(arg));
    } else if (arg == "--mismatch-report") {
      apply_option(cli, "mismatch_report", need_value(arg));
    } else if (arg == "--verify-golden") {
      apply_option(cli, "verify_golden", need_value(arg));
    } else if (arg == "--dump-thermal-map") {
      apply_option(cli, "dump_thermal_map", need_value(arg));
    } else if (arg == "--memory-backend") {
      apply_option(cli, "memory_backend", need_value(arg));
    } else if (arg == "--memory-capacity-bytes") {
      apply_option(cli, "memory_capacity_bytes", need_value(arg));
    } else if (arg == "--memory-data-file") {
      apply_option(cli, "memory_data_file", need_value(arg));
    } else if (arg == "--memory-init-file") {
      apply_option(cli, "memory_init_file", need_value(arg));
    } else if (arg == "--memory-meta-file") {
      apply_option(cli, "memory_meta_file", need_value(arg));
    } else if (arg == "--memory-presence-file") {
      apply_option(cli, "memory_presence_file", need_value(arg));
    } else if (arg == "--memory-chunk-size") {
      apply_option(cli, "memory_chunk_size", need_value(arg));
    } else if (arg == "--memory-chunk-cache-entries") {
      apply_option(cli, "memory_chunk_cache_entries", need_value(arg));
    } else if (arg == "--sparse-density-warning-pct") {
      apply_option(cli, "sparse_density_warning_pct", need_value(arg));
    } else if (arg == "--topology-stats-scan-limit") {
      apply_option(cli, "topology_stats_scan_limit", need_value(arg));
    } else if (arg == "--floorplan") {
      apply_option(cli, "floorplan", need_value(arg));
    } else if (arg == "--power-model") {
      apply_option(cli, "power_model", need_value(arg));
    } else if (arg == "--thermal-model") {
      apply_option(cli, "thermal_model", need_value(arg));
    } else if (arg == "--power-source" || arg == "--power-calibration") {
      apply_option(cli, "power_source", need_value(arg));
    } else if (arg == "--power-scale") {
      apply_option(cli, "power_scale", need_value(arg));
    } else if (arg == "--thermal-ambient-c") {
      apply_option(cli, "thermal_ambient_c", need_value(arg));
    } else if (arg == "--thermal-cooling-per-cycle") {
      apply_option(cli, "thermal_cooling_per_cycle", need_value(arg));
    } else if (arg == "--thermal-rise-c-per-pj") {
      apply_option(cli, "thermal_rise_c_per_pj", need_value(arg));
    } else if (arg == "--thermal-grid-cols-per-tile") {
      apply_option(cli, "thermal_grid_cols_per_tile", need_value(arg));
    } else if (arg == "--thermal-grid-rows-per-tile") {
      apply_option(cli, "thermal_grid_rows_per_tile", need_value(arg));
    } else if (arg == "--thermal-coupling") {
      apply_option(cli, "thermal_coupling", need_value(arg));
    } else if (arg == "--thermal-lateral-coupling") {
      apply_option(cli, "thermal_lateral_coupling", need_value(arg));
    } else if (arg == "--thermal-vertical-coupling") {
      apply_option(cli, "thermal_vertical_coupling", need_value(arg));
    } else if (arg == "--thermal-tsv-coupling-scale") {
      apply_option(cli, "thermal_tsv_coupling_scale", need_value(arg));
    } else if (arg == "--thermal-tsvs-per-grid") {
      apply_option(cli, "thermal_tsvs_per_grid", need_value(arg));
    } else if (arg == "--subarrays-per-bank") {
      apply_option(cli, "subarrays_per_bank", need_value(arg));
    } else if (arg == "--mats-per-subarray-x") {
      apply_option(cli, "mats_per_subarray_x", need_value(arg));
    } else if (arg == "--mats-per-subarray-y") {
      apply_option(cli, "mats_per_subarray_y", need_value(arg));
    } else if (arg == "--cells-per-mat-x") {
      apply_option(cli, "cells_per_mat_x", need_value(arg));
    } else if (arg == "--cells-per-mat-y") {
      apply_option(cli, "cells_per_mat_y", need_value(arg));
    } else if (arg == "--microbumps-x") {
      apply_option(cli, "microbumps_x", need_value(arg));
    } else if (arg == "--microbumps-y") {
      apply_option(cli, "microbumps_y", need_value(arg));
    } else if (arg == "--ecc-shadow") {
      apply_option(cli, "ecc_shadow", need_value(arg));
    } else if (arg == "--ecc-check-on-read") {
      apply_option(cli, "ecc_check_on_read", need_value(arg));
    } else if (arg == "--ecc-correct-single-bit") {
      apply_option(cli, "ecc_correct_single_bit", need_value(arg));
    } else if (arg == "--ecc-inject-period") {
      apply_option(cli, "ecc_inject_period", need_value(arg));
    } else if (arg == "--power-vdd") {
      apply_option(cli, "power_vdd", need_value(arg));
    } else if (arg == "--idd0") {
      apply_option(cli, "idd0", need_value(arg));
    } else if (arg == "--idd2n") {
      apply_option(cli, "idd2n", need_value(arg));
    } else if (arg == "--idd3n") {
      apply_option(cli, "idd3n", need_value(arg));
    } else if (arg == "--idd4r") {
      apply_option(cli, "idd4r", need_value(arg));
    } else if (arg == "--idd4w") {
      apply_option(cli, "idd4w", need_value(arg));
    } else if (arg == "--idd5ab") {
      apply_option(cli, "idd5ab", need_value(arg));
    } else if (arg == "--idd5pb") {
      apply_option(cli, "idd5pb", need_value(arg));
    } else if (arg == "--idd6x") {
      apply_option(cli, "idd6x", need_value(arg));
    } else if (arg == "--idd-devices-per-rank") {
      apply_option(cli, "idd_devices_per_rank", need_value(arg));
    } else if (arg == "--idd-burst-cycles") {
      apply_option(cli, "idd_burst_cycles", need_value(arg));
    } else {
      throw std::invalid_argument("unknown option: " + arg);
    }
  }
  // spec_overrides 中新增的尾部元素来自 CLI。把它们也放入 provenance 链，
  // 这样 --compare-preset 和 standard/device 模式检查的是最终执行模型，而不是
  // 只检查配置文件；命令行位于固定 layer 60，高于 [override] 的 layer 50。
  hbm_sim::config::ModelOverrides cli_timing_sources;
  for (std::size_t index = config_spec_override_count;
       index < cli.spec_overrides.size(); ++index) {
    const auto& [key, value] = cli.spec_overrides[index];
    cli.resolved_config_entries.push_back(
        {key, value, "command-line", "<command-line>", 0, 60});
    if (hbm_sim::config::is_timing_override_key(key))
      cli_timing_sources.emplace_back("timing_source." + key, "research_default");
  }
  cli.spec_overrides.insert(cli.spec_overrides.end(), cli_timing_sources.begin(), cli_timing_sources.end());

  // 非 DramSpec 的 CLI 模型项（例如 scheduler、PHY、backend、stack_count）
  // 也进入审计链。这里不重新赋值，只记录已经由上方严格解析过的参数。
  const std::set<std::string> no_value_options{
      "--help", "-h", "--check-config", "--compare-preset", "--list-presets",
      "--list-schedulers", "--list-row-policies", "--list-backends",
      "--list-phy-modes", "--single-controller", "--strict-timing-table",
      "--validate-cmd-trace", "--validate-dfi-trace", "--fail-on-data-mismatch",
      "--allow-data-mismatch"};
  const std::map<std::string, std::string> cli_audit_aliases{
      {"phy_mode", "mem_phy_mode"},
      {"mem_phy", "mem_phy_mode"},
      {"addr_mapping", "address_mapping"},
      {"transaction_size", "dram_transaction_bytes"},
      {"lpddr_efficiency", "lpddr_efficiency_mode"},
      {"lpddr_dvfs", "lpddr_dvfs_mode"},
      {"lpddr_wck", "lpddr_wck_mode"},
      {"refresh_temperature", "refresh_temperature_mode"},
      {"hbm_link_crc", "hbm_link_crc_mode"},
      {"hbm_link_crc_bits", "hbm_link_crc_bits_per_request"},
      {"hbm_ras_metadata_bits", "hbm_ras_metadata_bits_per_request"},
      {"hbm_ecc_bits", "hbm_ecc_bits_per_request"},
      {"hbm_link_retry", "hbm_link_retry_enabled"},
      {"lpddr_dbi", "lpddr_dbi_enabled"},
      {"lpddr_dbi_bits", "lpddr_dbi_bits_per_request"},
      {"lpddr_link_ecc", "lpddr_link_ecc_enabled"},
      {"lpddr_link_ecc_bits", "lpddr_link_ecc_bits_per_request"},
      {"lpddr_ca_parity", "lpddr_ca_parity_enabled"},
      {"lpddr_ca_parity_bits", "lpddr_ca_parity_bits_per_command"},
      {"low_power", "low_power_mode"},
      {"power_calibration", "power_source"},
      {"idd_devices_per_rank", "idd_devices_per_rank"}};
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (!arg.starts_with("-") || no_value_options.contains(arg)) continue;
    if (index + 1 >= argc) break;  // 缺值已经在上方抛错；这里只是防御。
    std::string key = normalize_key(arg.substr(2));
    if (const auto alias = cli_audit_aliases.find(key); alias != cli_audit_aliases.end()) {
      key = alias->second;
    }
    const std::string value = argv[++index];
    if (key == "config" || key == "standard" || key == "preset" ||
        key == "model_name" || key == "validation_mode" ||
        is_spec_override_key(key)) {
      continue;
    }
    cli.resolved_config_entries.push_back(
        {key, value, "command-line", "<command-line>",
         static_cast<std::size_t>(index), 60});
  }
  if (cli.single_controller) {
    cli.resolved_config_entries.push_back(
        {"single_controller", "true", "command-line", "<command-line>", 0, 60});
  }
  return cli;
}

struct ConfigDifference {
  std::string key;
  std::string baseline;
  std::string value;
  std::string origin;
};

bool is_auditable_model_key(const std::string& key) {
  static const std::set<std::string> non_model_keys{
      "schema_version", "standard", "model_name", "preset", "parameter_reference",
      "validation_mode", "pattern", "trace", "trace_path", "requests", "read_ratio",
      "seed", "random_address_space_bytes", "addr_stride", "inject_interval", "init_sequence",
      "init_sequence_interval", "max_cycles", "progress_interval", "stats_view", "stats_json",
      "strict_timing_table",
      "cmd_trace", "cmd_trace_path", "host_response_trace", "response_trace",
      "response_trace_path", "transaction_response_trace",
      "transaction_response_trace_path", "response_delivery_mode",
      "host_response_queue_capacity", "transaction_response_queue_capacity",
      "dump_dfi_trace", "dfi_trace", "dfi_trace_path",
      "dump_dfi_signal_trace", "dfi_signal_trace", "dfi_signal_trace_path",
      "dump_timing_table", "timing_table_path", "validate_cmd_trace",
      "validate_command_trace", "validate_dfi_trace", "fail_on_data_mismatch",
      "memory_image", "memory_image_path", "dump_memory_image",
      "dump_memory_image_path", "dump_memory_csv", "dump_memory_csv_path",
      "mismatch_report", "mismatch_report_path", "verify_golden",
      "verify_golden_path", "dump_thermal_map", "thermal_map", "thermal_map_path",
      "dump_resolved_config", "dump_config_diff", "explain_config"};
  return !non_model_keys.contains(key) && key != "timing_override_source" &&
         key != "timing_source";
}

std::vector<ConfigDifference> config_differences(const Cli& cli) {
  std::map<std::string, hbm_sim::config::ConfigEntry> baseline;
  std::map<std::string, hbm_sim::config::ConfigEntry> resolved;
  for (const auto& entry : cli.resolved_config_entries) {
    const std::string key = normalize_key(entry.key);
    resolved[key] = entry;
    if (entry.layer < 50) baseline[key] = entry;
  }

  std::vector<ConfigDifference> differences;
  for (const auto& [key, final_entry] : resolved) {
    if (!is_auditable_model_key(key)) continue;
    auto original = baseline.find(key);
    if (original == baseline.end()) {
      if (final_entry.layer >= 50) {
        differences.push_back({key, "<preset-unset>", final_entry.value,
                               final_entry.path + ":" + std::to_string(final_entry.line)});
      }
      continue;
    }
    if (original->second.value != final_entry.value) {
      differences.push_back({key, original->second.value, final_entry.value,
                             final_entry.path + ":" + std::to_string(final_entry.line)});
    }
  }
  return differences;
}

void write_config_diff(std::ostream& out, const Cli& cli) {
  const auto differences = config_differences(cli);
  out << "model_name=" << cli.model_name << '\n'
      << "base_standard=" << hbm_sim::config::canonical_standard(cli.standard) << '\n'
      << "preset=" << (cli.preset.empty() ? "<built-in>" : cli.preset) << '\n'
      << "validation_mode=" << hbm_sim::config::to_string(cli.validation_mode) << '\n'
      << "modified_parameters=" << differences.size() << "\n\n";
  for (const auto& difference : differences) {
    out << difference.key << ":\n"
        << "  preset = " << difference.baseline << '\n'
        << "  model  = " << difference.value << '\n'
        << "  origin = " << difference.origin << '\n';
  }
}

void explain_config(const Cli& cli, const std::string& requested_key) {
  const std::string key = normalize_key(requested_key);
  bool found = false;
  std::cout << "Configuration history for " << key << ":\n";
  for (const auto& entry : cli.resolved_config_entries) {
    if (normalize_key(entry.key) != key) continue;
    found = true;
    std::cout << "  layer=" << entry.layer << " value=" << entry.value
              << " source=" << entry.path << ':' << entry.line;
    if (!entry.section.empty()) std::cout << " [" << entry.section << ']';
    std::cout << '\n';
  }
  if (!found) std::cout << "  no active config-file assignment; using built-in/CLI value\n";
  for (const auto& derived : cli.derived_parameters) {
    if (derived.key == key)
      std::cout << "  derived value=" << derived.value << " formula=" << derived.formula << '\n';
  }
}

bool handle_list_commands(const Cli& cli) {
  bool handled = false;
  if (cli.list_schedulers) {
    std::cout << "fcfs\n  First Come First Served; parameters: none\n"
                 "frfcfs\n  First Ready, First Come First Served; parameters: none\n";
    handled = true;
  }
  if (cli.list_row_policies) {
    std::cout << "open_page\nclosed_page\nclosed_cap\n  parameter: row_policy_cap (> 0)\n";
    handled = true;
  }
  if (cli.list_backends) {
    std::cout << "sparse\nmmap_sparse\nchunk_file\n";
    handled = true;
  }
  if (cli.list_phy_modes) {
    std::cout << "direct\nbehavioral\n";
    handled = true;
  }
  if (cli.list_presets) {
    std::set<std::string> presets;
    for (const auto& document : cli.config_documents) {
      for (const auto& preset : hbm_sim::config::list_presets(document, cli.standard)) {
        presets.insert(preset);
      }
    }
    if (presets.empty()) {
      std::cout << "No named presets found for " << cli.standard
                << "; pass --config configs/hbm.cfg or configs/lpddr.cfg.\n";
    } else {
      for (const auto& preset : presets) std::cout << preset << '\n';
    }
    handled = true;
  }
  return handled;
}

std::vector<std::string> validate_runtime_config(const Cli& cli,
                                                 const hbm_sim::DramSpec& spec) {
  std::vector<std::string> errors;
  if (cli.stack_count <= 0) errors.push_back("stack_count must be > 0");
  if (cli.stack_interleave_bytes == 0) errors.push_back("stack_interleave_bytes must be > 0");
  if (cli.stack_ingress_buffer_size == 0) errors.push_back("stack_ingress_buffer_size must be > 0");
  if (cli.stack_dispatch_width == 0) errors.push_back("stack_dispatch_width must be > 0");
  if (cli.read_ratio < 0 || cli.read_ratio > 100) errors.push_back("read_ratio must be in [0, 100]");
  if (cli.controller.read_buffer_size == 0 || cli.controller.write_buffer_size == 0 ||
      cli.controller.priority_buffer_size == 0) {
    errors.push_back("controller buffer sizes must be > 0");
  }
  if (cli.controller.write_low_watermark < 0.0 || cli.controller.write_high_watermark > 1.0 ||
      cli.controller.write_low_watermark >= cli.controller.write_high_watermark) {
    errors.push_back("write watermarks must satisfy 0 <= low < high <= 1");
  }
  if (cli.controller.row_policy == hbm_sim::RowPolicyKind::ClosedCap &&
      cli.controller.row_policy_cap <= 0) {
    errors.push_back("row_policy_cap must be > 0 for closed_cap");
  }
  if (spec.org.channels <= 0 || spec.org.pseudo_channels <= 0 || spec.org.sids <= 0 ||
      spec.org.ranks <= 0 || spec.org.bank_groups <= 0 || spec.org.banks_per_group <= 0 ||
      spec.org.rows <= 0 || spec.org.columns <= 0) {
    errors.push_back("all organization dimensions must be > 0");
  }
  if (spec.data_rate_mbps <= 0 || spec.data_bus_bits <= 0 || spec.internal_prefetch_size <= 0 ||
      spec.timing.tCK_ps <= 0.0) {
    errors.push_back("data rate, bus width, prefetch and tCK must be > 0");
  }
  const bool has_sectioned_config = std::any_of(
      cli.config_documents.begin(), cli.config_documents.end(),
      [](const auto& document) { return document.sectioned; });
  if (has_sectioned_config && cli.config_schema_version != 2 && cli.config_schema_version != 3) {
    errors.push_back("sectioned config documents require schema_version = 2 or 3");
  }
  const std::string protocol = lower_value(cli.phy_protocol);
  if (protocol != "auto" && protocol != "hbm" && protocol != "lpddr") {
    errors.push_back("phy protocol must be auto, hbm or lpddr");
  } else if ((protocol == "hbm" && spec.lpddr_family) ||
             (protocol == "lpddr" && !spec.lpddr_family)) {
    errors.push_back("selected PHY protocol has no executable semantics for base standard " + spec.name);
  }

  const std::string init_sequence = lower_value(cli.init_sequence);
  const bool init_disabled = init_sequence.empty() || init_sequence == "none" ||
                             init_sequence == "off";
  const bool init_auto = init_sequence == "auto";
  const bool hbm_init = init_sequence == "hbm" || init_sequence == "hbm3" ||
                        init_sequence == "hbm4";
  const bool lpddr_init = init_sequence == "lpddr" || init_sequence == "lpddr5" ||
                          init_sequence == "lpddr6" ||
                          init_sequence == "lpddr6_full";
  if (!init_disabled && !init_auto && !hbm_init && !lpddr_init) {
    errors.push_back("unsupported init_sequence: " + cli.init_sequence);
  } else if ((hbm_init && spec.lpddr_family) ||
             (lpddr_init && !spec.lpddr_family)) {
    errors.push_back("init_sequence '" + cli.init_sequence +
                     "' is incompatible with base standard " + spec.name);
  }

  bool selected_preset_exists = cli.preset.empty();
  for (const auto& document : cli.config_documents) {
    const auto presets = hbm_sim::config::list_presets(document, cli.standard);
    selected_preset_exists = selected_preset_exists ||
        std::find(presets.begin(), presets.end(), normalize_key(cli.preset)) != presets.end();
  }
  if (!cli.preset.empty() && !selected_preset_exists) {
    errors.push_back("preset '" + cli.preset + "' does not exist for standard " + cli.standard);
  }

  // 分节配置可以把另一协议族的键放在 inactive standard section 中；这种键不会
  // 进入 resolved entries。若它出现在 common/override 中则会成为静默 no-op，
  // 因而必须在运行前拒绝，而不能让用户误以为该行为已经得到建模。
  for (const auto& entry : cli.resolved_config_entries) {
    const std::string key = normalize_key(entry.key);
    const bool wrong_family =
        (spec.lpddr_family && key.starts_with("hbm_")) ||
        (!spec.lpddr_family && key.starts_with("lpddr_"));
    if (wrong_family) {
      errors.push_back(entry.path + ":" + std::to_string(entry.line) +
                       ": option '" + key + "' has no executable semantics for " +
                       spec.name);
    }
  }

  const auto differences = config_differences(cli);
  const auto conformance_difference_count = static_cast<std::size_t>(std::count_if(
      differences.begin(), differences.end(), [](const ConfigDifference& difference) {
        return is_spec_override_key(difference.key);
      }));
  if (cli.validation_mode != hbm_sim::config::ValidationMode::Exploratory &&
      conformance_difference_count > 0) {
    errors.push_back("strict conformance mode rejects " +
                     std::to_string(conformance_difference_count) +
                     " override(s) that differ from the selected preset; use exploratory mode to run them");
  }
  if (cli.validation_mode == hbm_sim::config::ValidationMode::Standard &&
      (spec.timing_table.source_count(hbm_sim::TimingValueSource::ResearchDefault) > 0 ||
       spec.timing_table.source_count(hbm_sim::TimingValueSource::ExternalReference) > 0)) {
    errors.push_back("standard mode requires all model-required timing to be JEDEC, vendor or derived");
  }
  if (cli.validation_mode == hbm_sim::config::ValidationMode::Device &&
      spec.timing_table.provisional_count() > 0) {
    errors.push_back("device mode requires all provisional/vendor-required timing to be calibrated");
  }
  if (cli.validation_mode == hbm_sim::config::ValidationMode::Device &&
      (lower_value(spec.vendor_profile).empty() ||
       lower_value(spec.vendor_profile) == "generic" ||
       spec.timing_table.source_count(hbm_sim::TimingValueSource::Vendor) == 0)) {
    errors.push_back(
        "device mode requires a named vendor_profile and vendor-sourced timing values");
  }
  return errors;
}

void write_resolved_config(const std::string&, const Cli&, const hbm_sim::DramSpec&);
void serialize_resolved_config(std::ostream& out, const Cli& cli,
                               const hbm_sim::DramSpec& spec) {
  out << std::boolalpha << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "# Fully resolved hbm_sim configuration. Values below are the executed model,\n"
         "# including built-in defaults, active sections, overrides and CLI options.\n\n";
  for (const auto& derived : cli.derived_parameters)
    out << "# derived " << derived.key << " = " << derived.value
        << " : " << derived.formula << '\n';
  out << "[meta]\n"
      << "schema_version = 3\n\n";
  out << "[model]\n"
      << "name = " << cli.model_name << '\n'
      << "base_standard = " << hbm_sim::config::canonical_standard(cli.standard) << '\n';
  if (!cli.preset.empty()) {
    out << "preset = " << cli.preset << '\n';
  }
  out << "timing_profile = " << hbm_sim::config::model_field_text(spec, "timing_profile") << '\n'
      << "vendor_profile = " << hbm_sim::config::model_field_text(spec, "vendor_profile") << '\n'
      << "mode_profile = " << hbm_sim::config::model_field_text(spec, "mode_profile") << '\n';
  if (!cli.parameter_reference.empty()) {
    out << "parameter_reference = " << cli.parameter_reference << '\n';
  }
  out << "\n[validation]\n"
      << "mode = " << hbm_sim::config::to_string(cli.validation_mode) << '\n'
      << "strict_timing_table = " << cli.strict_timing_table << "\n\n";
  out << "[system]\n"
      << "stack_count = " << cli.stack_count << '\n'
      << "stack_mapping = " << stack_mapping_name(cli.stack_mapping) << '\n'
      << "stack_interleave_bytes = " << cli.stack_interleave_bytes << '\n'
      << "stack_ingress_buffer_size = " << cli.stack_ingress_buffer_size << '\n'
      << "stack_dispatch_width = " << cli.stack_dispatch_width << '\n'
      << "stack_qos_policy = " << stack_qos_policy_name(cli.stack_qos_policy) << '\n'
      << "response_delivery_mode = "
      << response_delivery_mode_name(cli.response_delivery_mode) << '\n'
      << "host_response_queue_capacity = "
      << cli.host_response_queue_capacity << '\n'
      << "transaction_response_queue_capacity = "
      << cli.transaction_response_queue_capacity << '\n'
      << "single_controller = " << cli.single_controller << "\n\n";
  out << "[workload]\n"
      << "pattern = " << cli.pattern << '\n'
      << "requests = " << cli.requests << '\n'
      << "read_ratio = " << cli.read_ratio << '\n'
      << "seed = " << cli.seed << '\n'
      << "random_address_space_bytes = " << cli.random_address_space_bytes << '\n'
      << "addr_stride = " << cli.addr_stride << '\n'
      << "inject_interval = " << cli.inject_interval << '\n'
      << "init_sequence = " << cli.init_sequence << '\n'
      << "init_sequence_interval = " << cli.init_sequence_interval << '\n'
      << "max_cycles = " << cli.max_cycles << '\n'
      << "progress_interval = " << cli.progress_interval << '\n'
      << "stats_view = " << cli.stats_view << "\n\n";
  if (!cli.trace_path.empty()) out << "trace = " << cli.trace_path << "\n\n";
  out << "[controller]\n"
      << "scheduler = " << hbm_sim::to_string(cli.controller.scheduler) << '\n'
      << "row_policy = " << hbm_sim::to_string(cli.controller.row_policy) << '\n'
      << "row_policy_cap = " << cli.controller.row_policy_cap << '\n'
      << "read_buffer_size = " << cli.controller.read_buffer_size << '\n'
      << "write_buffer_size = " << cli.controller.write_buffer_size << '\n'
      << "priority_buffer_size = " << cli.controller.priority_buffer_size << '\n'
      << "write_low_watermark = " << cli.controller.write_low_watermark << '\n'
      << "write_high_watermark = " << cli.controller.write_high_watermark << "\n\n";
  out << "[mapping]\n"
      << "address_mapping = " << hbm_sim::to_string(spec.address_mapping) << '\n'
      << "channel_mapper = " << hbm_sim::to_string(cli.channel_mapper) << "\n\n";
  out << "[phy]\n"
      << "mode = " << hbm_sim::to_string(cli.controller.phy.mode) << '\n'
      << "protocol = " << (spec.lpddr_family ? "lpddr" : "hbm") << '\n'
      << "dfi_version = " << cli.controller.phy.dfi_version << '\n'
      << "command_fifo_depth = " << cli.controller.phy.command_fifo_depth << '\n'
      << "read_fifo_depth = " << cli.controller.phy.read_fifo_depth << '\n'
      << "write_fifo_depth = " << cli.controller.phy.write_fifo_depth << '\n'
      << "command_pipeline_cycles = " << cli.controller.phy.command_pipeline_cycles << '\n'
      << "read_return_pipeline_cycles = " << cli.controller.phy.read_return_pipeline_cycles << '\n'
      << "write_data_pipeline_cycles = " << cli.controller.phy.write_data_pipeline_cycles << '\n'
      << "reset_cycles = " << cli.controller.phy.reset_cycles << '\n'
      << "initialization_cycles = " << cli.controller.phy.initialization_cycles << '\n'
      << "training_cycles = " << cli.controller.phy.training_cycles << '\n'
      << "auto_train = " << cli.controller.phy.auto_train << "\n\n";
  out << "[architecture]\n";
  for (const auto key : {"speed_bin_mbps", "data_rate_mbps", "density_gb", "stack_height", "channels", "pseudo_channels", "sids", "ranks", "bank_groups", "banks_per_group", "rows", "columns", "line_size", "dram_transaction_bytes", "data_bus_bits", "prefetch_size", "dfi_phase_count", "dfi_data_lane_bytes", "dfi_read_latency_nck", "dfi_write_latency_nck", "tick_multiplier", "full_stack_model", "tck_ps"})
    hbm_sim::config::write_model_field(out, spec, key);
  out << '\n';

  out << "[maintenance]\n"
      << "supports_refresh = " << hbm_sim::config::model_field_text(spec, "supports_refresh") << '\n'
      << "refresh_policy = " << hbm_sim::to_string(spec.refresh_policy) << '\n'
      << "supports_rfm = " << hbm_sim::config::model_field_text(spec, "supports_rfm") << '\n'
      << "rfm_policy = " << hbm_sim::to_string(spec.rfm_policy) << '\n'
      << "rfm_act_threshold = " << hbm_sim::config::model_field_text(spec, "rfm_act_threshold") << '\n'
      << "rfm_decrement = " << hbm_sim::config::model_field_text(spec, "rfm_decrement") << '\n'
      << "refresh_postpone_limit = " << hbm_sim::config::model_field_text(spec, "refresh_postpone_limit") << '\n'
      << "refresh_pullin_limit = " << hbm_sim::config::model_field_text(spec, "refresh_pullin_limit") << '\n'
      << "refresh_credit_limit = " << hbm_sim::config::model_field_text(spec, "refresh_credit_limit") << '\n'
      << "refresh_temperature_mode = " << hbm_sim::to_string(spec.refresh_temperature_mode) << '\n'
      << "refresh_high_temp_multiplier = " << hbm_sim::config::model_field_text(spec, "refresh_high_temp_multiplier") << '\n'
      << "low_power_mode = " << hbm_sim::to_string(spec.low_power_mode) << '\n'
      << "low_power_entry_cycles = " << hbm_sim::config::model_field_text(spec, "low_power_entry_cycles") << '\n'
      << "low_power_exit_cycles = " << hbm_sim::config::model_field_text(spec, "low_power_exit_cycles") << '\n'
      << "self_refresh_exit_cycles = " << hbm_sim::config::model_field_text(spec, "self_refresh_exit_cycles") << "\n\n";
  if (spec.lpddr_family) {
    out << "lpddr_dual_bank_refresh = " << hbm_sim::config::model_field_text(spec, "lpddr_dual_bank_refresh") << "\n\n";
  }

  out << "[reliability]\n"
      << "supports_ecc = " << hbm_sim::config::model_field_text(spec, "supports_ecc") << '\n'
      << "metadata_bits_per_request = " << hbm_sim::config::model_field_text(spec, "metadata_bits_per_request") << '\n'
      << "ecc_bits_per_request = " << hbm_sim::config::model_field_text(spec, "ecc_bits_per_request") << '\n';
  if (!spec.lpddr_family) {
    out << "hbm_full_32_channel_stack = " << hbm_sim::config::model_field_text(spec, "hbm_full_32_channel_stack") << '\n'
        << "hbm_sid_interleave = " << spec.hbm_sid_interleave << '\n'
        << "hbm_pc_interleave = " << hbm_sim::config::model_field_text(spec, "hbm_pc_interleave") << '\n'
        << "hbm_edge_pairing = " << hbm_sim::config::model_field_text(spec, "hbm_edge_pairing") << '\n'
        << "hbm_strict_edge_pairing = " << hbm_sim::config::model_field_text(spec, "hbm_strict_edge_pairing") << '\n'
        << "hbm_edge_pairing_matrix = " << hbm_sim::config::model_field_text(spec, "hbm_edge_pairing_matrix") << '\n'
        << "hbm_sid_mapping = " << hbm_sim::config::model_field_text(spec, "hbm_sid_mapping") << '\n'
        << "hbm_ecc_scheme = " << hbm_sim::config::model_field_text(spec, "hbm_ecc_scheme") << '\n'
        << "hbm_ras_policy = " << hbm_sim::config::model_field_text(spec, "hbm_ras_policy") << '\n'
        << "hbm_link_crc_mode = " << hbm_sim::config::model_field_text(spec, "hbm_link_crc_mode") << '\n'
        << "hbm_link_retry_enabled = " << hbm_sim::config::model_field_text(spec, "hbm_link_retry_enabled") << '\n'
        << "hbm_link_crc_bits_per_request = " << hbm_sim::config::model_field_text(spec, "hbm_link_crc_bits_per_request") << '\n'
        << "hbm_ras_metadata_bits_per_request = " << hbm_sim::config::model_field_text(spec, "hbm_ras_metadata_bits_per_request") << '\n'
        << "hbm_ecc_bits_per_request = " << hbm_sim::config::model_field_text(spec, "hbm_ecc_bits_per_request") << '\n';
  } else {
    out << "lpddr_link_protection = " << hbm_sim::config::model_field_text(spec, "lpddr_link_protection") << '\n'
        << "lpddr_dynamic_efficiency = " << spec.lpddr_dynamic_efficiency << '\n'
        << "lpddr_efficiency_mode = " << hbm_sim::to_string(spec.lpddr_efficiency_mode) << '\n'
        << "lpddr_dvfs_mode = " << hbm_sim::to_string(spec.lpddr_dvfs_mode) << '\n'
        << "lpddr_low_data_rate_mbps = " << hbm_sim::config::model_field_text(spec, "lpddr_low_data_rate_mbps") << '\n'
        << "lpddr_wck_mode = " << hbm_sim::to_string(spec.lpddr_wck_mode) << '\n'
        << "lpddr_wck_ratio = " << hbm_sim::config::model_field_text(spec, "lpddr_wck_ratio") << '\n'
        << "lpddr_mode_register_profile = " << hbm_sim::config::model_field_text(spec, "lpddr_mode_register_profile") << '\n'
        << "lpddr_wck_training_mode = " << hbm_sim::config::model_field_text(spec, "lpddr_wck_training_mode") << '\n'
        << "lpddr_dvfs_transition_policy = " << hbm_sim::config::model_field_text(spec, "lpddr_dvfs_transition_policy") << '\n'
        << "lpddr_link_protection_mode = " << hbm_sim::config::model_field_text(spec, "lpddr_link_protection_mode") << '\n'
        << "lpddr_low_power_state_policy = " << hbm_sim::config::model_field_text(spec, "lpddr_low_power_state_policy") << '\n'
        << "lpddr_wck_training_required = " << hbm_sim::config::model_field_text(spec, "lpddr_wck_training_required") << '\n'
        << "lpddr_dbi_enabled = " << hbm_sim::config::model_field_text(spec, "lpddr_dbi_enabled") << '\n'
        << "lpddr_link_ecc_enabled = " << hbm_sim::config::model_field_text(spec, "lpddr_link_ecc_enabled") << '\n'
        << "lpddr_ca_parity_enabled = " << hbm_sim::config::model_field_text(spec, "lpddr_ca_parity_enabled") << '\n'
        << "lpddr_dbi_bits_per_request = " << hbm_sim::config::model_field_text(spec, "lpddr_dbi_bits_per_request") << '\n'
        << "lpddr_link_ecc_bits_per_request = " << hbm_sim::config::model_field_text(spec, "lpddr_link_ecc_bits_per_request") << '\n'
        << "lpddr_ca_parity_bits_per_command = " << hbm_sim::config::model_field_text(spec, "lpddr_ca_parity_bits_per_command") << '\n';
  }

  // 每种来源使用独立 timing subsection，使导出的配置重新加载后仍能保留
  // JEDEC/vendor/derived/external/research provenance，而不只是保留数值。
  std::map<std::string, std::vector<const hbm_sim::TimingTableEntry*>> timing_groups;
  for (const auto& entry : spec.timing_table.entries) {
    timing_groups[hbm_sim::to_string(entry.source)].push_back(&entry);
  }
  for (const auto& [source, entries] : timing_groups) {
    out << "\n[timing." << source << "]\n"
        << "source = " << source << '\n';
    for (const auto* entry : entries) {
      out << entry->name << " = " << entry->value_nck << '\n';
    }
  }
  const auto& storage = cli.storage_model;
  out << "\n[storage]\n"
      << "backend = " << hbm_sim::to_string(storage.memory_backend.kind) << '\n'
      << "capacity_bytes = " << storage.memory_backend.capacity_bytes << '\n'
      << "chunk_size = " << storage.memory_backend.chunk_size_bytes << '\n'
      << "chunk_cache_entries = " << storage.memory_backend.chunk_cache_entries << '\n'
      << "sparse_density_warning_pct = " << storage.sparse_density_warning_pct << '\n'
      << "topology_stats_scan_limit = " << storage.topology_stats_scan_limit << '\n'
      << "floorplan = " << storage.floorplan_enabled << '\n'
      << "subarrays_per_bank = " << storage.subarrays_per_bank << '\n'
      << "mats_per_subarray_x = " << storage.mats_per_subarray_x << '\n'
      << "mats_per_subarray_y = " << storage.mats_per_subarray_y << '\n'
      << "cells_per_mat_x = " << storage.cells_per_mat_x << '\n'
      << "cells_per_mat_y = " << storage.cells_per_mat_y << '\n'
      << "microbumps_x = " << storage.microbumps_x << '\n'
      << "microbumps_y = " << storage.microbumps_y << '\n';
  if (!storage.memory_backend.data_file.empty()) {
    out << "data_file = " << storage.memory_backend.data_file << '\n';
  }
  if (!storage.memory_backend.init_file.empty()) {
    out << "init_file = " << storage.memory_backend.init_file << '\n';
  }
  if (!storage.memory_backend.meta_file.empty()) {
    out << "meta_file = " << storage.memory_backend.meta_file << '\n';
  }
  if (!storage.memory_backend.presence_file.empty()) {
    out << "presence_file = " << storage.memory_backend.presence_file << '\n';
  }
  out << "[reliability.payload]\n"
      << "ecc_shadow = " << storage.ecc_shadow_enabled << '\n'
      << "ecc_check_on_read = " << storage.ecc_check_on_read << '\n'
      << "ecc_correct_single_bit = " << storage.ecc_correct_single_bit << '\n'
      << "ecc_inject_period = " << storage.ecc_inject_period << "\n\n";
  out << "[power]\n"
      << "enabled = " << storage.power_enabled << '\n'
      << "source = " << storage.power_source << '\n'
      << "scale = " << storage.power_scale << '\n'
      << "power_act_pj = " << storage.act_energy_pj << '\n'
      << "power_act1_pj = " << storage.act1_energy_pj << '\n'
      << "power_act2_pj = " << storage.act2_energy_pj << '\n'
      << "power_pre_pj = " << storage.pre_energy_pj << '\n'
      << "power_preab_pj = " << storage.preab_energy_pj << '\n'
      << "power_cas_pj = " << storage.cas_energy_pj << '\n'
      << "power_read_pj = " << storage.read_energy_pj << '\n'
      << "power_read_per_byte_pj = " << storage.read_energy_per_byte_pj << '\n'
      << "power_write_pj = " << storage.write_energy_pj << '\n'
      << "power_write_per_byte_pj = " << storage.write_energy_per_byte_pj << '\n'
      << "power_refpb_pj = " << storage.refpb_energy_pj << '\n'
      << "power_refdb_pj = " << storage.refdb_energy_pj << '\n'
      << "power_refab_pj = " << storage.refab_energy_pj << '\n'
      << "power_rfmpb_pj = " << storage.rfmpb_energy_pj << '\n'
      << "power_rfmab_pj = " << storage.rfmab_energy_pj << '\n'
      << "power_control_pj = " << storage.control_energy_pj << '\n'
      << "power_vdd = " << storage.idd_vdd << '\n'
      << "idd0 = " << storage.idd0_ma << '\n'
      << "idd2n = " << storage.idd2n_ma << '\n'
      << "idd3n = " << storage.idd3n_ma << '\n'
      << "idd4r = " << storage.idd4r_ma << '\n'
      << "idd4w = " << storage.idd4w_ma << '\n'
      << "idd5ab = " << storage.idd5ab_ma << '\n'
      << "idd5pb = " << storage.idd5pb_ma << '\n'
      << "idd6x = " << storage.idd6x_ma << '\n'
      << "idd_devices_per_rank = " << storage.idd_devices_per_rank << '\n'
      << "idd_burst_cycles = " << storage.idd_burst_cycles << "\n\n";
  out << "[thermal]\n"
      << "enabled = " << storage.thermal_enabled << '\n'
      << "ambient_c = " << storage.thermal_ambient_c << '\n'
      << "cooling_per_cycle = " << storage.thermal_cooling_per_cycle << '\n'
      << "rise_c_per_pj = " << storage.thermal_rise_c_per_pj << '\n'
      << "thermal_grid_cols_per_tile = " << storage.thermal_grid_cols_per_tile << '\n'
      << "thermal_grid_rows_per_tile = " << storage.thermal_grid_rows_per_tile << '\n'
      << "thermal_coupling = " << storage.thermal_coupling_enabled << '\n'
      << "lateral_coupling = " << storage.thermal_lateral_coupling << '\n'
      << "vertical_coupling = " << storage.thermal_vertical_coupling << '\n'
      << "thermal_tsv_coupling_scale = " << storage.thermal_tsv_coupling_scale << '\n'
      << "thermal_tsvs_per_grid = " << storage.thermal_tsvs_per_grid << '\n'
      << "thermal_chip_dim_x_m = " << storage.thermal_chip_dim_x_m << '\n'
      << "thermal_chip_dim_y_m = " << storage.thermal_chip_dim_y_m << '\n'
      << "thermal_tsv_radius_m = " << storage.thermal_tsv_radius_m << '\n'
      << "thermal_k_silicon = " << storage.thermal_k_silicon << '\n'
      << "thermal_k_copper = " << storage.thermal_k_copper << "\n\n";

  out << "[outputs]\n"
      << "validate_cmd_trace = " << cli.validate_cmd_trace << '\n'
      << "validate_dfi_trace = " << cli.validate_dfi_trace << '\n'
      << "fail_on_data_mismatch = " << cli.fail_on_data_mismatch << '\n';
  const auto write_path = [&](const char* key, const std::string& value) {
    if (!value.empty()) out << key << " = " << value << '\n';
  };
  write_path("cmd_trace", cli.cmd_trace_path);
  write_path("response_trace", cli.response_trace_path);
  write_path("transaction_response_trace",
             cli.transaction_response_trace_path);
  write_path("dfi_trace", cli.dfi_trace_path);
  write_path("dfi_signal_trace", cli.dfi_signal_trace_path);
  write_path("dump_timing_table", cli.timing_table_path);
  write_path("stats_json", cli.stats_json_path);
  write_path("memory_image", cli.memory_image_path);
  write_path("dump_memory_image", cli.dump_memory_image_path);
  write_path("dump_memory_csv", cli.dump_memory_csv_path);
  write_path("mismatch_report", cli.mismatch_report_path);
  write_path("verify_golden", cli.verify_golden_path);
  write_path("dump_thermal_map", cli.thermal_map_path);
}

void write_resolved_config(const std::string& path, const Cli& cli,
                           const hbm_sim::DramSpec& spec) {
  ensure_parent_directory(path);
  std::ofstream out(path);
  if (!out) throw std::runtime_error("failed to open resolved config output: " + path);
  serialize_resolved_config(out, cli, spec);
  out.close();
  if (!out) throw std::runtime_error("failed to write resolved config: " + path);
}

}  // namespace

int main(int argc, char** argv) {
  std::string result_path;
  hbm_sim::ResultFields full_stats;
  hbm_sim::ResultReport report;
  try {
    Cli cli = parse_args(argc, argv);
    if (handle_list_commands(cli)) return 0;
    if (!cli.stats_json_path.empty()) {
      const auto output = std::filesystem::weakly_canonical(cli.stats_json_path);
      auto check_distinct = [&](const std::string& path) {
        if (!path.empty() && output == std::filesystem::weakly_canonical(path))
          throw std::invalid_argument("stats_json must not overwrite another input/output: " + path);
      };
      for (const auto& document : cli.config_documents) check_distinct(document.path);
      for (const auto& path : {cli.trace_path, cli.memory_image_path, cli.verify_golden_path,
                              cli.cmd_trace_path, cli.response_trace_path,
                              cli.transaction_response_trace_path, cli.dfi_trace_path,
                              cli.dfi_signal_trace_path, cli.timing_table_path,
                              cli.dump_resolved_config_path, cli.dump_config_diff_path,
                              cli.dump_memory_image_path, cli.dump_memory_csv_path,
                              cli.mismatch_report_path, cli.thermal_map_path})
        check_distinct(path);
      const auto& backend_paths = cli.storage_model.memory_backend;
      for (const auto& path : {backend_paths.data_file, backend_paths.init_file,
                              backend_paths.meta_file, backend_paths.presence_file})
        check_distinct(path);
      if (!backend_paths.data_file.empty()) {
        check_distinct(backend_paths.data_file + ".init");
        check_distinct(backend_paths.data_file + ".meta");
        check_distinct(backend_paths.data_file + ".present");
      }
      for (int stack = 0; stack < cli.stack_count; ++stack) {
        for (const auto& path : {cli.dump_memory_image_path, cli.dump_memory_csv_path,
                                cli.thermal_map_path, cli.verify_golden_path,
                                backend_paths.data_file, backend_paths.init_file,
                                backend_paths.meta_file, backend_paths.presence_file})
          check_distinct(stack_path(path, stack, cli.stack_count));
        const auto data = stack_path(backend_paths.data_file, stack, cli.stack_count);
        if (!data.empty()) {
          check_distinct(data + ".init");
          check_distinct(data + ".meta");
          check_distinct(data + ".present");
        }
      }
    }
    result_path = cli.stats_json_path;
    // Invalidate an older result before execution so a failed rerun cannot leave
    // an apparently successful artifact for experiment tools to consume.
    hbm_sim::write_result_json(result_path, {}, "failed", "run has not completed");
    resolve_response_delivery(cli);
    // CLI 使用 draft，等 profile 选择项和显式覆盖全部就绪后只 finalize 一次。
    // 所有 HBM/LPDDR 差异仍从 DramSpec 传入共享 Controller。
    hbm_sim::DramSpec spec = hbm_sim::config::build_model(
        cli.standard, cli.spec_overrides, cli.config_schema_version, &cli.derived_parameters);
    const bool require_device_timing =
        cli.validation_mode == hbm_sim::config::ValidationMode::Device;
    std::vector<std::string> timing_errors =
        hbm_sim::validate_timing_table(spec, cli.strict_timing_table || require_device_timing);
    if (!timing_errors.empty()) {
      // strict_timing_table 不是“能不能跑”的检查，而是“能不能做真实器件数值级
      // 对比”的检查。默认允许研究默认值，严格模式要求 vendor-required 项被覆盖。
      std::string msg = "timing table validation failed:";
      for (const auto& error : timing_errors) {
        msg += "\n  - " + error;
      }
      throw std::runtime_error(msg);
    }
    std::vector<std::string> protocol_errors = validate_protocol_config(spec);
    if (!protocol_errors.empty()) {
      std::string msg = "protocol config validation failed:";
      for (const auto& error : protocol_errors) {
        msg += "\n  - " + error;
      }
      throw std::runtime_error(msg);
    }
    std::vector<std::string> storage_errors = validate_storage_model_config(cli.storage_model);
    if (!storage_errors.empty()) {
      std::string msg = "storage model config validation failed:";
      for (const auto& error : storage_errors) {
        msg += "\n  - " + error;
      }
      throw std::runtime_error(msg);
    }
    std::vector<std::string> phy_errors = validate_phy_config(cli.controller.phy);
    if (!phy_errors.empty()) {
      std::string msg = "PHY config validation failed:";
      for (const auto& error : phy_errors) msg += "\n  - " + error;
      throw std::runtime_error(msg);
    }
    std::vector<std::string> runtime_errors = validate_runtime_config(cli, spec);
    if (!runtime_errors.empty()) {
      std::string msg = "runtime/config validation failed:";
      for (const auto& error : runtime_errors) msg += "\n  - " + error;
      throw std::runtime_error(msg);
    }

    if (cli.stack_count <= 0) {
      throw std::invalid_argument("stack_count must be positive");
    }
    if (cli.stack_mapping == hbm_sim::StackMappingKind::Interleaved &&
        (cli.stack_interleave_bytes <
             static_cast<std::uint64_t>(spec.org.line_size) ||
         cli.stack_interleave_bytes %
                 static_cast<std::uint64_t>(spec.org.line_size) !=
             0)) {
      throw std::invalid_argument(
          "interleaved stack mapping requires stack_interleave_bytes to be a "
          "positive multiple of line_size");
    }
    if (cli.single_controller && cli.stack_count != 1) {
      throw std::invalid_argument("single_controller is only valid with stack_count=1");
    }
    if (cli.single_controller &&
        cli.response_delivery_mode != hbm_sim::ResponseDeliveryMode::Disabled) {
      throw std::invalid_argument(
          "response delivery requires the default MemorySystem path; "
          "single_controller has no system-level response aggregation");
    }

    if (!cli.dump_resolved_config_path.empty()) {
      write_resolved_config(cli.dump_resolved_config_path, cli, spec);
    }
    if (cli.compare_preset) write_config_diff(std::cout, cli);
    if (!cli.dump_config_diff_path.empty()) {
      ensure_parent_directory(cli.dump_config_diff_path);
      std::ofstream diff_out(cli.dump_config_diff_path);
      if (!diff_out) {
        throw std::runtime_error("failed to open config diff output: " + cli.dump_config_diff_path);
      }
      write_config_diff(diff_out, cli);
    }
    if (!cli.explain_config_key.empty()) explain_config(cli, cli.explain_config_key);
    if (cli.check_config) {
      std::cout << "config_validation=pass\n"
                << "model_name=" << cli.model_name << '\n'
                << "base_standard=" << spec.name << '\n'
                << "preset=" << (cli.preset.empty() ? "<none>" : cli.preset) << '\n'
                << "validation_mode=" << hbm_sim::config::to_string(cli.validation_mode) << '\n'
                << "modified_parameters=" << config_differences(cli).size() << '\n';
      return 0;
    }

    // 输出路径可以直接指向 outputs/<run-name>/...；CLI 负责建立父目录，用户
    // 不需要在每次仿真前手工 mkdir。输入文件路径不会在这里创建或改写。
    prepare_stack_output_directories(cli.cmd_trace_path, 1);
    prepare_stack_output_directories(cli.response_trace_path, 1);
    prepare_stack_output_directories(cli.transaction_response_trace_path, 1);
    prepare_stack_output_directories(cli.dfi_trace_path, 1);
    prepare_stack_output_directories(cli.dfi_signal_trace_path, 1);
    prepare_stack_output_directories(cli.timing_table_path, 1);
    prepare_stack_output_directories(cli.dump_memory_image_path, cli.stack_count);
    prepare_stack_output_directories(cli.dump_memory_csv_path, cli.stack_count);
    prepare_stack_output_directories(cli.thermal_map_path, cli.stack_count);
    prepare_stack_output_directories(cli.mismatch_report_path, 1);

    std::vector<std::shared_ptr<hbm_sim::MemoryImage>> stack_memory_images;
    stack_memory_images.reserve(static_cast<std::size_t>(cli.stack_count));
    for (int stack = 0; stack < cli.stack_count; stack++) {
      hbm_sim::StorageModelOptions stack_options = cli.storage_model;
      stack_options.stack_id = stack;
      auto& backend = stack_options.memory_backend;
      backend.data_file = stack_path(backend.data_file, stack, cli.stack_count);
      backend.init_file = stack_path(backend.init_file, stack, cli.stack_count);
      backend.meta_file = stack_path(backend.meta_file, stack, cli.stack_count);
      backend.presence_file = stack_path(backend.presence_file, stack, cli.stack_count);
      ensure_parent_directory(backend.data_file);
      ensure_parent_directory(backend.init_file);
      ensure_parent_directory(backend.meta_file);
      ensure_parent_directory(backend.presence_file);
      stack_memory_images.push_back(
          std::make_shared<hbm_sim::MemoryImage>(spec, 0, std::move(stack_options)));
    }
    auto memory_image = stack_memory_images.front();
    if (!cli.memory_image_path.empty()) {
      // 输入 image 使用系统全局地址。先读入临时稀疏镜像，再按 stack mapper
      // 分发成各 stack 的局部地址，从而保持旧文本/二进制格式兼容。
      hbm_sim::StorageModelOptions source_options = cli.storage_model;
      source_options.memory_backend = {};
      auto source_image = std::make_shared<hbm_sim::MemoryImage>(spec, 0, source_options);
      source_image->load_file(cli.memory_image_path);
      hbm_sim::StackAddressMapper stack_mapper(cli.stack_count,
                                                cli.stack_interleave_bytes,
                                                spec.addressable_capacity_bytes(),
                                                cli.stack_mapping);
      for (hbm_sim::Address system_base : source_image->all_addresses()) {
        hbm_sim::StackAddress routed = stack_mapper.decode(system_base);
        hbm_sim::ByteVector data = source_image->read(system_base, source_image->line_size());
        hbm_sim::ByteVector mask =
            source_image->read_initialized_mask(system_base, source_image->line_size());
        hbm_sim::DecodedAddress decoded = hbm_sim::AddressMapper(spec).decode(routed.local_address);
        stack_memory_images[static_cast<std::size_t>(routed.stack)]->write(
            routed.local_address, data, &mask, &decoded, 0, 0);
      }
    }
    auto data_validator = std::make_shared<hbm_sim::DataValidator>();
    cli.controller.memory_image = memory_image;
    cli.controller.data_validator = data_validator;

    hbm_sim::TrafficOptions traffic;
    traffic.pattern = cli.pattern;
    traffic.trace_path = cli.trace_path;
    traffic.requests = cli.requests;
    traffic.read_ratio = cli.read_ratio;
    traffic.seed = cli.seed;
    traffic.random_address_space_bytes = cli.random_address_space_bytes;
    traffic.addr_stride = cli.addr_stride;
    traffic.inject_interval = cli.inject_interval;
    traffic.init_sequence = cli.init_sequence;
    traffic.init_sequence_interval = cli.init_sequence_interval;
    traffic.stack_count = cli.stack_count;

    // CLI 默认使用流式 source：synthetic 请求按需生成，trace 逐行解析，burst
    // 逐 cache line 展开。内存占用由 controller queue 决定，不再与请求总数线性增长。
    std::unique_ptr<hbm_sim::TrafficStream> request_stream =
        hbm_sim::make_traffic_stream(spec, traffic);
    const bool need_command_trace = cli.validate_cmd_trace ||
                                    cli.validate_dfi_trace ||
                                    !cli.cmd_trace_path.empty() ||
                                    !cli.dfi_trace_path.empty() ||
                                    !cli.dfi_signal_trace_path.empty();
    cli.controller.retain_command_trace = need_command_trace;

    hbm_sim::Stats stats;
    std::vector<hbm_sim::IssuedCommand> issued_commands;
    std::vector<hbm_sim::Stats> per_stack_stats;
    std::uint64_t exported_host_responses = 0;
    std::uint64_t exported_transaction_responses = 0;
    std::uint64_t consumed_host_responses = 0;
    std::uint64_t consumed_transaction_responses = 0;
    const auto progress_consumer = [](const hbm_sim::RunProgress& progress) {
      std::cerr << "progress cycle=" << progress.cycle
                << " completed_reads=" << progress.completed_reads
                << " completed_writes=" << progress.completed_writes
                << " remaining_frontend=" << progress.remaining_frontend
                << '\n';
    };
    if (cli.single_controller) {
      // 单控制器路径保留给早期模型兼容和最小化调试；它不会体现 stack-level
      // 多 channel 并行，因此正式带宽实验通常应使用默认 MemorySystem。
      cli.controller.retain_responses = false;
      hbm_sim::Controller controller(spec, cli.controller);
      controller.run(*request_stream, cli.max_cycles, cli.progress_interval,
                     progress_consumer);
      stats = controller.stats();
      issued_commands = controller.issued_commands();
    } else {
      // MemorySystem 是 Ramulator2.1 风格的多 channel 外壳：每个 channel 一个
      // Controller，同一个 system cycle 内并行 tick，再聚合统计和命令 trace。
      hbm_sim::MemorySystemOptions memory_options;
      memory_options.controller = cli.controller;
      memory_options.channel_mapper = cli.channel_mapper;
      memory_options.stack_count = cli.stack_count;
      memory_options.stack_mapping = cli.stack_mapping;
      memory_options.stack_interleave_bytes = cli.stack_interleave_bytes;
      memory_options.stack_memory_images = stack_memory_images;
      memory_options.stack_ingress_buffer_size = cli.stack_ingress_buffer_size;
      memory_options.stack_dispatch_width = cli.stack_dispatch_width;
      memory_options.stack_qos_policy = cli.stack_qos_policy;
      memory_options.response_delivery_mode = cli.response_delivery_mode;
      memory_options.host_response_queue_capacity =
          cli.host_response_queue_capacity;
      memory_options.transaction_response_queue_capacity =
          cli.transaction_response_queue_capacity;
      hbm_sim::MemorySystem memory(spec, memory_options);
      if (cli.response_delivery_mode ==
          hbm_sim::ResponseDeliveryMode::Disabled) {
        memory.run(*request_stream, cli.max_cycles, cli.progress_interval,
                   progress_consumer);
      } else {
        std::ofstream host_response_trace;
        if (!cli.response_trace_path.empty()) {
          host_response_trace.open(cli.response_trace_path);
          if (!host_response_trace) {
            throw std::runtime_error(
                "failed to open host response trace output: " +
                cli.response_trace_path);
          }
          write_host_response_csv_header(host_response_trace);
        }
        std::ofstream transaction_response_trace;
        if (!cli.transaction_response_trace_path.empty()) {
          transaction_response_trace.open(
              cli.transaction_response_trace_path);
          if (!transaction_response_trace) {
            throw std::runtime_error(
                "failed to open transaction response trace output: " +
                cli.transaction_response_trace_path);
          }
          write_transaction_response_csv_header(transaction_response_trace);
        }

        hbm_sim::MemorySystem::RunOptions run_options;
        run_options.drain_responses = true;
        run_options.progress_interval = cli.progress_interval;
        run_options.progress = progress_consumer;
        if (cli.response_delivery_mode == hbm_sim::ResponseDeliveryMode::HostOnly ||
            cli.response_delivery_mode == hbm_sim::ResponseDeliveryMode::Both) {
          run_options.host_response = [&](const hbm_sim::HostResponse& response) {
            consumed_host_responses++;
            if (host_response_trace.is_open()) {
              write_host_response_csv_row(host_response_trace, response);
              exported_host_responses++;
            }
          };
        }
        if (cli.response_delivery_mode == hbm_sim::ResponseDeliveryMode::TransactionOnly ||
            cli.response_delivery_mode == hbm_sim::ResponseDeliveryMode::Both) {
          run_options.transaction_response = [&](const hbm_sim::TransactionResponse& response) {
            consumed_transaction_responses++;
            if (transaction_response_trace.is_open()) {
              write_transaction_response_csv_row(transaction_response_trace, response);
              exported_transaction_responses++;
            }
          };
        }
        memory.run(*request_stream, cli.max_cycles, run_options);
      }
      stats = memory.stats();
      issued_commands = memory.issued_commands();
      per_stack_stats = memory.per_stack_stats();
    }

    const hbm_sim::TrafficStreamStats& stream_stats = request_stream->stream_stats();
    stats.host_requests = stream_stats.host_requests;
    stats.dram_transactions = stream_stats.dram_transactions;
    stats.burst_trace_lines = stream_stats.burst_trace_lines;
    stats.burst_split_requests = stream_stats.burst_split_requests;
    stats.burst_read_requests = stream_stats.burst_read_requests;
    stats.burst_write_requests = stream_stats.burst_write_requests;
    stats.burst_read_bytes = stream_stats.burst_read_bytes;
    stats.burst_write_bytes = stream_stats.burst_write_bytes;

    hbm_sim::CommandValidationReport validation_report;
    if (cli.validate_cmd_trace) {
      validation_report = hbm_sim::validate_command_trace(spec, issued_commands);
      if (!validation_report.ok()) {
        std::string msg = "command trace validation failed:";
        for (const auto& error : validation_report.errors) {
          msg += "\n  - " + error;
        }
        throw std::runtime_error(msg);
      }
    }

    std::vector<hbm_sim::DfiEvent> dfi_events;
    hbm_sim::DfiValidationReport dfi_validation_report;
    const bool need_dfi_events = cli.validate_dfi_trace ||
                                 !cli.dfi_trace_path.empty() ||
                                 !cli.dfi_signal_trace_path.empty();
    if (need_dfi_events) {
      dfi_events = hbm_sim::build_dfi_trace(spec, issued_commands);
    }
    if (cli.validate_dfi_trace) {
      dfi_validation_report = hbm_sim::validate_dfi_trace(spec, issued_commands, dfi_events);
      if (!dfi_validation_report.ok()) {
        std::string msg = "DFI trace validation failed:";
        for (const auto& error : dfi_validation_report.errors) {
          msg += "\n  - " + error;
        }
        throw std::runtime_error(msg);
      }
    }

    if (!cli.cmd_trace_path.empty()) {
      hbm_sim::write_command_trace_csv(cli.cmd_trace_path, issued_commands);
    }
    if (!cli.dfi_trace_path.empty() || !cli.dfi_signal_trace_path.empty()) {
      if (!cli.dfi_trace_path.empty()) {
        hbm_sim::write_dfi_trace_csv(cli.dfi_trace_path, dfi_events);
      }
      if (!cli.dfi_signal_trace_path.empty()) {
        hbm_sim::write_dfi_signal_trace_csv(cli.dfi_signal_trace_path, dfi_events);
      }
    }
    if (!cli.timing_table_path.empty()) {
      write_timing_table_csv(cli.timing_table_path, spec.timing_table);
    }
    if (!cli.dump_memory_image_path.empty()) {
      for (int stack = 0; stack < cli.stack_count; stack++) {
        stack_memory_images[static_cast<std::size_t>(stack)]->dump_file(
            stack_path(cli.dump_memory_image_path, stack, cli.stack_count));
      }
    }
    if (!cli.dump_memory_csv_path.empty()) {
      for (int stack = 0; stack < cli.stack_count; stack++) {
        stack_memory_images[static_cast<std::size_t>(stack)]->dump_csv(
            stack_path(cli.dump_memory_csv_path, stack, cli.stack_count));
      }
    }
    if (!cli.thermal_map_path.empty()) {
      for (int stack = 0; stack < cli.stack_count; stack++) {
        stack_memory_images[static_cast<std::size_t>(stack)]->dump_thermal_text(
            stack_path(cli.thermal_map_path, stack, cli.stack_count));
      }
    }
    if (!cli.mismatch_report_path.empty()) {
      data_validator->dump_text(cli.mismatch_report_path);
    }

    // Golden 文件验证：将仿真后的存储区与预期 golden 文件逐地址比对。
    // 同时检验 payload 字节和 initialized mask，避免"恰好默认值相同"的假阴性。
    // 典型用法：先用纯写负载运行并 --dump-memory-image golden.bin，
    // 再用读负载 --verify-golden golden.bin 验证数据完整性。
    if (!cli.verify_golden_path.empty()) {
      hbm_sim::StorageModelOptions golden_options = cli.storage_model;
      golden_options.memory_backend = {};
      std::vector<std::shared_ptr<hbm_sim::MemoryImage>> golden_images;
      for (int stack = 0; stack < cli.stack_count; stack++) {
        golden_options.stack_id = stack;
        golden_images.push_back(
            std::make_shared<hbm_sim::MemoryImage>(spec, 0, golden_options));
      }
      const bool per_stack_golden =
          cli.stack_count > 1 &&
          (cli.verify_golden_path.find("{stack}") != std::string::npos ||
           std::filesystem::exists(
               stack_path(cli.verify_golden_path, 0, cli.stack_count)));
      if (per_stack_golden) {
        for (int stack = 0; stack < cli.stack_count; stack++) {
          golden_images[static_cast<std::size_t>(stack)]->load_file(
              stack_path(cli.verify_golden_path, stack, cli.stack_count));
        }
      } else {
        auto global_golden =
            std::make_shared<hbm_sim::MemoryImage>(spec, 0, golden_options);
        global_golden->load_file(cli.verify_golden_path);
        hbm_sim::StackAddressMapper stack_mapper(
            cli.stack_count, cli.stack_interleave_bytes,
            spec.addressable_capacity_bytes(), cli.stack_mapping);
        for (hbm_sim::Address system_base : global_golden->all_addresses()) {
          hbm_sim::StackAddress routed = stack_mapper.decode(system_base);
          hbm_sim::ByteVector data =
              global_golden->read(system_base, global_golden->line_size());
          hbm_sim::ByteVector mask = global_golden->read_initialized_mask(
              system_base, global_golden->line_size());
          hbm_sim::DecodedAddress decoded =
              hbm_sim::AddressMapper(spec).decode(routed.local_address);
          golden_images[static_cast<std::size_t>(routed.stack)]->write(
              routed.local_address, data, &mask, &decoded, 0, 0);
        }
      }
      std::uint64_t verified = 0;
      std::uint64_t mismatches = 0;
      std::uint64_t golden_uninitialized_lines = 0;
      std::uint64_t actual_uninitialized_lines = 0;
      std::vector<std::string> mismatch_lines;
      auto fully_initialized = [](const hbm_sim::ByteVector& mask) {
        return std::all_of(mask.begin(), mask.end(),
                           [](std::uint8_t byte) { return byte != 0; });
      };
      hbm_sim::StackAddressMapper stack_mapper(
          cli.stack_count, cli.stack_interleave_bytes,
          spec.addressable_capacity_bytes(), cli.stack_mapping);
      // 每颗 stack 使用局部地址比较；错误信息同时报告 stack 和系统地址。
      for (int stack = 0; stack < cli.stack_count; stack++) {
        auto& golden = golden_images[static_cast<std::size_t>(stack)];
        auto& actual_image =
            stack_memory_images[static_cast<std::size_t>(stack)];
        for (hbm_sim::Address base : golden->all_addresses()) {
          auto golden_meta = golden->metadata(base);
          if (!golden_meta.has_value())
            continue;
          const std::size_t size = golden->line_size();
          hbm_sim::ByteVector actual = actual_image->read(base, size);
          hbm_sim::ByteVector expected = golden->read(base, size);
          hbm_sim::ByteVector actual_init_mask =
              actual_image->read_initialized_mask(base, size);
          hbm_sim::ByteVector expected_init_mask =
              golden->read_initialized_mask(base, size);
          bool golden_init = fully_initialized(expected_init_mask);
          bool actual_init = fully_initialized(actual_init_mask);
          verified++;

          bool payload_match = (actual == expected);
          bool init_match = (actual_init_mask == expected_init_mask);
          if (!payload_match || !init_match) {
            mismatches++;
            if (mismatch_lines.size() < 32) {
              std::ostringstream os;
              os << "Golden mismatch at stack=" << stack
                 << " local=" << hbm_sim::format_address(base) << " system="
                 << hbm_sim::format_address(stack_mapper.encode(stack, base))
                 << ": expected=" << hbm_sim::bytes_to_hex(expected)
                 << " actual=" << hbm_sim::bytes_to_hex(actual)
                 << " expected_init_mask="
                 << hbm_sim::bytes_to_hex(expected_init_mask)
                 << " actual_init_mask="
                 << hbm_sim::bytes_to_hex(actual_init_mask)
                 << " golden_init=" << (golden_init ? "true" : "false")
                 << " actual_init=" << (actual_init ? "true" : "false");
              mismatch_lines.push_back(os.str());
            }
          }
          if (!golden_init) {
            golden_uninitialized_lines++;
          }
          if (!actual_init) {
            actual_uninitialized_lines++;
          }
        }
      }
      hbm_sim::record_field(full_stats, "golden_verified", verified);
      hbm_sim::record_field(full_stats, "golden_mismatches", mismatches);
      hbm_sim::record_field(full_stats, "golden_uninitialized_lines", golden_uninitialized_lines);
      hbm_sim::record_field(full_stats, "golden_actual_uninitialized_lines", actual_uninitialized_lines);
      if (mismatches > 0) {
        hbm_sim::print_diagnostics(cli.stats_view == "diagnostic" ? std::cout : std::cerr,
                                   full_stats);
        for (const auto& line : mismatch_lines) {
          std::cerr << line << '\n';
        }
        throw std::runtime_error(
            "golden verification failed: " + std::to_string(mismatches) + " mismatches");
      }
    }
    // max_cycles 命中时，run() 会设置 hit_cycle_limit，main()
    // 最终用退出码 2 告诉脚本“不是语法错误，而是仿真未完成”。

    // Collect typed effective configuration and counters. The public report
    // selects semantic fields; the complete catalog is diagnostic-only.
    const auto& storage_options = memory_image->options();
    const auto model_differences = config_differences(cli);
    const char* conformance =
        cli.validation_mode == hbm_sim::config::ValidationMode::Device
            ? "device_checked"
            : (cli.validation_mode == hbm_sim::config::ValidationMode::Standard
                   ? "standard_checked"
                   : "custom_exploratory");
    const std::string run_status =
        stats.hit_cycle_limit ? "truncated" :
        (stats.data_mismatches > 0 || stats.remaining_requests > 0 ||
         stats.remaining_pending > 0) ? "failed" : "completed";
    hbm_sim::record_field(full_stats, "run_status", run_status);
    hbm_sim::record_field(full_stats, "tick_duration_ps", spec.timing.tCK_ps / spec.tick_multiplier);
    hbm_sim::record_field(full_stats, "model_name", cli.model_name);
    hbm_sim::record_field(full_stats, "base_standard", spec.name);
    hbm_sim::record_field(full_stats, "selected_preset", cli.preset.empty() ? "none" : cli.preset);
    hbm_sim::record_field(full_stats, "validation_mode", hbm_sim::config::to_string(cli.validation_mode));
    hbm_sim::record_field(full_stats, "model_conformance", conformance);
    hbm_sim::record_field(full_stats, "modified_parameters", model_differences.size());
    // Output names may differ from configuration aliases; typed values share
    // the registry used to construct and export the executed model.
    for (const auto& [output_key, config_key] : {
        std::pair{"timing_profile", "timing_profile"},
        std::pair{"vendor_profile", "vendor_profile"},
        std::pair{"mode_profile", "mode_profile"},
        std::pair{"speed_bin_mbps", "speed_bin_mbps"},
        std::pair{"density_gb", "density_gb"},
        std::pair{"stack_height", "stack_height"},
        std::pair{"data_rate_mbps", "data_rate_mbps"},
        std::pair{"data_bus_bits", "data_bus_bits"},
        std::pair{"rows", "rows"},
        std::pair{"columns", "columns"},
        std::pair{"ranks", "ranks"},
        std::pair{"bank_groups", "bank_groups"},
        std::pair{"banks_per_group", "banks_per_group"},
        std::pair{"prefetch_size", "prefetch_size"},
        std::pair{"tick_multiplier", "tick_multiplier"},
        std::pair{"tCK_ps", "tck_ps"},
        std::pair{"full_stack_model", "full_stack_model"},
        std::pair{"supports_refresh", "supports_refresh"},
        std::pair{"lpddr_dual_bank_refresh", "lpddr_dual_bank_refresh"},
        std::pair{"supports_rfm", "supports_rfm"},
        std::pair{"supports_ecc", "supports_ecc"},
        std::pair{"hbm_full_32ch_stack", "hbm_full_32_channel_stack"},
        std::pair{"hbm_pc_interleave", "hbm_pc_interleave"},
        std::pair{"hbm_edge_pairing", "hbm_edge_pairing"},
        std::pair{"hbm_strict_edge_pairing", "hbm_strict_edge_pairing"},
        std::pair{"hbm_pairing_matrix", "hbm_edge_pairing_matrix"},
        std::pair{"hbm_sid_mapping", "hbm_sid_mapping"},
        std::pair{"hbm_ecc_scheme", "hbm_ecc_scheme"},
        std::pair{"hbm_ras_policy", "hbm_ras_policy"},
        std::pair{"hbm_link_crc_mode", "hbm_link_crc_mode"},
        std::pair{"hbm_link_retry", "hbm_link_retry_enabled"},
        std::pair{"rfm_act_threshold", "rfm_act_threshold"},
        std::pair{"rfm_decrement", "rfm_decrement"},
        std::pair{"lpddr_link_protection", "lpddr_link_protection"},
        std::pair{"lpddr_low_rate_mbps", "lpddr_low_data_rate_mbps"},
        std::pair{"lpddr_wck_ratio", "lpddr_wck_ratio"},
        std::pair{"lpddr_mr_profile", "lpddr_mode_register_profile"},
        std::pair{"lpddr_wck_training", "lpddr_wck_training_mode"},
        std::pair{"lpddr_dvfs_policy", "lpddr_dvfs_transition_policy"},
        std::pair{"lpddr_link_mode", "lpddr_link_protection_mode"},
        std::pair{"lpddr_low_power_policy", "lpddr_low_power_state_policy"},
        std::pair{"lpddr_wck_train_req", "lpddr_wck_training_required"},
        std::pair{"lpddr_dbi_enabled", "lpddr_dbi_enabled"},
        std::pair{"lpddr_link_ecc_enabled", "lpddr_link_ecc_enabled"},
        std::pair{"lpddr_ca_parity", "lpddr_ca_parity_enabled"},
        std::pair{"low_power_entry_cycles", "low_power_entry_cycles"},
        std::pair{"low_power_exit_cycles", "low_power_exit_cycles"},
        std::pair{"self_refresh_exit_cycles", "self_refresh_exit_cycles"},
        std::pair{"refresh_postpone_limit", "refresh_postpone_limit"},
        std::pair{"refresh_pullin_limit", "refresh_pullin_limit"},
        std::pair{"refresh_credit_limit", "refresh_credit_limit"},
        std::pair{"refresh_high_temp_mult", "refresh_high_temp_multiplier"},
        std::pair{"metadata_bits_per_req", "metadata_bits_per_request"},
        std::pair{"ecc_bits_per_req", "ecc_bits_per_request"},
        std::pair{"hbm_link_crc_bits_req", "hbm_link_crc_bits_per_request"},
        std::pair{"hbm_ras_meta_bits_req", "hbm_ras_metadata_bits_per_request"},
        std::pair{"hbm_ecc_bits_req", "hbm_ecc_bits_per_request"},
        std::pair{"lpddr_dbi_bits_req", "lpddr_dbi_bits_per_request"},
        std::pair{"lpddr_link_ecc_bits_req", "lpddr_link_ecc_bits_per_request"},
        std::pair{"lpddr_ca_parity_bits_cmd", "lpddr_ca_parity_bits_per_command"},
        std::pair{"line_size", "line_size"},
        std::pair{"channels", "channels"},
        std::pair{"pseudo_channels", "pseudo_channels"},
        std::pair{"sids", "sids"}}) {
      std::visit([&](const auto& value) {
        hbm_sim::record_field(full_stats, output_key, value);
      }, hbm_sim::config::model_field_value(spec, config_key));
    }
    hbm_sim::record_field(full_stats, "standard", spec.name);
    hbm_sim::record_field(full_stats, "stack_mapping", stack_mapping_name(cli.stack_mapping));
    hbm_sim::record_field(full_stats, "stack_interleave_bytes", cli.stack_interleave_bytes);
    hbm_sim::record_field(full_stats, "stack_ingress_buffer_size", cli.stack_ingress_buffer_size);
    hbm_sim::record_field(full_stats, "stack_dispatch_width", cli.stack_dispatch_width);
    hbm_sim::record_field(full_stats, "stack_qos_policy", stack_qos_policy_name(cli.stack_qos_policy));
    hbm_sim::record_field(full_stats, "aggregate_capacity_bytes",
                spec.addressable_capacity_bytes() * static_cast<std::uint64_t>(cli.stack_count));
    hbm_sim::record_field(full_stats, "dual_command_bus", spec.dual_command_bus);
    hbm_sim::record_field(full_stats, "split_activate", spec.split_activate);
    hbm_sim::record_field(full_stats, "lpddr_family", spec.lpddr_family);
    hbm_sim::record_field(full_stats, "dfi_phase_count", hbm_sim::dfi_phase_count(spec));
    hbm_sim::record_field(full_stats, "dfi_data_lane_bytes", hbm_sim::dfi_payload_beat_bytes(spec));
    hbm_sim::record_field(full_stats, "dfi_read_latency_nck",
                spec.dfi_read_latency_nck > 0 ? spec.dfi_read_latency_nck : spec.timing.nCL);
    hbm_sim::record_field(full_stats, "dfi_write_latency_nck",
                spec.dfi_write_latency_nck > 0 ? spec.dfi_write_latency_nck : spec.timing.nCWL);
    hbm_sim::record_field(full_stats, "mem_phy_mode", hbm_sim::to_string(cli.controller.phy.mode));
    hbm_sim::record_field(full_stats, "phy_protocol", spec.lpddr_family ? "lpddr" : "hbm");
    hbm_sim::record_field(full_stats, "dfi_version", cli.controller.phy.dfi_version);
    hbm_sim::record_field(full_stats, "phy_command_fifo_depth", cli.controller.phy.command_fifo_depth);
    hbm_sim::record_field(full_stats, "phy_read_fifo_depth", cli.controller.phy.read_fifo_depth);
    hbm_sim::record_field(full_stats, "phy_write_fifo_depth", cli.controller.phy.write_fifo_depth);
    hbm_sim::record_field(full_stats, "phy_command_pipeline", cli.controller.phy.command_pipeline_cycles);
    hbm_sim::record_field(full_stats, "phy_read_return_pipeline", cli.controller.phy.read_return_pipeline_cycles);
    hbm_sim::record_field(full_stats, "phy_write_data_pipeline", cli.controller.phy.write_data_pipeline_cycles);
    hbm_sim::record_field(full_stats, "phy_reset_config_cycles", cli.controller.phy.reset_cycles);
    hbm_sim::record_field(full_stats, "phy_init_config_cycles", cli.controller.phy.initialization_cycles);
    hbm_sim::record_field(full_stats, "phy_train_config_cycles", cli.controller.phy.training_cycles);
    hbm_sim::record_field(full_stats, "phy_auto_train", cli.controller.phy.auto_train);
    hbm_sim::record_field(full_stats, "refresh_policy", hbm_sim::to_string(spec.refresh_policy));
    hbm_sim::record_field(full_stats, "rfm_policy", hbm_sim::to_string(spec.rfm_policy));
    hbm_sim::record_field(full_stats, "hbm_sid_interleave", spec.hbm_sid_interleave);
    hbm_sim::record_field(full_stats, "lpddr_efficiency_mode", hbm_sim::to_string(spec.lpddr_efficiency_mode));
    hbm_sim::record_field(full_stats, "lpddr_dvfs_mode", hbm_sim::to_string(spec.lpddr_dvfs_mode));
    hbm_sim::record_field(full_stats, "lpddr_wck_mode", hbm_sim::to_string(spec.lpddr_wck_mode));
    hbm_sim::record_field(full_stats, "low_power_mode", hbm_sim::to_string(spec.low_power_mode));
    hbm_sim::record_field(full_stats, "refresh_temperature", hbm_sim::to_string(spec.refresh_temperature_mode));
    hbm_sim::record_field(full_stats, "lpddr_metadata_bits_req", hbm_sim::lpddr_metadata_lane_bits_per_request(spec));
    hbm_sim::record_field(full_stats, "timing_table_entries", spec.timing_table.entries.size());
    hbm_sim::record_field(full_stats, "timing_vendor_required", spec.timing_table.provisional_count());
    hbm_sim::record_field(full_stats, "timing_source_jedec",
                spec.timing_table.source_count(hbm_sim::TimingValueSource::JEDEC));
    hbm_sim::record_field(full_stats, "timing_source_vendor",
                spec.timing_table.source_count(hbm_sim::TimingValueSource::Vendor));
    hbm_sim::record_field(full_stats, "timing_source_derived",
                spec.timing_table.source_count(hbm_sim::TimingValueSource::Derived));
    hbm_sim::record_field(full_stats, "timing_source_reference",
                spec.timing_table.source_count(hbm_sim::TimingValueSource::ExternalReference));
    hbm_sim::record_field(full_stats, "timing_source_research",
                spec.timing_table.source_count(hbm_sim::TimingValueSource::ResearchDefault));
    hbm_sim::record_field(full_stats, "timing_vendor_required_only", spec.timing_table.vendor_required_count());
    hbm_sim::record_field(full_stats, "timing_table_dump", cli.timing_table_path.empty() ? "off" : cli.timing_table_path);
    hbm_sim::record_field(full_stats, "memory_system",
                cli.single_controller
                    ? "single_controller"
                    : (cli.stack_count > 1 ? "multi_stack_multi_controller"
                                           : "multi_controller"));
    hbm_sim::record_field(full_stats, "request_source", "streaming");
    hbm_sim::record_field(full_stats, "command_trace_retained",
                cli.controller.retain_command_trace);
    hbm_sim::record_field(full_stats, "scheduler", hbm_sim::to_string(cli.controller.scheduler));
    hbm_sim::record_field(full_stats, "row_policy", hbm_sim::to_string(cli.controller.row_policy));
    hbm_sim::record_field(full_stats, "row_policy_cap", cli.controller.row_policy_cap);
    hbm_sim::record_field(full_stats, "address_mapping", hbm_sim::to_string(spec.address_mapping));
    hbm_sim::record_field(full_stats, "channel_mapper", hbm_sim::to_string(cli.channel_mapper));
    hbm_sim::record_field(full_stats, "cmd_trace", cli.cmd_trace_path.empty() ? "off" : cli.cmd_trace_path);
    hbm_sim::record_field(full_stats, "response_trace",
                cli.response_trace_path.empty() ? "off" : cli.response_trace_path);
    hbm_sim::record_field(full_stats, "transaction_response_trace",
                cli.transaction_response_trace_path.empty()
                    ? "off"
                    : cli.transaction_response_trace_path);
    hbm_sim::record_field(full_stats, "response_delivery_mode",
                response_delivery_mode_name(cli.response_delivery_mode));
    hbm_sim::record_field(full_stats, "host_response_queue_capacity",
                cli.host_response_queue_capacity);
    hbm_sim::record_field(full_stats, "transaction_response_queue_capacity",
                cli.transaction_response_queue_capacity);
    hbm_sim::record_field(full_stats, "host_responses_consumed",
                consumed_host_responses);
    hbm_sim::record_field(full_stats, "host_responses_exported", exported_host_responses);
    hbm_sim::record_field(full_stats, "transaction_responses_consumed",
                consumed_transaction_responses);
    hbm_sim::record_field(full_stats, "transaction_responses_exported",
                exported_transaction_responses);
    hbm_sim::record_field(full_stats, "dfi_trace", cli.dfi_trace_path.empty() ? "off" : cli.dfi_trace_path);
    hbm_sim::record_field(full_stats, "dfi_signal_trace",
                cli.dfi_signal_trace_path.empty() ? "off" : cli.dfi_signal_trace_path);
    hbm_sim::record_field(full_stats, "memory_image", cli.memory_image_path.empty() ? "off" : cli.memory_image_path);
    hbm_sim::record_field(full_stats, "memory_image_dump",
                cli.dump_memory_image_path.empty() ? "off" : cli.dump_memory_image_path);
    hbm_sim::record_field(full_stats, "memory_csv_dump",
                cli.dump_memory_csv_path.empty() ? "off" : cli.dump_memory_csv_path);
    hbm_sim::record_field(full_stats, "mismatch_report", cli.mismatch_report_path.empty() ? "off" : cli.mismatch_report_path);
    hbm_sim::record_field(full_stats, "thermal_map_dump", cli.thermal_map_path.empty() ? "off" : cli.thermal_map_path);
    hbm_sim::record_field(full_stats, "memory_backend",
                hbm_sim::to_string(storage_options.memory_backend.kind));
    hbm_sim::record_field(full_stats, "memory_backend_line_bytes", memory_image->line_size());
    hbm_sim::record_field(full_stats, "memory_capacity_bytes",
                storage_options.memory_backend.capacity_bytes);
    hbm_sim::record_field(full_stats, "memory_data_file",
                storage_options.memory_backend.data_file.empty()
                    ? "off"
                    : storage_options.memory_backend.data_file);
    hbm_sim::record_field(full_stats, "memory_chunk_size",
                storage_options.memory_backend.chunk_size_bytes);
    hbm_sim::record_field(full_stats, "memory_chunk_cache_entries",
                storage_options.memory_backend.chunk_cache_entries);
    hbm_sim::record_field(full_stats, "sparse_density_warning_pct",
                storage_options.sparse_density_warning_pct);
    hbm_sim::record_field(full_stats, "topology_stats_scan_limit",
                storage_options.topology_stats_scan_limit);
    const bool sparse_density_warning =
        storage_options.memory_backend.kind == hbm_sim::MemoryBackendKind::Sparse &&
        stats.storage_density_pct >= storage_options.sparse_density_warning_pct;
    hbm_sim::record_field(full_stats, "storage_backend_recommendation",
                sparse_density_warning ? "switch_to_mmap_sparse_or_chunk_file" : "current_backend_ok");
    hbm_sim::record_field(full_stats, "floorplan_enabled", storage_options.floorplan_enabled);
    hbm_sim::record_field(full_stats, "power_model_enabled", storage_options.power_enabled);
    hbm_sim::record_field(full_stats, "thermal_model_enabled", storage_options.thermal_enabled);
    hbm_sim::record_field(full_stats, "thermal_model_kind", "behavioral_sparse_coupling");
    hbm_sim::record_field(full_stats, "power_source", storage_options.power_source);
    hbm_sim::record_field(full_stats, "power_scale", storage_options.power_scale);
    hbm_sim::record_field(full_stats, "thermal_ambient_C", storage_options.thermal_ambient_c);
    hbm_sim::record_field(full_stats, "thermal_cooling_per_cycle", storage_options.thermal_cooling_per_cycle);
    hbm_sim::record_field(full_stats, "thermal_rise_C_per_pJ", storage_options.thermal_rise_c_per_pj);
    hbm_sim::record_field(full_stats, "thermal_grid_cols_tile", storage_options.thermal_grid_cols_per_tile);
    hbm_sim::record_field(full_stats, "thermal_grid_rows_tile", storage_options.thermal_grid_rows_per_tile);
    hbm_sim::record_field(full_stats, "thermal_coupling", storage_options.thermal_coupling_enabled);
    hbm_sim::record_field(full_stats, "thermal_lateral_coupling", storage_options.thermal_lateral_coupling);
    hbm_sim::record_field(full_stats, "thermal_vertical_coupling", storage_options.thermal_vertical_coupling);
    hbm_sim::record_field(full_stats, "thermal_tsv_coupling_scale", storage_options.thermal_tsv_coupling_scale);
    hbm_sim::record_field(full_stats, "thermal_tsvs_per_grid", storage_options.thermal_tsvs_per_grid);
    hbm_sim::record_field(full_stats, "subarrays_per_bank", storage_options.subarrays_per_bank);
    hbm_sim::record_field(full_stats, "mats_per_subarray_x", storage_options.mats_per_subarray_x);
    hbm_sim::record_field(full_stats, "mats_per_subarray_y", storage_options.mats_per_subarray_y);
    hbm_sim::record_field(full_stats, "cells_per_mat_x", storage_options.cells_per_mat_x);
    hbm_sim::record_field(full_stats, "cells_per_mat_y", storage_options.cells_per_mat_y);
    hbm_sim::record_field(full_stats, "microbumps_x", storage_options.microbumps_x);
    hbm_sim::record_field(full_stats, "microbumps_y", storage_options.microbumps_y);
    hbm_sim::record_field(full_stats, "ecc_shadow", storage_options.ecc_shadow_enabled);
    hbm_sim::record_field(full_stats, "ecc_check_on_read", storage_options.ecc_check_on_read);
    hbm_sim::record_field(full_stats, "ecc_correct_single_bit", storage_options.ecc_correct_single_bit);
    hbm_sim::record_field(full_stats, "ecc_inject_period", storage_options.ecc_inject_period);
    hbm_sim::record_field(full_stats, "power_act_pJ", storage_options.act_energy_pj);
    hbm_sim::record_field(full_stats, "power_read_pJ", storage_options.read_energy_pj);
    hbm_sim::record_field(full_stats, "power_write_pJ", storage_options.write_energy_pj);
    hbm_sim::record_field(full_stats, "power_refab_pJ", storage_options.refab_energy_pj);
    hbm_sim::record_field(full_stats, "power_refpb_pJ", storage_options.refpb_energy_pj);
    hbm_sim::record_field(full_stats, "power_vdd", storage_options.idd_vdd);
    hbm_sim::record_field(full_stats, "idd_devices_per_rank", storage_options.idd_devices_per_rank);
    hbm_sim::record_field(full_stats, "cmd_validation", cli.validate_cmd_trace ? "pass" : "off");
    hbm_sim::record_field(full_stats, "cmd_validation_checked",
                cli.validate_cmd_trace ? validation_report.checked_commands : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_bus_checks",
                cli.validate_cmd_trace ? validation_report.bus_checks : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_edge_checks",
                cli.validate_cmd_trace ? validation_report.edge_checks : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_pair_checks",
                cli.validate_cmd_trace ? validation_report.edge_pairing_checks : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_state_checks",
                cli.validate_cmd_trace ? validation_report.state_checks : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_timing_checks",
                cli.validate_cmd_trace ? validation_report.timing_constraint_checks : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_timing_updates",
                cli.validate_cmd_trace ? validation_report.timing_constraint_updates : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_faw_events",
                cli.validate_cmd_trace ? validation_report.faw_events_checked : 0);
    hbm_sim::record_field(full_stats, "cmd_validation_wck_checks",
                cli.validate_cmd_trace ? validation_report.wck_window_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation", cli.validate_dfi_trace ? "pass" : "off");
    hbm_sim::record_field(full_stats, "dfi_validation_events",
                cli.validate_dfi_trace ? dfi_validation_report.checked_events : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_command_checks",
                cli.validate_dfi_trace ? dfi_validation_report.command_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_data_checks",
                cli.validate_dfi_trace ? dfi_validation_report.data_beat_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_latency_checks",
                cli.validate_dfi_trace ? dfi_validation_report.latency_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_phase_checks",
                cli.validate_dfi_trace ? dfi_validation_report.phase_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_signal_checks",
                cli.validate_dfi_trace ? dfi_validation_report.signal_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_payload_checks",
                cli.validate_dfi_trace ? dfi_validation_report.payload_checks : 0);
    hbm_sim::record_field(full_stats, "dfi_validation_expected_checks",
                cli.validate_dfi_trace
                    ? dfi_validation_report.expected_payload_checks
                    : 0);
    hbm_sim::record_field(full_stats, "pattern", cli.pattern);
    hbm_sim::record_field(full_stats, "requests", cli.requests);
    hbm_sim::record_field(full_stats, "read_ratio", cli.read_ratio);
    hbm_sim::record_field(full_stats, "seed", cli.seed);
    hbm_sim::record_field(full_stats, "random_address_space_bytes",
                cli.random_address_space_bytes);
    hbm_sim::record_field(full_stats, "addr_stride", cli.addr_stride);
    hbm_sim::record_field(full_stats, "max_cycles", cli.max_cycles);
    hbm_sim::record_field(full_stats, "progress_interval", cli.progress_interval);
    hbm_sim::record_field(full_stats, "stats_view", cli.stats_view);
    hbm_sim::record_field(full_stats, "inject_interval", cli.inject_interval);
    hbm_sim::record_field(full_stats, "init_sequence", cli.init_sequence);
    hbm_sim::record_field(full_stats, "init_sequence_interval", cli.init_sequence_interval);
    hbm_sim::record_field(full_stats, "read_buffer_size", cli.controller.read_buffer_size);
    hbm_sim::record_field(full_stats, "write_buffer_size", cli.controller.write_buffer_size);
    hbm_sim::record_field(full_stats, "dram_transaction_bytes", spec.transaction_bytes());
    hbm_sim::record_field(full_stats, "banks", spec.total_banks());
    for (std::size_t stack = 0; stack < per_stack_stats.size(); stack++) {
      const auto& per = per_stack_stats[stack];
      const std::string prefix = "stack_" + std::to_string(stack) + "_";
      hbm_sim::record_field(full_stats, (prefix + "active_ctrls").c_str(), per.active_controllers);
      hbm_sim::record_field(full_stats, (prefix + "reads").c_str(), per.completed_reads);
      hbm_sim::record_field(full_stats, (prefix + "writes").c_str(), per.completed_writes);
      hbm_sim::record_field(full_stats, (prefix + "bw_GBps").c_str(), per.achieved_bandwidth_GBps);
      const double avg_latency = per.completed_reads == 0 ? 0.0 :
          static_cast<double>(per.total_read_latency) / per.completed_reads;
      hbm_sim::record_field(full_stats, (prefix + "avg_read_latency").c_str(), avg_latency);
      hbm_sim::record_field(full_stats, (prefix + "ingress_stalls").c_str(), per.stack_ingress_stall_cycles);
      hbm_sim::record_field(full_stats, (prefix + "qos_dispatches").c_str(), per.qos_priority_dispatches);
      hbm_sim::record_field(full_stats, (prefix + "power_pJ").c_str(), per.power_energy_pj);
      hbm_sim::record_field(full_stats, (prefix + "peak_temp_C").c_str(), per.thermal_peak_temp_c);
    }
    full_stats.merge(hbm_sim::collect_stats(stats));
    hbm_sim::record_field(full_stats, "capacity_per_instance_bytes", spec.addressable_capacity_bytes());
    hbm_sim::record_field(full_stats, "input_kind", cli.trace_path.empty() ? "synthetic" : "trace");
    hbm_sim::record_field(full_stats, "effective_random_address_space_bytes",
        cli.random_address_space_bytes == 0
          ? spec.addressable_capacity_bytes() * static_cast<std::uint64_t>(cli.stack_count)
          : cli.random_address_space_bytes);
    report = hbm_sim::make_result_report(full_stats);
    report.validation["run_status"] = run_status;
    Cli baseline_cli;
    baseline_cli.standard = cli.standard;
    const auto baseline_spec = hbm_sim::config::build_model(cli.standard, {}, 3);
    std::ostringstream baseline_config, effective_config;
    serialize_resolved_config(baseline_config, baseline_cli, baseline_spec);
    serialize_resolved_config(effective_config, cli, spec);
    report.baseline = "built-in " + spec.name + " (schema 3)";
    report.changes = hbm_sim::compare_resolved_parameters(
        baseline_config.str(), effective_config.str());
    hbm_sim::write_result_json(result_path, report, run_status);
    if (cli.stats_view == "diagnostic")
      hbm_sim::print_diagnostics(std::cout, full_stats);
    else
      hbm_sim::print_result(std::cout, report);

    if (stats.hit_cycle_limit) {
      return 2;
    }
    if (cli.fail_on_data_mismatch && stats.data_mismatches > 0) {
      return 3;
    }
    return 0;
  } catch (const std::exception& e) {
    try {
      // Keep partial verification evidence without inventing missing counters.
      if (report.validation.empty()) report = hbm_sim::make_result_report(full_stats);
      hbm_sim::write_result_json(result_path, report, "failed", e.what());
    } catch (const std::exception& report_error) {
      std::cerr << "result error: " << report_error.what() << '\n';
    }
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
