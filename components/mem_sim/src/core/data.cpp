#include "hbm_sim/core/data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "hbm_sim/dram/semantics.hpp"
#include "hbm_sim/stats/stats.hpp"

namespace hbm_sim {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint64_t checked_multiply_u64(std::uint64_t lhs, std::uint64_t rhs,
                                   const char *context) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(context) + " exceeds uint64 range");
  }
  return lhs * rhs;
}

void validate_address_span(Address address, std::size_t size,
                           const char *context,
                           std::uint64_t capacity_bytes = 0) {
  if (size == 0) {
    return;
  }
  const std::uintmax_t last_offset =
      static_cast<std::uintmax_t>(size - 1);
  const std::uintmax_t available = static_cast<std::uintmax_t>(
      std::numeric_limits<Address>::max() - address);
  if (last_offset > available) {
    throw std::overflow_error(std::string(context) +
                              " address range exceeds Address space");
  }
  if (capacity_bytes != 0 &&
      (address >= capacity_bytes ||
       last_offset >= static_cast<std::uintmax_t>(capacity_bytes - address))) {
    throw std::out_of_range(std::string(context) +
                            " address range exceeds configured capacity");
  }
}

int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + c - 'a';
  if (c >= 'A' && c <= 'F')
    return 10 + c - 'A';
  return -1;
}

std::string compact_hex(std::string text) {
  if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
    text.erase(0, 2);
  }
  text.erase(std::remove_if(text.begin(), text.end(),
                            [](unsigned char c) {
                              return c == '_' || c == ':' || std::isspace(c);
                            }),
             text.end());
  return text;
}

std::uint64_t key_of(std::initializer_list<int> values) {
  std::uint64_t h = 1469598103934665603ull;
  for (int value : values) {
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
    h *= 1099511628211ull;
  }
  return h;
}

std::uint32_t checksum_bytes(const ByteVector &bytes) {
  std::uint32_t h = 2166136261u;
  for (std::uint8_t byte : bytes) {
    h ^= byte;
    h *= 16777619u;
  }
  return h;
}

bool all_masked(const ByteVector &mask) {
  return std::all_of(mask.begin(), mask.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool is_power_of_two_u64(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

bool byte_bit(const ByteVector &bytes, std::size_t bit) {
  std::size_t byte = bit / 8;
  std::size_t shift = bit % 8;
  return byte < bytes.size() && ((bytes[byte] >> shift) & 0x1u) != 0;
}

void flip_byte_bit(ByteVector &bytes, std::size_t bit) {
  std::size_t byte = bit / 8;
  std::size_t shift = bit % 8;
  if (byte < bytes.size()) {
    bytes[byte] ^= static_cast<std::uint8_t>(1u << shift);
  }
}

int parity_bits_for_data_bits(std::size_t data_bits) {
  int parity_bits = 1;
  while ((std::uint64_t{1} << parity_bits) <
         data_bits + static_cast<std::size_t>(parity_bits) + 1) {
    parity_bits++;
  }
  return parity_bits;
}

std::uint64_t code_position_for_data_bit(std::size_t data_bit) {
  std::size_t data_seen = 0;
  std::uint64_t code_pos = 1;
  while (true) {
    if (!is_power_of_two_u64(code_pos)) {
      if (data_seen == data_bit) {
        return code_pos;
      }
      data_seen++;
    }
    code_pos++;
  }
}

std::optional<std::size_t>
data_bit_for_code_position(std::uint64_t code_position, std::size_t data_bits) {
  std::size_t data_seen = 0;
  for (std::uint64_t code_pos = 1; data_seen < data_bits; code_pos++) {
    if (is_power_of_two_u64(code_pos)) {
      continue;
    }
    if (code_pos == code_position) {
      return data_seen;
    }
    data_seen++;
  }
  return std::nullopt;
}

struct SecdedShadow {
  std::uint64_t hamming = 0;
  bool overall = false;
  int parity_bits = 0;
};

SecdedShadow compute_secded_shadow(const ByteVector &bytes) {
  SecdedShadow shadow;
  const std::size_t data_bits = bytes.size() * 8;
  shadow.parity_bits = parity_bits_for_data_bits(data_bits);
  bool overall = false;
  for (std::size_t bit = 0; bit < data_bits; bit++) {
    if (!byte_bit(bytes, bit)) {
      continue;
    }
    overall = !overall;
    std::uint64_t code_pos = code_position_for_data_bit(bit);
    for (int parity = 0; parity < shadow.parity_bits; parity++) {
      if ((code_pos & (std::uint64_t{1} << parity)) != 0) {
        shadow.hamming ^= (std::uint64_t{1} << parity);
      }
    }
  }
  for (int parity = 0; parity < shadow.parity_bits; parity++) {
    if ((shadow.hamming & (std::uint64_t{1} << parity)) != 0) {
      overall = !overall;
    }
  }
  shadow.overall = overall;
  return shadow;
}

int ceil_sqrt_int(int value) {
  value = std::max(1, value);
  int root = 1;
  while (root * root < value) {
    root++;
  }
  return root;
}

std::string lower_token(std::string token) {
  std::transform(token.begin(), token.end(), token.begin(),
                 [](unsigned char c) {
                   if (c == '-')
                     return '_';
                   return static_cast<char>(std::tolower(c));
                 });
  return token;
}

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::vector<std::string> split_tokens(const std::string &line) {
  std::istringstream iss(line);
  std::vector<std::string> tokens;
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

Address parse_address_token(const std::string &token) {
  if (token.empty() || token.front() == '-') {
    throw std::invalid_argument("invalid memory image address: " + token);
  }
  std::size_t pos = 0;
  Address value = std::stoull(token, &pos, 0);
  if (pos != token.size()) {
    throw std::invalid_argument("invalid memory image address: " + token);
  }
  return value;
}

std::string format_u32_hex(std::uint32_t value) {
  std::ostringstream os;
  os << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
  return os.str();
}

StorageKey key_from_physical(const PhysicalAddress &physical) {
  return StorageKey{
      physical.stack, physical.channel, physical.pseudo_channel,
      physical.sid,   physical.rank,    physical.bank_group,
      physical.bank,  physical.row,     physical.column,
  };
}

DecodedAddress decoded_from_storage_key(const StorageKey &key) {
  DecodedAddress decoded;
  decoded.channel = key.channel;
  decoded.pseudo_channel = key.pseudo_channel;
  decoded.sid = key.sid;
  decoded.rank = key.rank;
  decoded.bank_group = key.bank_group;
  decoded.bank = key.bank;
  decoded.row = key.row;
  decoded.column = key.column;
  return decoded;
}

BackendLine backend_line_from_block(const DataBlock &block) {
  BackendLine line;
  line.bytes = block.bytes;
  line.initialized_mask = block.initialized_mask;
  line.initialized = block.initialized;
  line.version = block.version;
  line.last_writer_request_id = block.last_writer_request_id;
  line.last_write_cycle = block.last_write_cycle;
  line.last_access_cycle = block.last_access_cycle;
  line.checksum = block.checksum;
  line.ecc_hamming = block.ecc_hamming;
  line.ecc_overall = block.ecc_overall;
  line.ecc_parity_bits = block.ecc_parity_bits;
  line.ecc_valid = block.ecc_valid;
  line.ecc_uncorrectable = block.ecc_uncorrectable;
  line.ecc_error_injections = block.ecc_error_injections;
  line.storage_key = block.storage_key;
  return line;
}

StorageKey bank_key_from_storage_key(StorageKey key) {
  key.row = -1;
  key.column = -1;
  return key;
}

StorageKey row_key_from_storage_key(StorageKey key) {
  key.column = -1;
  return key;
}

double command_energy_pj(const StorageModelOptions &options, Command command,
                         std::size_t payload_bytes) {
  switch (command) {
  case Command::ACT:
    return options.act_energy_pj;
  case Command::ACT1:
    return options.act1_energy_pj;
  case Command::ACT2:
    return options.act2_energy_pj;
  case Command::PRE:
  case Command::PREPB:
    return options.pre_energy_pj;
  case Command::PREAB:
    return options.preab_energy_pj;
  case Command::CASRD:
  case Command::CASWR:
    return options.cas_energy_pj;
  case Command::RD:
  case Command::RDA:
    return options.read_energy_pj +
           options.read_energy_per_byte_pj * static_cast<double>(payload_bytes);
  case Command::WR:
  case Command::WRA:
    return options.write_energy_pj + options.write_energy_per_byte_pj *
                                         static_cast<double>(payload_bytes);
  case Command::REFPB:
    return options.refpb_energy_pj;
  case Command::REFDB:
    return options.refdb_energy_pj;
  case Command::REFAB:
    return options.refab_energy_pj;
  case Command::RFMPB:
    return options.rfmpb_energy_pj;
  case Command::RFMAB:
    return options.rfmab_energy_pj;
  case Command::MRW:
  case Command::MRR:
  case Command::WCKSYNC:
  case Command::WCKTRAIN:
  case Command::DVFS:
  case Command::PDE:
  case Command::PDX:
  case Command::SREFEN:
  case Command::SREFEX:
  case Command::ECCSCRUB:
  case Command::RASERR:
    return options.control_energy_pj;
  case Command::NOP:
    return 0.0;
  }
  return 0.0;
}

bool uses_dramsim3_idd_power(const StorageModelOptions &options) {
  std::string source = lower_token(options.power_source);
  return source == "idd" || source == "dramsim3_idd" || source == "dramsim3";
}

double nck_to_ns(int nck, double tck_ps) {
  return static_cast<double>(std::max(0, nck)) * tck_ps / 1000.0;
}

double positive_current_delta(double active_ma, double background_ma) {
  return std::max(0.0, active_ma - background_ma);
}

StorageModelOptions calibrate_power_from_idd(const DramSpec &spec,
                                             StorageModelOptions options) {
  if (!uses_dramsim3_idd_power(options)) {
    return options;
  }

  // DRAMsim3 使用 VDD * current_delta(mA) * time(ns) * devices_per_rank
  // 推导命令能量；1 V * 1 mA * 1 ns = 1 pJ。
  const double tck_ps = spec.timing.tCK_ps > 0.0 ? spec.timing.tCK_ps : 1000.0;
  const double devices =
      options.idd_devices_per_rank > 0.0 ? options.idd_devices_per_rank : 1.0;
  const double burst_cycles =
      options.idd_burst_cycles > 0.0
          ? options.idd_burst_cycles
          : static_cast<double>(std::max(1, spec.timing.nBL));
  const double burst_ns = burst_cycles * tck_ps / 1000.0;
  const double tras_ns = nck_to_ns(spec.timing.nRAS, tck_ps);
  const double trp_ns = nck_to_ns(spec.timing.nRP, tck_ps);
  const double trc_ns = nck_to_ns(spec.timing.nRC, tck_ps);
  const double trfc_ns = nck_to_ns(spec.timing.nRFC, tck_ps);
  const double trfcpb_ns = nck_to_ns(spec.timing.nRFCpb, tck_ps);

  options.act_energy_pj = std::max(
      0.0, options.idd_vdd *
               (options.idd0_ma * trc_ns -
                (options.idd3n_ma * tras_ns + options.idd2n_ma * trp_ns)) *
               devices);
  options.act1_energy_pj = options.act_energy_pj * 0.5;
  options.act2_energy_pj = options.act_energy_pj * 0.5;
  options.read_energy_pj =
      options.idd_vdd *
      positive_current_delta(options.idd4r_ma, options.idd3n_ma) * burst_ns *
      devices;
  options.write_energy_pj =
      options.idd_vdd *
      positive_current_delta(options.idd4w_ma, options.idd3n_ma) * burst_ns *
      devices;
  options.read_energy_per_byte_pj = 0.0;
  options.write_energy_per_byte_pj = 0.0;
  options.refab_energy_pj =
      options.idd_vdd *
      positive_current_delta(options.idd5ab_ma, options.idd3n_ma) * trfc_ns *
      devices;
  options.refpb_energy_pj =
      options.idd_vdd *
      positive_current_delta(options.idd5pb_ma, options.idd3n_ma) * trfcpb_ns *
      devices;
  options.refdb_energy_pj = options.refpb_energy_pj;
  // HBM4 RFM/PRAC 能量取决于厂商和 RAS 策略。没有公开 IDD 风格 RFM 表时，
  // 保守复用 REF 恢复能量，并在 CLI 和文档中保留来源。
  options.rfmab_energy_pj = options.refab_energy_pj;
  options.rfmpb_energy_pj = options.refpb_energy_pj;
  return options;
}

StorageModelOptions validate_and_calibrate_storage_options(
    const DramSpec &spec, StorageModelOptions options) {
  if (options.stack_id < 0) {
    throw std::invalid_argument("storage stack_id must be non-negative");
  }
  if (options.sparse_density_warning_pct < 0.0 ||
      options.sparse_density_warning_pct > 100.0 ||
      !std::isfinite(options.sparse_density_warning_pct)) {
    throw std::invalid_argument(
        "sparse_density_warning_pct must be finite and in [0, 100]");
  }
  const std::initializer_list<std::pair<const char *, int>> positive_ints = {
      {"thermal_grid_cols_per_tile", options.thermal_grid_cols_per_tile},
      {"thermal_grid_rows_per_tile", options.thermal_grid_rows_per_tile},
      {"thermal_tsvs_per_grid", options.thermal_tsvs_per_grid},
      {"subarrays_per_bank", options.subarrays_per_bank},
      {"mats_per_subarray_x", options.mats_per_subarray_x},
      {"mats_per_subarray_y", options.mats_per_subarray_y},
      {"cells_per_mat_x", options.cells_per_mat_x},
      {"cells_per_mat_y", options.cells_per_mat_y},
      {"microbumps_x", options.microbumps_x},
      {"microbumps_y", options.microbumps_y},
  };
  for (const auto &[name, value] : positive_ints) {
    if (value <= 0) {
      throw std::invalid_argument(std::string(name) + " must be > 0");
    }
  }
  if (options.ecc_inject_period < 0) {
    throw std::invalid_argument("ecc_inject_period must be >= 0");
  }
  const std::initializer_list<std::pair<const char *, double>> non_negative = {
      {"power_scale", options.power_scale},
      {"thermal_cooling_per_cycle", options.thermal_cooling_per_cycle},
      {"thermal_rise_c_per_pj", options.thermal_rise_c_per_pj},
      {"thermal_lateral_coupling", options.thermal_lateral_coupling},
      {"thermal_vertical_coupling", options.thermal_vertical_coupling},
      {"thermal_tsv_coupling_scale", options.thermal_tsv_coupling_scale},
      {"thermal_chip_dim_x_m", options.thermal_chip_dim_x_m},
      {"thermal_chip_dim_y_m", options.thermal_chip_dim_y_m},
      {"thermal_tsv_radius_m", options.thermal_tsv_radius_m},
      {"thermal_k_silicon", options.thermal_k_silicon},
      {"thermal_k_copper", options.thermal_k_copper},
      {"idd_vdd", options.idd_vdd},
      {"idd0_ma", options.idd0_ma},
      {"idd2n_ma", options.idd2n_ma},
      {"idd3n_ma", options.idd3n_ma},
      {"idd4r_ma", options.idd4r_ma},
      {"idd4w_ma", options.idd4w_ma},
      {"idd5ab_ma", options.idd5ab_ma},
      {"idd5pb_ma", options.idd5pb_ma},
      {"idd6x_ma", options.idd6x_ma},
      {"idd_devices_per_rank", options.idd_devices_per_rank},
      {"idd_burst_cycles", options.idd_burst_cycles},
      {"act_energy_pj", options.act_energy_pj},
      {"act1_energy_pj", options.act1_energy_pj},
      {"act2_energy_pj", options.act2_energy_pj},
      {"pre_energy_pj", options.pre_energy_pj},
      {"preab_energy_pj", options.preab_energy_pj},
      {"cas_energy_pj", options.cas_energy_pj},
      {"read_energy_pj", options.read_energy_pj},
      {"read_energy_per_byte_pj", options.read_energy_per_byte_pj},
      {"write_energy_pj", options.write_energy_pj},
      {"write_energy_per_byte_pj", options.write_energy_per_byte_pj},
      {"refpb_energy_pj", options.refpb_energy_pj},
      {"refdb_energy_pj", options.refdb_energy_pj},
      {"refab_energy_pj", options.refab_energy_pj},
      {"rfmpb_energy_pj", options.rfmpb_energy_pj},
      {"rfmab_energy_pj", options.rfmab_energy_pj},
      {"control_energy_pj", options.control_energy_pj},
  };
  for (const auto &[name, value] : non_negative) {
    if (!std::isfinite(value) || value < 0.0) {
      throw std::invalid_argument(std::string(name) +
                                  " must be finite and >= 0");
    }
  }
  if (!std::isfinite(options.thermal_ambient_c)) {
    throw std::invalid_argument("thermal_ambient_c must be finite");
  }
  const std::string source = lower_token(options.power_source);
  if (source != "configured_pj" && source != "manual" && source != "idd" &&
      source != "dramsim3_idd" && source != "dramsim3") {
    throw std::invalid_argument(
        "power_source must be configured_pj or dramsim3_idd");
  }
  if (uses_dramsim3_idd_power(options) &&
      options.idd_devices_per_rank <= 0.0) {
    throw std::invalid_argument(
        "idd_devices_per_rank must be > 0 for IDD power mode");
  }
  return calibrate_power_from_idd(spec, std::move(options));
}

std::size_t validated_storage_line_size(const DramSpec &spec) {
  validate_spec(spec);
  (void)spec.total_addressable_transactions();
  return static_cast<std::size_t>(spec.transaction_bytes());
}

DataBlockMetadata metadata_from_block(const DataBlock &block) {
  return DataBlockMetadata{
      block.initialized,
      block.version,
      block.last_writer_request_id,
      block.last_write_cycle,
      block.checksum,
      block.ecc_hamming,
      block.ecc_overall,
      block.ecc_parity_bits,
      block.ecc_valid,
      block.ecc_uncorrectable,
      block.physical,
      block.storage_key,
  };
}

void set_decoded_key(DecodedAddress &decoded, const std::string &key,
                     const std::string &value) {
  if (value.empty() || value.front() == '-') {
    throw std::invalid_argument("invalid non-negative memory coordinate: " +
                                value);
  }
  std::size_t consumed = 0;
  int parsed = std::stoi(value, &consumed, 10);
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid non-negative memory coordinate: " +
                                value);
  }
  if (key == "ch" || key == "channel")
    decoded.channel = parsed;
  else if (key == "pc" || key == "pseudo_channel" || key == "pseudo")
    decoded.pseudo_channel = parsed;
  else if (key == "sid")
    decoded.sid = parsed;
  else if (key == "rank")
    decoded.rank = parsed;
  else if (key == "bg" || key == "bank_group")
    decoded.bank_group = parsed;
  else if (key == "bank")
    decoded.bank = parsed;
  else if (key == "row")
    decoded.row = parsed;
  else if (key == "col" || key == "column")
    decoded.column = parsed;
}

bool is_decoded_key(const std::string &key) {
  return key == "ch" || key == "channel" || key == "pc" ||
         key == "pseudo_channel" || key == "pseudo" || key == "sid" ||
         key == "rank" || key == "bg" || key == "bank_group" || key == "bank" ||
         key == "row" || key == "col" || key == "column";
}

} // namespace

void apply_physical_storage_stats(Stats &stats,
                                  const PhysicalStorageStats &storage) {
  stats.storage_lines_allocated = storage.lines_allocated;
  stats.unique_written_lines = storage.unique_written_lines;
  stats.storage_bytes_allocated = storage.bytes_allocated;
  stats.storage_topology_lines_scanned = storage.topology_lines_scanned;
  stats.storage_topology_scan_skipped = storage.topology_scan_skipped;
  stats.storage_stacks_touched = storage.stacks_touched;
  stats.storage_dies_touched = storage.dies_touched;
  stats.storage_layers_touched = storage.layers_touched;
  stats.storage_channels_touched = storage.channels_touched;
  stats.storage_pseudo_channels_touched = storage.pseudo_channels_touched;
  stats.storage_sids_touched = storage.sids_touched;
  stats.storage_ranks_touched = storage.ranks_touched;
  stats.storage_bank_groups_touched = storage.bank_groups_touched;
  stats.storage_banks_touched = storage.banks_touched;
  stats.storage_rows_touched = storage.rows_touched;
  stats.storage_columns_touched = storage.columns_touched;
  stats.storage_subarrays_touched = storage.subarrays_touched;
  stats.storage_mats_touched = storage.mats_touched;
  stats.storage_cells_touched = storage.cells_touched;
  stats.storage_microbumps_touched = storage.microbumps_touched;
  stats.floorplan_tiles_touched = storage.floorplan_tiles_touched;
  stats.thermal_tiles_touched = storage.thermal_tiles_touched;
  stats.thermal_grid_cells_touched = storage.thermal_grid_cells_touched;
  stats.storage_read_line_accesses = storage.read_line_accesses;
  stats.storage_write_line_accesses = storage.write_line_accesses;
  stats.rowbuf_activations = storage.row_buffer_activations;
  stats.rowbuf_precharges = storage.row_buffer_precharges;
  stats.rowbuf_dirty_writebacks = storage.row_buffer_dirty_writebacks;
  stats.rowbuf_clean_precharges = storage.row_buffer_clean_precharges;
  stats.rowbuf_hits = storage.row_buffer_hits;
  stats.rowbuf_misses = storage.row_buffer_misses;
  stats.rowbuf_lazy_loads = storage.row_buffer_lazy_loads;
  stats.rowbuf_reads = storage.row_buffer_reads;
  stats.rowbuf_writes = storage.row_buffer_writes;
  stats.rowbuf_forced_closes = storage.row_buffer_forced_closes;
  stats.rowbuf_open_rows = storage.row_buffer_open_rows;
  stats.rowbuf_dirty_rows = storage.row_buffer_dirty_rows;
  stats.power_events = storage.power_events;
  stats.thermal_updates = storage.thermal_updates;
  stats.power_energy_pj = storage.power_energy_pj;
  stats.power_act_energy_pj = storage.power_act_energy_pj;
  stats.power_pre_energy_pj = storage.power_pre_energy_pj;
  stats.power_read_energy_pj = storage.power_read_energy_pj;
  stats.power_write_energy_pj = storage.power_write_energy_pj;
  stats.power_refresh_energy_pj = storage.power_refresh_energy_pj;
  stats.power_rfm_energy_pj = storage.power_rfm_energy_pj;
  stats.power_control_energy_pj = storage.power_control_energy_pj;
  stats.thermal_peak_temp_c = storage.thermal_peak_temp_c;
  stats.thermal_avg_temp_c = storage.thermal_avg_temp_c;
  stats.thermal_hotspot_layer = storage.thermal_hotspot_layer;
  stats.thermal_hotspot_x = storage.thermal_hotspot_x;
  stats.thermal_hotspot_y = storage.thermal_hotspot_y;
  stats.thermal_lateral_transfers = storage.thermal_lateral_transfers;
  stats.thermal_vertical_transfers = storage.thermal_vertical_transfers;
  stats.thermal_tsv_transfers = storage.thermal_tsv_transfers;
  stats.thermal_coupled_delta_c = storage.thermal_coupled_delta_c;
  stats.ecc_shadow_updates = storage.ecc_shadow_updates;
  stats.ecc_checked_reads = storage.ecc_checked_reads;
  stats.ecc_corrected_errors = storage.ecc_corrected_errors;
  stats.ecc_uncorrectable_errors = storage.ecc_uncorrectable_errors;
  stats.ecc_injected_errors = storage.ecc_injected_errors;
  stats.ecc_parity_repairs = storage.ecc_parity_repairs;
}

std::size_t StorageKeyHash::operator()(const StorageKey &key) const {
  return static_cast<std::size_t>(
      key_of({key.stack, key.channel, key.pseudo_channel, key.sid, key.rank,
              key.bank_group, key.bank, key.row, key.column}));
}

std::size_t FloorplanKeyHash::operator()(const FloorplanKey &key) const {
  return static_cast<std::size_t>(
      key_of({key.stack, key.layer, key.tile_x, key.tile_y}));
}

std::size_t ThermalGridKeyHash::operator()(const ThermalGridKey &key) const {
  return static_cast<std::size_t>(key_of({key.stack, key.layer, key.x, key.y}));
}

std::size_t MemoryImage::allocated_bytes() const {
  const std::uint64_t bytes = checked_multiply_u64(
      backend_->allocated_lines(), static_cast<std::uint64_t>(line_size_),
      "allocated storage bytes");
  if (bytes >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::overflow_error("allocated storage bytes exceeds size_t range");
  }
  return static_cast<std::size_t>(bytes);
}

DataCheckResult
DataValidator::check_read(Cycle cycle, std::uint64_t request_id,
                          Address address, const PhysicalAddress &physical,
                          const ByteVector &expected, const ByteVector &actual,
                          bool initialized, bool forwarded,
                          std::optional<DataBlockMetadata> block) {
  DataCheckResult result;
  result.initialized = initialized;
  result.actual = actual;
  result.matched = expected == actual;
  if (result.matched) {
    return result;
  }

  std::size_t mismatch_offset = 0;
  while (mismatch_offset < expected.size() && mismatch_offset < actual.size() &&
         expected[mismatch_offset] == actual[mismatch_offset]) {
    mismatch_offset++;
  }
  std::ostringstream msg;
  msg << "data mismatch at " << format_address(address)
      << " request=" << request_id << " offset=" << mismatch_offset
      << " expected=" << bytes_to_hex(expected)
      << " actual=" << bytes_to_hex(actual);
  result.message = msg.str();

  mismatches_.push_back(DataMismatchRecord{
      cycle,
      request_id,
      address,
      initialized,
      forwarded,
      expected,
      actual,
      std::move(block),
      physical,
  });
  return result;
}

void DataValidator::dump_text(const std::string &path) const {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open mismatch report output: " + path);
  }
  out << "# cycle request_id address initialized forwarded stack ch pc sid "
         "rank bg bank row col layer tile_x tile_y "
      << "version last_writer last_write_cycle checksum ecc_valid ecc_hamming "
         "ecc_overall ecc_uncorrectable "
      << "expected actual\n";
  for (const auto &mismatch : mismatches_) {
    const PhysicalAddress &p = mismatch.physical;
    out << mismatch.cycle << ' ' << mismatch.request_id << ' '
        << format_address(mismatch.address) << ' '
        << (mismatch.initialized ? "true" : "false") << ' '
        << (mismatch.forwarded ? "true" : "false") << ' ' << p.stack << ' '
        << p.channel << ' ' << p.pseudo_channel << ' ' << p.sid << ' '
        << (mismatch.block ? mismatch.block->storage_key.rank : p.rank) << ' '
        << p.bank_group << ' ' << p.bank << ' ' << p.row << ' ' << p.column
        << ' ' << p.layer << ' ' << p.tile_x << ' ' << p.tile_y << ' ';
    if (mismatch.block) {
      out << mismatch.block->version << ' '
          << mismatch.block->last_writer_request_id << ' '
          << mismatch.block->last_write_cycle << ' '
          << format_u32_hex(mismatch.block->checksum) << ' '
          << (mismatch.block->ecc_valid ? "true" : "false") << ' ' << "0x"
          << std::hex << mismatch.block->ecc_hamming << std::dec << ' '
          << (mismatch.block->ecc_overall ? "true" : "false") << ' '
          << (mismatch.block->ecc_uncorrectable ? "true" : "false");
    } else {
      out << "NA NA NA NA NA NA NA NA";
    }
    out << ' ' << bytes_to_hex(mismatch.expected) << ' '
        << bytes_to_hex(mismatch.actual) << '\n';
  }
}

MemoryImage::MemoryImage(std::size_t line_size, std::uint8_t default_value)
    : line_size_(line_size),
      default_value_(default_value),
      backend_(make_memory_backend(line_size_, options_.memory_backend)) {
  thermal_peak_temp_c_ = options_.thermal_ambient_c;
}

MemoryImage::MemoryImage(DramSpec spec, std::uint8_t default_value,
                         StorageModelOptions options)
    : line_size_(validated_storage_line_size(spec)),
      default_value_(default_value), spec_(std::move(spec)),
      options_(validate_and_calibrate_storage_options(*spec_,
                                                      std::move(options))) {
  if (options_.memory_backend.capacity_bytes == 0) {
    options_.memory_backend.capacity_bytes =
        spec_->addressable_capacity_bytes();
  }
  backend_ = make_memory_backend(line_size_, options_.memory_backend);
  thermal_peak_temp_c_ = options_.thermal_ambient_c;
}

MemoryImage::~MemoryImage() {
  try {
    // 析构函数不能抛异常；正常 Controller/MemorySystem 完成路径会显式调用
    // flush_backend() 并传播错误。这里仅处理调用方在异常展开中遗漏收尾的情况。
    flush_backend();
  } catch (...) {
  }
}

void MemoryImage::flush_backend() {
  if (backend_) {
    backend_->flush();
  }
}

Address MemoryImage::line_base(Address address) const {
  return address - (address % static_cast<Address>(line_size_));
}

std::size_t MemoryImage::line_offset(Address address) const {
  return static_cast<std::size_t>(address % static_cast<Address>(line_size_));
}

void MemoryImage::validate_decoded_address(
    const DecodedAddress &decoded) const {
  if (!spec_.has_value()) {
    return;
  }
  const Organization &o = spec_->org;
  const auto require_coordinate = [](int value, int count,
                                     const char *name) {
    if (value < 0 || value >= count) {
      throw std::out_of_range(std::string("decoded ") + name +
                              " is outside the DRAM organization");
    }
  };
  require_coordinate(decoded.channel, o.channels, "channel");
  require_coordinate(decoded.pseudo_channel, o.pseudo_channels,
                     "pseudo_channel");
  require_coordinate(decoded.sid, o.sids, "sid");
  require_coordinate(decoded.rank, o.ranks, "rank");
  require_coordinate(decoded.bank_group, o.bank_groups, "bank_group");
  require_coordinate(decoded.bank, o.banks_per_group, "bank");
  require_coordinate(decoded.row, o.rows, "row");
  require_coordinate(decoded.column, o.columns, "column");
}

PhysicalAddress
MemoryImage::physical_address(Address address,
                              const DecodedAddress *decoded) const {
  Address base = line_base(address);
  PhysicalAddress physical;
  physical.logical_line_base = base;
  physical.byte_offset = line_offset(address);

  DecodedAddress d;
  if (decoded != nullptr) {
    d = *decoded;
    validate_decoded_address(d);
  } else if (spec_.has_value()) {
    d = AddressMapper(*spec_).decode(base);
  } else {
    std::uint64_t line = base / static_cast<Address>(line_size_);
    d.row = static_cast<int>(line & 0x7fffffffull);
    d.column = 0;
  }

  physical.channel = d.channel;
  physical.pseudo_channel = d.pseudo_channel;
  physical.sid = d.sid;
  physical.rank = d.rank;
  physical.bank_group = d.bank_group;
  physical.bank = d.bank;
  physical.row = d.row;
  physical.column = d.column;

  const int stack_height =
      spec_.has_value() ? std::max(1, spec_->stack_height) : 1;
  const int sid_count = spec_.has_value() ? std::max(1, spec_->org.sids) : 1;
  const int banks_per_group =
      spec_.has_value() ? std::max(1, spec_->org.banks_per_group) : 1;
  const int bank_index =
      std::max(0, d.bank_group) * banks_per_group + std::max(0, d.bank);
  const int layers_per_sid =
      std::max(1, (stack_height + sid_count - 1) / sid_count);
  int layer =
      std::max(0, d.sid) * layers_per_sid + (bank_index % layers_per_sid);
  if (layer >= stack_height) {
    layer %= stack_height;
  }
  physical.layer = layer;
  physical.die = layer;
  physical.stack = options_.stack_id;
  if (options_.floorplan_enabled) {
    const int pseudo_channels =
        spec_.has_value() ? std::max(1, spec_->org.pseudo_channels) : 1;
    const int ranks = spec_.has_value() ? std::max(1, spec_->org.ranks) : 1;
    const int bank_groups =
        spec_.has_value() ? std::max(1, spec_->org.bank_groups) : 1;
    const int banks_per_layer = std::max(1, bank_groups * banks_per_group);
    const int floorplan_cols = ceil_sqrt_int(banks_per_layer);
    const int floorplan_rows_per_slice =
        (banks_per_layer + floorplan_cols - 1) / floorplan_cols;
    const int slice = ((std::max(0, d.rank) * sid_count + std::max(0, d.sid)) *
                       pseudo_channels) +
                      std::max(0, d.pseudo_channel);
    physical.floorplan_cols = floorplan_cols;
    physical.floorplan_rows = std::max(
        1, floorplan_rows_per_slice * pseudo_channels * sid_count * ranks);
    physical.tile_x = bank_index % floorplan_cols;
    physical.tile_y =
        slice * floorplan_rows_per_slice + (bank_index / floorplan_cols);
    physical.tile_z = layer;
    physical.tile_id =
        (physical.tile_z * physical.floorplan_rows + physical.tile_y) *
            physical.floorplan_cols +
        physical.tile_x;
  } else {
    physical.floorplan_cols = 1;
    physical.floorplan_rows = 1;
    physical.tile_x = 0;
    physical.tile_y = 0;
    physical.tile_z = layer;
    physical.tile_id = layer;
  }

  const int grid_cols = std::max(1, options_.thermal_grid_cols_per_tile);
  const int grid_rows = std::max(1, options_.thermal_grid_rows_per_tile);
  const int row_count =
      spec_.has_value() ? std::max(1, spec_->org.rows) : std::max(1, d.row + 1);
  const int column_count = spec_.has_value() ? std::max(1, spec_->org.columns)
                                             : std::max(1, d.column + 1);
  auto scaled_index = [](int value, int span, int buckets) {
    int safe_value = std::max(0, value);
    int idx = static_cast<int>((static_cast<long long>(safe_value) * buckets) /
                               std::max(1, span));
    return std::clamp(idx, 0, std::max(1, buckets) - 1);
  };
  physical.thermal_grid_x = scaled_index(d.row, row_count, grid_cols);
  physical.thermal_grid_y = scaled_index(d.column, column_count, grid_rows);
  physical.thermal_x = physical.tile_x * grid_cols + physical.thermal_grid_x;
  physical.thermal_y = physical.tile_y * grid_rows + physical.thermal_grid_y;
  physical.thermal_z = physical.tile_z;
  physical.thermal_cols = std::max(1, physical.floorplan_cols * grid_cols);
  physical.thermal_rows = std::max(1, physical.floorplan_rows * grid_rows);

  const int subarrays = std::max(1, options_.subarrays_per_bank);
  const int mats_x = std::max(1, options_.mats_per_subarray_x);
  const int mats_y = std::max(1, options_.mats_per_subarray_y);
  const int cells_x = std::max(1, options_.cells_per_mat_x);
  const int cells_y = std::max(1, options_.cells_per_mat_y);
  const int row_per_subarray =
      std::max(1, (row_count + subarrays - 1) / subarrays);
  const int row_in_subarray = std::max(0, d.row) % row_per_subarray;
  const int col = std::max(0, d.column);
  physical.subarrays_per_bank = subarrays;
  physical.mats_per_subarray_x = mats_x;
  physical.mats_per_subarray_y = mats_y;
  physical.subarray = scaled_index(d.row, row_count, subarrays);
  physical.mat_x = scaled_index(row_in_subarray, row_per_subarray, mats_x);
  physical.mat_y = scaled_index(col, column_count, mats_y);
  physical.mat_id = physical.mat_y * mats_x + physical.mat_x;
  physical.cell_x = scaled_index(row_in_subarray, row_per_subarray, cells_x);
  physical.cell_y = scaled_index(col, column_count, cells_y);
  physical.microbump_x =
      std::clamp(physical.tile_x % std::max(1, options_.microbumps_x), 0,
                 std::max(1, options_.microbumps_x) - 1);
  physical.microbump_y = std::clamp((physical.tile_y + physical.layer) %
                                        std::max(1, options_.microbumps_y),
                                    0, std::max(1, options_.microbumps_y) - 1);
  return physical;
}

StorageKey MemoryImage::storage_key(Address address,
                                    const DecodedAddress *decoded) const {
  return key_from_physical(physical_address(address, decoded));
}

PhysicalStorageStats MemoryImage::storage_stats() const {
  PhysicalStorageStats stats;
  stats.lines_allocated = backend_->allocated_lines();
  stats.unique_written_lines = backend_->unique_written_lines();
  stats.bytes_allocated = checked_multiply_u64(
      stats.lines_allocated, static_cast<std::uint64_t>(line_size_),
      "allocated storage bytes");
  const bool scan_topology =
      options_.topology_stats_scan_limit == 0 ||
      backend_->address_scan_lines() <= options_.topology_stats_scan_limit;
  stats.topology_scan_skipped = scan_topology ? 0 : 1;
  stats.read_line_accesses = read_line_accesses_;
  stats.write_line_accesses = write_line_accesses_;
  stats.row_buffer_activations = row_buffer_activations_;
  stats.row_buffer_precharges = row_buffer_precharges_;
  stats.row_buffer_dirty_writebacks = row_buffer_dirty_writebacks_;
  stats.row_buffer_clean_precharges = row_buffer_clean_precharges_;
  stats.row_buffer_hits = row_buffer_hits_;
  stats.row_buffer_misses = row_buffer_misses_;
  stats.row_buffer_lazy_loads = row_buffer_lazy_loads_;
  stats.row_buffer_reads = row_buffer_reads_;
  stats.row_buffer_writes = row_buffer_writes_;
  stats.row_buffer_forced_closes = row_buffer_forced_closes_;
  stats.power_events = power_events_;
  stats.thermal_updates = thermal_updates_;
  stats.power_energy_pj = power_energy_pj_;
  stats.power_act_energy_pj = power_act_energy_pj_;
  stats.power_pre_energy_pj = power_pre_energy_pj_;
  stats.power_read_energy_pj = power_read_energy_pj_;
  stats.power_write_energy_pj = power_write_energy_pj_;
  stats.power_refresh_energy_pj = power_refresh_energy_pj_;
  stats.power_rfm_energy_pj = power_rfm_energy_pj_;
  stats.power_control_energy_pj = power_control_energy_pj_;
  stats.thermal_peak_temp_c = thermal_peak_temp_c_;
  stats.thermal_avg_temp_c = options_.thermal_ambient_c;
  stats.thermal_hotspot_layer = thermal_hotspot_layer_;
  stats.thermal_hotspot_x = thermal_hotspot_x_;
  stats.thermal_hotspot_y = thermal_hotspot_y_;
  stats.thermal_lateral_transfers = thermal_lateral_transfers_;
  stats.thermal_vertical_transfers = thermal_vertical_transfers_;
  stats.thermal_tsv_transfers = thermal_tsv_transfers_;
  stats.thermal_coupled_delta_c = thermal_coupled_delta_c_;
  stats.ecc_shadow_updates = ecc_shadow_updates_;
  stats.ecc_checked_reads = ecc_checked_reads_;
  stats.ecc_corrected_errors = ecc_corrected_errors_;
  stats.ecc_uncorrectable_errors = ecc_uncorrectable_errors_;
  stats.ecc_injected_errors = ecc_injected_errors_;
  stats.ecc_parity_repairs = ecc_parity_repairs_;

  std::unordered_set<std::uint64_t> stacks;
  std::unordered_set<std::uint64_t> dies;
  std::unordered_set<std::uint64_t> layers;
  std::unordered_set<std::uint64_t> channels;
  std::unordered_set<std::uint64_t> pseudo_channels;
  std::unordered_set<std::uint64_t> sids;
  std::unordered_set<std::uint64_t> ranks;
  std::unordered_set<std::uint64_t> bank_groups;
  std::unordered_set<std::uint64_t> banks;
  std::unordered_set<std::uint64_t> rows;
  std::unordered_set<std::uint64_t> columns;
  std::unordered_set<std::uint64_t> subarrays;
  std::unordered_set<std::uint64_t> mats;
  std::unordered_set<std::uint64_t> cells;
  std::unordered_set<std::uint64_t> microbumps;
  std::unordered_set<std::uint64_t> floorplan_tiles;

  if (scan_topology) {
    for (Address base : backend_->all_addresses()) {
      DataBlock block;
      if (!load_backend_line(base, block)) {
        continue;
      }
      stats.topology_lines_scanned++;
      const PhysicalAddress &p = block.physical;
      stacks.insert(key_of({p.stack}));
      dies.insert(key_of({p.stack, p.die}));
      layers.insert(key_of({p.stack, p.layer}));
      channels.insert(key_of({p.stack, p.channel}));
      pseudo_channels.insert(key_of({p.stack, p.channel, p.pseudo_channel}));
      sids.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid}));
      ranks.insert(
          key_of({p.stack, p.channel, p.pseudo_channel, p.sid, p.rank}));
      bank_groups.insert(key_of(
          {p.stack, p.channel, p.pseudo_channel, p.sid, p.rank, p.bank_group}));
      banks.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid, p.rank,
                           p.bank_group, p.bank}));
      rows.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid, p.rank,
                          p.bank_group, p.bank, p.row}));
      columns.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid,
                             p.rank, p.bank_group, p.bank, p.row, p.column}));
      subarrays.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid,
                               p.rank, p.bank_group, p.bank, p.subarray}));
      mats.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid, p.rank,
                          p.bank_group, p.bank, p.subarray, p.mat_id}));
      cells.insert(key_of({p.stack, p.channel, p.pseudo_channel, p.sid, p.rank,
                           p.bank_group, p.bank, p.subarray, p.mat_id, p.cell_x,
                           p.cell_y}));
      microbumps.insert(
          key_of({p.stack, p.layer, p.microbump_x, p.microbump_y}));
      floorplan_tiles.insert(key_of({p.stack, p.layer, p.tile_x, p.tile_y}));
    }
  }

  stats.stacks_touched = stacks.size();
  stats.dies_touched = dies.size();
  stats.layers_touched = layers.size();
  stats.channels_touched = channels.size();
  stats.pseudo_channels_touched = pseudo_channels.size();
  stats.sids_touched = sids.size();
  stats.ranks_touched = ranks.size();
  stats.bank_groups_touched = bank_groups.size();
  stats.banks_touched = banks.size();
  stats.rows_touched = rows.size();
  stats.columns_touched = columns.size();
  stats.subarrays_touched = subarrays.size();
  stats.mats_touched = mats.size();
  stats.cells_touched = cells.size();
  stats.microbumps_touched = microbumps.size();
  stats.floorplan_tiles_touched = floorplan_tiles.size();
  stats.thermal_tiles_touched = thermal_tiles_.size();
  stats.thermal_grid_cells_touched = thermal_tiles_.size();
  double temp_sum = 0.0;
  for (const auto &[key, tile] : thermal_tiles_) {
    (void)key;
    temp_sum += tile.temperature_c;
  }
  if (!thermal_tiles_.empty()) {
    stats.thermal_avg_temp_c =
        temp_sum / static_cast<double>(thermal_tiles_.size());
  }
  for (const auto &[key, row_buffer] : row_buffers_) {
    (void)key;
    if (row_buffer.open) {
      stats.row_buffer_open_rows++;
      if (row_buffer.dirty) {
        stats.row_buffer_dirty_rows++;
      }
    }
  }
  return stats;
}

std::optional<DataBlockMetadata>
MemoryImage::metadata(Address address, const DecodedAddress *decoded) const {
  Address base = line_base(address);
  if (const DataBlock *row_block = row_buffer_block(base, decoded)) {
    return metadata_from_block(*row_block);
  }
  DataBlock block;
  if (!load_backend_line(base, block, decoded)) {
    return std::nullopt;
  }
  DataBlockMetadata out = metadata_from_block(block);
  if (decoded != nullptr) {
    out.physical = physical_address(base, decoded);
    out.storage_key = key_from_physical(out.physical);
  }
  return out;
}

std::optional<Address>
MemoryImage::address_for_storage_key(const StorageKey &key) const {
  return backend_->address_for_storage_key(key);
}

DataBlock MemoryImage::make_line(Address base,
                                 const DecodedAddress *decoded) const {
  DataBlock block;
  block.bytes = ByteVector(line_size_, default_value_);
  block.byte_mask = ByteVector(line_size_, 0);
  block.initialized_mask = ByteVector(line_size_, 0);
  block.physical = physical_address(base, decoded);
  block.storage_key = key_from_physical(block.physical);
  block.checksum = checksum_bytes(block.bytes);
  return block;
}

void MemoryImage::refresh_line_metadata(DataBlock &block) {
  block.initialized = all_masked(block.initialized_mask);
  block.checksum = checksum_bytes(block.bytes);
  block.storage_key = key_from_physical(block.physical);
}

bool MemoryImage::load_backend_line(Address base, DataBlock &block,
                                    const DecodedAddress *decoded) const {
  BackendLine line;
  if (!backend_->load(base, line)) {
    return false;
  }
  block.bytes = std::move(line.bytes);
  block.byte_mask.assign(line_size_, 0);
  block.initialized_mask = std::move(line.initialized_mask);
  block.initialized = line.initialized;
  block.version = line.version;
  block.last_writer_request_id = line.last_writer_request_id;
  block.last_write_cycle = line.last_write_cycle;
  block.last_access_cycle = line.last_access_cycle;
  block.checksum = line.checksum;
  block.ecc_hamming = line.ecc_hamming;
  block.ecc_overall = line.ecc_overall;
  block.ecc_parity_bits = line.ecc_parity_bits;
  block.ecc_valid = line.ecc_valid;
  block.ecc_uncorrectable = line.ecc_uncorrectable;
  block.ecc_error_injections = line.ecc_error_injections;
  block.storage_key =
      decoded == nullptr ? line.storage_key : storage_key(base, decoded);
  DecodedAddress stored_decoded = decoded_from_storage_key(block.storage_key);
  block.physical =
      physical_address(base, decoded == nullptr ? &stored_decoded : decoded);
  return true;
}

bool MemoryImage::store_backend_line(Address base, const DataBlock &block) {
  return backend_->store(base, backend_line_from_block(block));
}

FloorplanKey MemoryImage::floorplan_key(const PhysicalAddress &physical) const {
  return FloorplanKey{physical.stack, physical.layer, physical.tile_x,
                      physical.tile_y};
}

ThermalGridKey
MemoryImage::thermal_grid_key(const PhysicalAddress &physical) const {
  return ThermalGridKey{physical.stack, physical.layer, physical.thermal_x,
                        physical.thermal_y};
}

void MemoryImage::refresh_ecc_shadow(DataBlock &block) {
  if (!options_.ecc_shadow_enabled) {
    block.ecc_valid = false;
    block.ecc_uncorrectable = false;
    return;
  }
  SecdedShadow shadow = compute_secded_shadow(block.bytes);
  block.ecc_hamming = shadow.hamming;
  block.ecc_overall = shadow.overall;
  block.ecc_parity_bits = shadow.parity_bits;
  block.ecc_valid = true;
  block.ecc_uncorrectable = false;
  ecc_shadow_updates_++;
}

void MemoryImage::maybe_inject_ecc_error(DataBlock &block) {
  if (!options_.ecc_shadow_enabled || options_.ecc_inject_period <= 0) {
    return;
  }
  if (block.version == 0 ||
      (block.version %
       static_cast<std::uint64_t>(options_.ecc_inject_period)) != 0) {
    return;
  }
  flip_byte_bit(block.bytes, 0);
  block.checksum = checksum_bytes(block.bytes);
  block.ecc_error_injections++;
  ecc_injected_errors_++;
}

void MemoryImage::check_ecc_shadow(DataBlock &block) {
  if (!options_.ecc_shadow_enabled || !options_.ecc_check_on_read ||
      !block.ecc_valid) {
    return;
  }
  ecc_checked_reads_++;
  SecdedShadow now = compute_secded_shadow(block.bytes);
  std::uint64_t syndrome = block.ecc_hamming ^ now.hamming;
  bool overall_mismatch = block.ecc_overall != now.overall;

  if (syndrome == 0 && !overall_mismatch) {
    return;
  }
  if (syndrome == 0 && overall_mismatch) {
    block.ecc_overall = now.overall;
    ecc_parity_repairs_++;
    return;
  }
  if (!overall_mismatch) {
    block.ecc_uncorrectable = true;
    ecc_uncorrectable_errors_++;
    return;
  }
  if (is_power_of_two_u64(syndrome)) {
    block.ecc_hamming = now.hamming;
    block.ecc_overall = now.overall;
    ecc_parity_repairs_++;
    return;
  }
  std::optional<std::size_t> data_bit =
      data_bit_for_code_position(syndrome, block.bytes.size() * 8);
  if (!data_bit.has_value() || !options_.ecc_correct_single_bit) {
    block.ecc_uncorrectable = true;
    ecc_uncorrectable_errors_++;
    return;
  }
  flip_byte_bit(block.bytes, *data_bit);
  block.checksum = checksum_bytes(block.bytes);
  refresh_ecc_shadow(block);
  ecc_corrected_errors_++;
}

void MemoryImage::relax_thermal_tile(ThermalTileState &tile, Cycle cycle) {
  Cycle delta = cycle > tile.last_cycle ? cycle - tile.last_cycle : 0;
  if (delta > 0 && tile.temperature_c > options_.thermal_ambient_c) {
    double cooling = std::min(1.0, static_cast<double>(delta) *
                                       options_.thermal_cooling_per_cycle);
    tile.temperature_c -=
        (tile.temperature_c - options_.thermal_ambient_c) * cooling;
  }
  tile.last_cycle = cycle;
}

void MemoryImage::advance_thermal(Cycle cycle) {
  for (auto &[key, tile] : thermal_tiles_) {
    (void)key;
    relax_thermal_tile(tile, cycle);
  }
}

double
MemoryImage::vertical_coupling_alpha(const PhysicalAddress &physical) const {
  double base = std::max(0.0, options_.thermal_vertical_coupling);
  const double cell_area = std::max(
      1e-18,
      (options_.thermal_chip_dim_x_m / std::max(1, physical.thermal_cols)) *
          (options_.thermal_chip_dim_y_m / std::max(1, physical.thermal_rows)));
  const double tsv_area =
      kPi * options_.thermal_tsv_radius_m * options_.thermal_tsv_radius_m *
      static_cast<double>(std::max(0, options_.thermal_tsvs_per_grid));
  const double tsv_fraction = std::clamp(tsv_area / cell_area, 0.0, 0.90);
  const double silicon_k = std::max(1e-9, options_.thermal_k_silicon);
  const double effective_k = (1.0 - tsv_fraction) * options_.thermal_k_silicon +
                             tsv_fraction * options_.thermal_k_copper;
  const double tsv_boost = options_.thermal_tsv_coupling_scale *
                           std::max(0.0, (effective_k / silicon_k) - 1.0);
  return std::clamp(base + tsv_boost, 0.0, 0.25);
}

void MemoryImage::couple_thermal_neighbor(const ThermalGridKey &source_key,
                                          int dx, int dy, int dz, double alpha,
                                          Cycle cycle, bool tsv_path) {
  if (alpha <= 0.0) {
    return;
  }
  auto src_it = thermal_tiles_.find(source_key);
  if (src_it == thermal_tiles_.end()) {
    return;
  }
  const PhysicalAddress src_physical = src_it->second.physical;
  ThermalGridKey neighbor_key{source_key.stack, source_key.layer + dz,
                              source_key.x + dx, source_key.y + dy};
  if (neighbor_key.layer < 0 || neighbor_key.x < 0 ||
      neighbor_key.x >= std::max(1, src_physical.thermal_cols) ||
      neighbor_key.y < 0 ||
      neighbor_key.y >= std::max(1, src_physical.thermal_rows)) {
    return;
  }
  if (spec_.has_value() &&
      neighbor_key.layer >= std::max(1, spec_->stack_height)) {
    return;
  }

  // 插入 neighbor 可能触发 unordered_map rehash，因此必须在插入之后重新
  // 获取 source iterator；继续使用插入前的引用会成为悬空引用。
  auto [dst_it, inserted] = thermal_tiles_.try_emplace(neighbor_key);
  auto &dst = dst_it->second;
  if (inserted) {
    dst.physical = src_physical;
    auto &p = dst.physical;
    p.stack = neighbor_key.stack;
    p.layer = p.die = p.tile_z = p.thermal_z = neighbor_key.layer;
    p.thermal_x = neighbor_key.x;
    p.thermal_y = neighbor_key.y;
    const int grid_cols = std::max(1, options_.thermal_grid_cols_per_tile);
    const int grid_rows = std::max(1, options_.thermal_grid_rows_per_tile);
    p.tile_x = neighbor_key.x / grid_cols;
    p.tile_y = neighbor_key.y / grid_rows;
    p.thermal_grid_x = neighbor_key.x % grid_cols;
    p.thermal_grid_y = neighbor_key.y % grid_rows;
    p.tile_id = (p.tile_z * p.floorplan_rows + p.tile_y) * p.floorplan_cols + p.tile_x;
    p.microbump_x = p.tile_x % std::max(1, options_.microbumps_x);
    p.microbump_y = (p.tile_y + p.layer) % std::max(1, options_.microbumps_y);
    // A thermal grid cell has no unique inverse DRAM address. Preserve grid
    // geometry, not the source node's channel/bank/row as a fictitious neighbor.
    p.logical_line_base = 0;
    p.byte_offset = 0;
    p.channel = p.pseudo_channel = p.sid = p.rank = -1;
    p.bank_group = p.bank = p.row = p.column = -1;
    p.subarray = p.mat_x = p.mat_y = p.mat_id = p.cell_x = p.cell_y = -1;
    dst.temperature_c = options_.thermal_ambient_c;
    dst.last_cycle = cycle;
  }
  src_it = thermal_tiles_.find(source_key);
  if (src_it == thermal_tiles_.end())
    return;
  auto &src = src_it->second;
  relax_thermal_tile(src, cycle);
  relax_thermal_tile(dst, cycle);

  double delta =
      (src.temperature_c - dst.temperature_c) * std::clamp(alpha, 0.0, 0.25);
  if (std::abs(delta) < 1e-12) {
    return;
  }
  src.temperature_c -= delta;
  dst.temperature_c += delta;
  record_thermal_peak(src);
  record_thermal_peak(dst);
  thermal_coupled_delta_c_ += std::abs(delta);
  if (dz == 0) {
    thermal_lateral_transfers_++;
  } else {
    thermal_vertical_transfers_++;
    if (tsv_path) {
      thermal_tsv_transfers_++;
    }
  }
}

void MemoryImage::record_thermal_peak(const ThermalTileState &tile) {
  if (tile.temperature_c <= thermal_peak_temp_c_)
    return;
  thermal_peak_temp_c_ = tile.temperature_c;
  thermal_hotspot_layer_ = tile.physical.layer;
  thermal_hotspot_x_ = tile.physical.thermal_x;
  thermal_hotspot_y_ = tile.physical.thermal_y;
}

void MemoryImage::apply_thermal_event(const PhysicalAddress &physical,
                                      Cycle cycle, double energy_pj) {
  if (!options_.thermal_enabled) {
    return;
  }
  ThermalGridKey key = thermal_grid_key(physical);
  auto [it, inserted] = thermal_tiles_.try_emplace(key);
  auto &tile = it->second;
  if (inserted) {
    tile.last_cycle = cycle;
    tile.temperature_c = options_.thermal_ambient_c;
  }
  // Zero direct events does not imply a new node: it may already carry heat
  // received from neighbors. Only attach its first representative address.
  if (tile.events == 0) tile.physical = physical;

  relax_thermal_tile(tile, cycle);
  tile.temperature_c += energy_pj * options_.thermal_rise_c_per_pj;
  tile.energy_pj += energy_pj;
  tile.events++;
  tile.last_cycle = cycle;
  thermal_updates_++;
  record_thermal_peak(tile);
  if (options_.thermal_coupling_enabled) {
    const double lateral =
        std::clamp(options_.thermal_lateral_coupling, 0.0, 0.25);
    const double vertical = vertical_coupling_alpha(physical);
    couple_thermal_neighbor(key, -1, 0, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 1, 0, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 0, -1, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 0, 1, 0, lateral, cycle, false);
    couple_thermal_neighbor(key, 0, 0, -1, vertical, cycle, true);
    couple_thermal_neighbor(key, 0, 0, 1, vertical, cycle, true);
  }
}

void MemoryImage::record_command_event(Command command,
                                       const DecodedAddress &decoded,
                                       Cycle cycle, std::size_t payload_bytes) {
  // 坐标是 MemoryImage/StackModel 的公共输入。即使关闭功耗模型，也不能让
  // 非法坐标因为下面的快速返回而悄悄通过。
  validate_decoded_address(decoded);
  if (!options_.power_enabled) {
    return;
  }

  double energy = command_energy_pj(options_, command, payload_bytes) *
                  options_.power_scale;
  if (energy <= 0.0) {
    return;
  }

  PhysicalAddress physical = physical_address(0, &decoded);
  power_events_++;
  power_energy_pj_ += energy;
  if (command == Command::ACT || command == Command::ACT1 ||
      command == Command::ACT2) {
    power_act_energy_pj_ += energy;
  } else if (command_meta(command).precharge) {
    power_pre_energy_pj_ += energy;
  } else if (command == Command::RD || command == Command::RDA ||
             command == Command::CASRD) {
    power_read_energy_pj_ += energy;
  } else if (command == Command::WR || command == Command::WRA ||
             command == Command::CASWR) {
    power_write_energy_pj_ += energy;
  } else if (command_meta(command).refresh) {
    power_refresh_energy_pj_ += energy;
  } else if (command_meta(command).rfm) {
    power_rfm_energy_pj_ += energy;
  } else {
    power_control_energy_pj_ += energy;
  }
  apply_thermal_event(physical, cycle, energy);
}

StorageKey
MemoryImage::bank_key_from_decoded(const DecodedAddress &decoded) const {
  StorageKey key;
  key.stack = options_.stack_id;
  key.channel = decoded.channel;
  key.pseudo_channel = decoded.pseudo_channel;
  key.sid = decoded.sid;
  key.rank = decoded.rank;
  key.bank_group = decoded.bank_group;
  key.bank = decoded.bank;
  key.row = -1;
  key.column = -1;
  return key;
}

StorageKey
MemoryImage::row_key_from_decoded(const DecodedAddress &decoded) const {
  StorageKey key = bank_key_from_decoded(decoded);
  key.row = decoded.row;
  key.column = -1;
  return key;
}

void MemoryImage::writeback_row_buffer(StorageKey bank_key, Cycle cycle) {
  auto rb_it = row_buffers_.find(bank_key);
  if (rb_it == row_buffers_.end() || !rb_it->second.open) {
    return;
  }

  RowBufferEntry &row_buffer = rb_it->second;
  row_buffer_precharges_++;
  if (!row_buffer.dirty) {
    row_buffer_clean_precharges_++;
    row_buffer.open = false;
    row_buffer.columns.clear();
    return;
  }

  for (auto &[column, block] : row_buffer.columns) {
    (void)column;
    Address base = block.physical.logical_line_base;
    block.last_access_cycle = cycle;
    refresh_line_metadata(block);
    store_backend_line(base, block);
    row_buffer_dirty_writebacks_++;
  }
  row_buffer.dirty = false;
  row_buffer.open = false;
  row_buffer.columns.clear();
}

void MemoryImage::activate_row(const DecodedAddress &decoded, Cycle cycle) {
  validate_decoded_address(decoded);
  StorageKey bank_key = bank_key_from_decoded(decoded);
  StorageKey row_key = row_key_from_decoded(decoded);
  auto &row_buffer = row_buffers_[bank_key];
  if (row_buffer.open) {
    if (row_buffer.row_key == row_key) {
      row_buffer.last_access_cycle = cycle;
      return;
    }
    row_buffer_forced_closes_++;
    writeback_row_buffer(bank_key, cycle);
  }

  row_buffer.open = true;
  row_buffer.dirty = false;
  row_buffer.bank_key = bank_key;
  row_buffer.row_key = row_key;
  row_buffer.opened_cycle = cycle;
  row_buffer.last_access_cycle = cycle;
  row_buffer.columns.clear();
  row_buffer_activations_++;
}

void MemoryImage::precharge_bank(const DecodedAddress &decoded, Cycle cycle) {
  validate_decoded_address(decoded);
  writeback_row_buffer(bank_key_from_decoded(decoded), cycle);
}

void MemoryImage::precharge_all(const DecodedAddress &decoded, Cycle cycle) {
  validate_decoded_address(decoded);
  std::vector<StorageKey> targets;
  for (const auto &[bank_key, row_buffer] : row_buffers_) {
    if (!row_buffer.open) {
      continue;
    }
    if (bank_key.channel == decoded.channel &&
        bank_key.pseudo_channel == decoded.pseudo_channel &&
        bank_key.sid == decoded.sid && bank_key.rank == decoded.rank) {
      targets.push_back(bank_key);
    }
  }
  for (const auto &bank_key : targets) {
    writeback_row_buffer(bank_key, cycle);
  }
}

void MemoryImage::flush_dirty_row_buffers(Cycle cycle) {
  for (auto &[bank_key, row_buffer] : row_buffers_) {
    (void)bank_key;
    if (!row_buffer.open || !row_buffer.dirty)
      continue;
    for (auto &[column, block] : row_buffer.columns) {
      (void)column;
      const Address base = block.physical.logical_line_base;
      block.last_access_cycle = cycle;
      refresh_line_metadata(block);
      store_backend_line(base, block);
      row_buffer_dirty_writebacks_++;
    }
    // 这是 checkpoint writeback，不是 PRE：row 仍保持打开，后续访问仍能
    // row-hit；仅 dirty ownership 已同步到后端。
    row_buffer.dirty = false;
  }
}

void MemoryImage::flush_all_row_buffers(Cycle cycle) {
  std::vector<StorageKey> targets;
  targets.reserve(row_buffers_.size());
  for (const auto &[bank_key, row_buffer] : row_buffers_) {
    if (row_buffer.open) {
      targets.push_back(bank_key);
    }
  }
  for (const auto &bank_key : targets) {
    writeback_row_buffer(bank_key, cycle);
  }
}

DataBlock *MemoryImage::row_buffer_block(Address base,
                                         const DecodedAddress *decoded,
                                         Cycle cycle, bool create) {
  if (decoded == nullptr) {
    row_buffer_misses_++;
    return nullptr;
  }
  StorageKey line_key = storage_key(base, decoded);
  StorageKey bank_key = bank_key_from_storage_key(line_key);
  StorageKey row_key = row_key_from_storage_key(line_key);
  auto rb_it = row_buffers_.find(bank_key);
  if (rb_it == row_buffers_.end() || !rb_it->second.open ||
      !(rb_it->second.row_key == row_key)) {
    row_buffer_misses_++;
    return nullptr;
  }

  row_buffer_hits_++;
  RowBufferEntry &row_buffer = rb_it->second;
  row_buffer.last_access_cycle = cycle;
  auto block_it = row_buffer.columns.find(line_key.column);
  if (block_it == row_buffer.columns.end()) {
    DataBlock backing;
    row_buffer_lazy_loads_++;
    if (load_backend_line(base, backing, decoded)) {
      block_it =
          row_buffer.columns.emplace(line_key.column, std::move(backing)).first;
    } else {
      (void)create;
      block_it =
          row_buffer.columns.emplace(line_key.column, make_line(base, decoded))
              .first;
    }
  }
  block_it->second.physical = physical_address(base, decoded);
  block_it->second.storage_key = line_key;
  return &block_it->second;
}

const DataBlock *
MemoryImage::row_buffer_block(Address base,
                              const DecodedAddress *decoded) const {
  if (decoded == nullptr) {
    return nullptr;
  }
  StorageKey line_key = storage_key(base, decoded);
  StorageKey bank_key = bank_key_from_storage_key(line_key);
  StorageKey row_key = row_key_from_storage_key(line_key);
  auto rb_it = row_buffers_.find(bank_key);
  if (rb_it == row_buffers_.end() || !rb_it->second.open ||
      !(rb_it->second.row_key == row_key)) {
    return nullptr;
  }
  auto block_it = rb_it->second.columns.find(line_key.column);
  if (block_it == rb_it->second.columns.end()) {
    return nullptr;
  }
  return &block_it->second;
}

ByteVector MemoryImage::read(Address address, std::size_t size,
                             bool *initialized, const DecodedAddress *decoded) {
  validate_address_span(address, size, "memory image read",
                        backend_->capacity_bytes());
  ByteVector out(size, default_value_);
  bool all_initialized = true;
  std::size_t copied = 0;
  const Address first_base = line_base(address);
  while (copied < size) {
    Address current = address + copied;
    Address base = line_base(current);
    std::size_t offset = line_offset(current);
    std::size_t chunk = std::min(size - copied, line_size_ - offset);
    // decoded 只描述请求起始事务行。跨行访问的后续行必须按其地址重新
    // 推导存储坐标，否则会把首行的 row/column 重用于后续行，并可能从
    // 打开的首行 row buffer 中重复读出错误数据。
    DecodedAddress derived_decoded;
    const DecodedAddress *line_decoded = decoded;
    if (base != first_base) {
      if (spec_.has_value()) {
        derived_decoded = AddressMapper(*spec_).decode(base);
        line_decoded = &derived_decoded;
      } else {
        line_decoded = nullptr;
      }
    }

    read_line_accesses_++;
    DataBlock *rb_block = row_buffer_block(base, line_decoded, 0, false);
    if (rb_block != nullptr) {
      check_ecc_shadow(*rb_block);
      row_buffer_reads_++;
      for (std::size_t i = 0; i < chunk; i++) {
        if (rb_block->initialized_mask[offset + i] == 0) {
          all_initialized = false;
        }
      }
      std::copy_n(rb_block->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                  static_cast<std::ptrdiff_t>(chunk),
                  out.begin() + static_cast<std::ptrdiff_t>(copied));
    } else {
      DataBlock block;
      if (!load_backend_line(base, block, line_decoded)) {
        all_initialized = false;
      } else {
        const ByteVector bytes_before_ecc = block.bytes;
        const std::uint64_t hamming_before_ecc = block.ecc_hamming;
        const bool overall_before_ecc = block.ecc_overall;
        const bool valid_before_ecc = block.ecc_valid;
        const bool uncorrectable_before_ecc = block.ecc_uncorrectable;
        check_ecc_shadow(block);
        for (std::size_t i = 0; i < chunk; i++) {
          if (block.initialized_mask[offset + i] == 0) {
            all_initialized = false;
          }
        }
        std::copy_n(block.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(chunk),
                    out.begin() + static_cast<std::ptrdiff_t>(copied));
        if (block.bytes != bytes_before_ecc ||
            block.ecc_hamming != hamming_before_ecc ||
            block.ecc_overall != overall_before_ecc ||
            block.ecc_valid != valid_before_ecc ||
            block.ecc_uncorrectable != uncorrectable_before_ecc) {
          refresh_line_metadata(block);
          store_backend_line(base, block);
        }
      }
    }
    copied += chunk;
  }
  if (initialized != nullptr) {
    *initialized = all_initialized;
  }
  return out;
}

ByteVector
MemoryImage::read_initialized_mask(Address address, std::size_t size,
                                   const DecodedAddress *decoded) const {
  validate_address_span(address, size, "memory image initialized-mask read",
                        backend_->capacity_bytes());
  ByteVector out(size, 0);
  std::size_t copied = 0;
  const Address first_base = line_base(address);
  while (copied < size) {
    Address current = address + copied;
    Address base = line_base(current);
    std::size_t offset = line_offset(current);
    std::size_t chunk = std::min(size - copied, line_size_ - offset);
    DecodedAddress derived_decoded;
    const DecodedAddress *line_decoded = decoded;
    if (base != first_base) {
      if (spec_.has_value()) {
        derived_decoded = AddressMapper(*spec_).decode(base);
        line_decoded = &derived_decoded;
      } else {
        line_decoded = nullptr;
      }
    }

    if (const DataBlock *rb_block = row_buffer_block(base, line_decoded)) {
      std::copy_n(rb_block->initialized_mask.begin() +
                      static_cast<std::ptrdiff_t>(offset),
                  static_cast<std::ptrdiff_t>(chunk),
                  out.begin() + static_cast<std::ptrdiff_t>(copied));
    } else {
      DataBlock block;
      if (load_backend_line(base, block, line_decoded)) {
        std::copy_n(block.initialized_mask.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(chunk),
                    out.begin() + static_cast<std::ptrdiff_t>(copied));
      }
    }
    copied += chunk;
  }
  return out;
}

void MemoryImage::write(Address address, const ByteVector &data,
                        const ByteVector *mask, const DecodedAddress *decoded,
                        std::uint64_t request_id, Cycle cycle) {
  validate_address_span(address, data.size(), "memory image write",
                        backend_->capacity_bytes());
  if (mask != nullptr && mask->size() != data.size()) {
    throw std::invalid_argument(
        "memory image write mask size does not match data size");
  }

  std::size_t copied = 0;
  Address first_base = line_base(address);
  while (copied < data.size()) {
    Address current = address + copied;
    Address base = line_base(current);
    std::size_t offset = line_offset(current);
    std::size_t chunk = std::min(data.size() - copied, line_size_ - offset);

    write_line_accesses_++;
    DecodedAddress derived_decoded;
    const DecodedAddress *line_decoded = decoded;
    if (base != first_base) {
      if (spec_.has_value()) {
        derived_decoded = AddressMapper(*spec_).decode(base);
        line_decoded = &derived_decoded;
      } else {
        line_decoded = nullptr;
      }
    }
    DataBlock backing;
    const bool backing_exists = load_backend_line(base, backing, line_decoded);
    if (!backing_exists) {
      backing = make_line(base, line_decoded);
    } else if (line_decoded != nullptr) {
      backing.physical = physical_address(base, line_decoded);
    }

    DataBlock *target = row_buffer_block(base, line_decoded, cycle, true);
    bool row_buffer_target = target != nullptr;
    if (target == nullptr) {
      target = &backing;
    } else if (!backing_exists) {
      // 保持既有分配边界：WR 立即分配后端数据块，新字节先留在打开的行缓冲中，
      // 直到 PRE、REF 或最终刷新写回。
      store_backend_line(base, backing);
    }

    std::fill(target->byte_mask.begin(), target->byte_mask.end(), 0);
    bool wrote_any = false;
    for (std::size_t i = 0; i < chunk; i++) {
      std::size_t src = copied + i;
      if (mask == nullptr || (*mask)[src] != 0) {
        target->bytes[offset + i] = data[src];
        target->initialized_mask[offset + i] = 0xff;
        target->byte_mask[offset + i] = 0xff;
        wrote_any = true;
      }
    }
    if (wrote_any) {
      target->version = next_version_++;
      target->last_writer_request_id = request_id;
      target->last_write_cycle = cycle;
      target->last_access_cycle = cycle;
      if (row_buffer_target) {
        row_buffer_writes_++;
        row_buffers_[bank_key_from_storage_key(target->storage_key)].dirty =
            true;
      }
    }
    if (row_buffer_target) {
      target->initialized = all_masked(target->initialized_mask);
      target->checksum = checksum_bytes(target->bytes);
      refresh_ecc_shadow(*target);
      maybe_inject_ecc_error(*target);
    } else {
      refresh_line_metadata(backing);
      refresh_ecc_shadow(backing);
      maybe_inject_ecc_error(backing);
      store_backend_line(base, backing);
    }
    copied += chunk;
  }
}

void MemoryImage::load_text(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open memory image input: " + path);
  }

  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    lineno++;
    std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> tokens = split_tokens(line);
    std::optional<Address> address;
    ByteVector data;
    ByteVector init_mask;
    DecodedAddress decoded;
    bool has_decoded = false;

    for (std::size_t i = 0; i < tokens.size(); i++) {
      const std::string &token = tokens[i];
      std::size_t eq = token.find('=');
      if (eq == std::string::npos) {
        if (!address.has_value()) {
          address = parse_address_token(token);
        } else if (data.empty()) {
          data = parse_hex_bytes(token);
        } else {
          throw std::runtime_error("memory image line " +
                                   std::to_string(lineno) +
                                   " has an unexpected token: " + token);
        }
        continue;
      }

      std::string key = lower_token(token.substr(0, eq));
      std::string value = token.substr(eq + 1);
      if (key == "addr" || key == "address") {
        address = parse_address_token(value);
      } else if (key == "data" || key == "payload") {
        data = parse_hex_bytes(value);
      } else if (key == "init" || key == "initialized" ||
                 key == "initialized_mask") {
        init_mask = parse_hex_bytes(value);
      } else if (is_decoded_key(key)) {
        set_decoded_key(decoded, key, value);
        has_decoded = true;
      } else if (key == "version" || key == "last_writer" ||
                 key == "last_write_cycle" || key == "checksum" ||
                 key == "layer" || key == "die" || key == "stack" ||
                 key == "tile_x" || key == "tile_y" || key == "tile_z" ||
                 key == "tile_id" || key == "floorplan_cols" ||
                 key == "floorplan_rows" || key == "thermal_x" ||
                 key == "thermal_y" || key == "thermal_z" ||
                 key == "thermal_grid_x" || key == "thermal_grid_y" ||
                 key == "thermal_cols" || key == "thermal_rows" ||
                 key == "subarray" || key == "mat_x" || key == "mat_y" ||
                 key == "mat_id" || key == "cell_x" || key == "cell_y" ||
                 key == "microbump_x" || key == "microbump_y" ||
                 key == "ecc_valid" || key == "ecc_hamming" ||
                 key == "ecc_overall" || key == "ecc_uncorrectable" ||
                 key == "ecc_parity_bits") {
        // 重新加载时接受导出元数据，但根据数据和 spec 重新计算。
      } else {
        throw std::runtime_error("memory image line " + std::to_string(lineno) +
                                 " has unknown key: " + key);
      }
    }

    if (!address.has_value()) {
      throw std::runtime_error("memory image line " + std::to_string(lineno) +
                               " missing address");
    }
    if (data.empty()) {
      throw std::runtime_error("memory image line " + std::to_string(lineno) +
                               " missing data");
    }
    validate_address_span(*address, data.size(), "memory image text input",
                          backend_->capacity_bytes());

    std::size_t copied = 0;
    Address first_base = line_base(*address);
    while (copied < data.size()) {
      Address current = *address + copied;
      Address base = line_base(current);
      std::size_t offset = line_offset(current);
      std::size_t chunk = std::min(data.size() - copied, line_size_ - offset);
      const DecodedAddress *line_decoded =
          has_decoded && base == first_base ? &decoded : nullptr;

      DataBlock block;
      if (!load_backend_line(base, block, line_decoded)) {
        block = make_line(base, line_decoded);
      } else if (line_decoded != nullptr) {
        block.physical = physical_address(base, line_decoded);
      }

      std::fill(block.byte_mask.begin(), block.byte_mask.end(), 0);
      for (std::size_t i = 0; i < chunk; i++) {
        std::size_t src = copied + i;
        block.bytes[offset + i] = data[src];
        bool initialized = init_mask.empty() ||
                           (src < init_mask.size() && init_mask[src] != 0);
        block.initialized_mask[offset + i] = initialized ? 0xff : 0x00;
        block.byte_mask[offset + i] = initialized ? 0xff : 0x00;
      }
      block.version = 0;
      block.last_writer_request_id = 0;
      block.last_write_cycle = 0;
      refresh_line_metadata(block);
      refresh_ecc_shadow(block);
      store_backend_line(base, block);
      copied += chunk;
    }
  }
}

void MemoryImage::dump_text(const std::string &path) const {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open memory image output: " + path);
  }
  out << "# address ch pc sid rank bg bank row col layer tile_x tile_y tile_z "
         "tile_id "
      << "subarray mat_x mat_y mat_id cell_x cell_y microbump_x microbump_y "
      << "version last_writer last_write_cycle checksum ecc_valid ecc_hamming "
         "ecc_overall ecc_uncorrectable "
      << "thermal_x thermal_y thermal_z thermal_grid_x thermal_grid_y data "
         "init\n";

  std::vector<Address> bases = backend_->all_addresses();

  for (Address base : bases) {
    DataBlock block;
    if (!load_backend_line(base, block))
      continue;
    const PhysicalAddress &p = block.physical;
    out << format_address(base) << " ch=" << p.channel
        << " pc=" << p.pseudo_channel << " sid=" << p.sid << " rank=" << p.rank
        << " bg=" << p.bank_group << " bank=" << p.bank << " row=" << p.row
        << " col=" << p.column << " layer=" << p.layer << " tile_x=" << p.tile_x
        << " tile_y=" << p.tile_y << " tile_z=" << p.tile_z
        << " tile_id=" << p.tile_id << " subarray=" << p.subarray
        << " mat_x=" << p.mat_x << " mat_y=" << p.mat_y
        << " mat_id=" << p.mat_id << " cell_x=" << p.cell_x
        << " cell_y=" << p.cell_y << " microbump_x=" << p.microbump_x
        << " microbump_y=" << p.microbump_y << " version=" << block.version
        << " last_writer=" << block.last_writer_request_id
        << " last_write_cycle=" << block.last_write_cycle
        << " checksum=" << format_u32_hex(block.checksum)
        << " ecc_valid=" << (block.ecc_valid ? "true" : "false")
        << " ecc_hamming=0x" << std::hex << block.ecc_hamming << std::dec
        << " ecc_overall=" << (block.ecc_overall ? "true" : "false")
        << " ecc_uncorrectable=" << (block.ecc_uncorrectable ? "true" : "false")
        << " thermal_x=" << p.thermal_x << " thermal_y=" << p.thermal_y
        << " thermal_z=" << p.thermal_z
        << " thermal_grid_x=" << p.thermal_grid_x
        << " thermal_grid_y=" << p.thermal_grid_y
        << " data=" << bytes_to_hex(block.bytes)
        << " init=" << bytes_to_hex(block.initialized_mask) << '\n';
  }
}

void MemoryImage::dump_csv(const std::string &path) const {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open memory image CSV output: " + path);
  }

  out << "address,data,init,initialized,version,last_writer,last_write_cycle,"
         "checksum,"
      << "ecc_valid,ecc_hamming,ecc_overall,ecc_uncorrectable,"
      << "channel,pseudo_channel,sid,rank,bank_group,bank,row,column,"
      << "stack,die,layer,tile_x,tile_y,tile_z,tile_id,floorplan_cols,"
         "floorplan_rows,"
      << "thermal_x,thermal_y,thermal_z,thermal_grid_x,thermal_grid_y,thermal_"
         "cols,thermal_rows,"
      << "subarray,mat_x,mat_y,mat_id,cell_x,cell_y,microbump_x,microbump_y\n";

  std::vector<Address> bases = backend_->all_addresses();

  for (Address base : bases) {
    DataBlock block;
    if (!load_backend_line(base, block))
      continue;
    const PhysicalAddress &p = block.physical;
    out << format_address(base) << ',' << bytes_to_hex(block.bytes) << ','
        << bytes_to_hex(block.initialized_mask) << ','
        << (block.initialized ? "true" : "false") << ',' << block.version << ','
        << block.last_writer_request_id << ',' << block.last_write_cycle << ','
        << format_u32_hex(block.checksum) << ','
        << (block.ecc_valid ? "true" : "false") << ',' << "0x" << std::hex
        << block.ecc_hamming << std::dec << ','
        << (block.ecc_overall ? "true" : "false") << ','
        << (block.ecc_uncorrectable ? "true" : "false") << ',' << p.channel
        << ',' << p.pseudo_channel << ',' << p.sid << ',' << p.rank << ','
        << p.bank_group << ',' << p.bank << ',' << p.row << ',' << p.column
        << ',' << p.stack << ',' << p.die << ',' << p.layer << ',' << p.tile_x
        << ',' << p.tile_y << ',' << p.tile_z << ',' << p.tile_id << ','
        << p.floorplan_cols << ',' << p.floorplan_rows << ',' << p.thermal_x
        << ',' << p.thermal_y << ',' << p.thermal_z << ',' << p.thermal_grid_x
        << ',' << p.thermal_grid_y << ',' << p.thermal_cols << ','
        << p.thermal_rows << ',' << p.subarray << ',' << p.mat_x << ','
        << p.mat_y << ',' << p.mat_id << ',' << p.cell_x << ',' << p.cell_y
        << ',' << p.microbump_x << ',' << p.microbump_y << '\n';
  }
}

namespace {

// 项目二进制 checkpoint 使用稀疏格式，只记录已分配数据行。
constexpr std::uint32_t kBinaryMagic = 0x534D4248; // "HBMS" little-endian
constexpr std::uint32_t kBinaryVersion = 1;

template <typename T> void write_le(std::ostream &os, T value) {
  // 显式逐字节编码，不能直接写宿主内存表示；后者在 big-endian 平台上会
  // 生成不兼容文件，与格式声明的 little-endian 语义冲突。
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;
  const Unsigned raw = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(T); i++) {
    os.put(static_cast<char>((raw >> (i * 8)) & Unsigned{0xff}));
  }
}

template <typename T> void read_le(std::istream &is, T &value) {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned raw = 0;
  for (std::size_t i = 0; i < sizeof(T); i++) {
    const int byte = is.get();
    if (byte == std::char_traits<char>::eof()) {
      throw std::runtime_error("binary memory image: unexpected end of file");
    }
    raw |= static_cast<Unsigned>(static_cast<unsigned char>(byte)) << (i * 8);
  }
  value = static_cast<T>(raw);
}

} // namespace

void MemoryImage::dump_binary(const std::string &path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open binary memory image output: " +
                             path);
  }

  // 文件头：magic + version + line_size + entry_count + flags（保留）。
  write_le(out, kBinaryMagic);
  write_le(out, kBinaryVersion);
  write_le(out, static_cast<std::uint32_t>(line_size_));
  if (backend_->allocated_lines() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(
        "binary memory image v1 cannot store more than 2^32-1 lines");
  }
  write_le(out, static_cast<std::uint32_t>(backend_->allocated_lines()));
  write_le(out, std::uint32_t{0}); // flags reserved

  // 按地址排序输出，保证确定性
  std::vector<Address> bases = backend_->all_addresses();

  for (Address base : bases) {
    DataBlock block;
    if (!load_backend_line(base, block))
      continue;
    std::uint32_t data_len = static_cast<std::uint32_t>(block.bytes.size());

    write_le(out, base);
    write_le(out, data_len);
    out.write(reinterpret_cast<const char *>(block.bytes.data()),
              static_cast<std::streamsize>(data_len));
    out.write(reinterpret_cast<const char *>(block.initialized_mask.data()),
              static_cast<std::streamsize>(data_len));
  }

  if (!out) {
    throw std::runtime_error("binary memory image write failed: " + path);
  }
}

void MemoryImage::load_binary(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open binary memory image input: " +
                             path);
  }

  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t file_line_size = 0;
  std::uint32_t entry_count = 0;
  std::uint32_t flags = 0;

  read_le(in, magic);
  read_le(in, version);
  read_le(in, file_line_size);
  read_le(in, entry_count);
  read_le(in, flags);

  if (magic != kBinaryMagic) {
    throw std::runtime_error("binary memory image has invalid magic number: " +
                             path);
  }
  if (version != kBinaryVersion) {
    throw std::runtime_error("binary memory image version mismatch: expected " +
                             std::to_string(kBinaryVersion) + ", got " +
                             std::to_string(version));
  }

  (void)flags; // reserved for future use

  if (file_line_size != static_cast<std::uint32_t>(line_size_)) {
    throw std::runtime_error("binary memory image line_size mismatch: file=" +
                             std::to_string(file_line_size) +
                             " current=" + std::to_string(line_size_));
  }

  // 清空已有数据（load 语义是替换整个存储区）
  backend_->clear();
  row_buffers_.clear();

  for (std::uint32_t i = 0; i < entry_count; i++) {
    Address base = 0;
    std::uint32_t data_len = 0;

    read_le(in, base);
    read_le(in, data_len);

    if (data_len != static_cast<std::uint32_t>(line_size_)) {
      throw std::runtime_error(
          "binary memory image entry " + std::to_string(i) +
          " data_len mismatch: entry=" + std::to_string(data_len) +
          " expected=" + std::to_string(line_size_));
    }

    DataBlock block;
    block.bytes.resize(data_len);
    block.initialized_mask.resize(data_len);
    block.byte_mask.resize(data_len, 0);

    in.read(reinterpret_cast<char *>(block.bytes.data()),
            static_cast<std::streamsize>(data_len));
    in.read(reinterpret_cast<char *>(block.initialized_mask.data()),
            static_cast<std::streamsize>(data_len));

    if (!in) {
      throw std::runtime_error("binary memory image: failed to read entry " +
                               std::to_string(i) + " at address 0x" + [base]() {
                                 std::ostringstream os;
                                 os << std::hex << base;
                                 return os.str();
                               }());
    }

    // 重建元数据
    std::fill(block.byte_mask.begin(), block.byte_mask.end(),
              static_cast<std::uint8_t>(0xff));
    block.initialized = all_masked(block.initialized_mask);
    block.checksum = checksum_bytes(block.bytes);

    // 如果有 spec 信息则重建物理地址，否则只设置逻辑地址
    if (spec_.has_value()) {
      block.physical = physical_address(base);
    } else {
      block.physical.logical_line_base = base;
      block.physical.byte_offset = 0;
    }
    block.storage_key = key_from_physical(block.physical);

    refresh_ecc_shadow(block);
    store_backend_line(base, block);
  }
}

namespace {

bool has_extension(const std::string &path, const std::string &ext) {
  if (path.size() < ext.size())
    return false;
  return path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

std::string lower_path(const std::string &path) {
  std::string s = path;
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

} // namespace

std::vector<Address> MemoryImage::all_addresses() const {
  return backend_->all_addresses();
}

void MemoryImage::load_file(const std::string &path) {
  // 探测文件头是否以 "HBMS" magic 开头 → binary，否则 → text
  std::ifstream probe(path, std::ios::binary);
  if (!probe) {
    throw std::runtime_error("failed to open memory image: " + path);
  }
  char magic[4] = {};
  probe.read(magic, 4);
  probe.close();

  if (magic[0] == 'H' && magic[1] == 'B' && magic[2] == 'M' &&
      magic[3] == 'S') {
    load_binary(path);
  } else {
    load_text(path);
  }
}

void MemoryImage::dump_file(const std::string &path) const {
  std::string lowered = lower_path(path);
  if (has_extension(lowered, ".csv")) {
    dump_csv(path);
  } else if (has_extension(lowered, ".bin")) {
    dump_binary(path);
  } else {
    dump_text(path);
  }
}

void MemoryImage::dump_thermal_text(const std::string &path) const {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open thermal map output: " + path);
  }
  out << "# model_kind=behavioral_sparse_coupling floorplan_enabled="
      << (options_.floorplan_enabled ? "true" : "false")
      << " power_enabled=" << (options_.power_enabled ? "true" : "false")
      << " thermal_enabled=" << (options_.thermal_enabled ? "true" : "false")
      << " power_scale=" << options_.power_scale
      << " thermal_ambient_c=" << options_.thermal_ambient_c
      << " thermal_cooling_per_cycle=" << options_.thermal_cooling_per_cycle
      << " thermal_rise_c_per_pj=" << options_.thermal_rise_c_per_pj
      << " thermal_grid_cols_per_tile=" << options_.thermal_grid_cols_per_tile
      << " thermal_grid_rows_per_tile=" << options_.thermal_grid_rows_per_tile
      << " thermal_coupling_enabled="
      << (options_.thermal_coupling_enabled ? "true" : "false")
      << " thermal_lateral_coupling=" << options_.thermal_lateral_coupling
      << " thermal_vertical_coupling=" << options_.thermal_vertical_coupling
      << " thermal_tsv_coupling_scale=" << options_.thermal_tsv_coupling_scale
      << " thermal_tsv_radius_m=" << options_.thermal_tsv_radius_m
      << " thermal_tsvs_per_grid=" << options_.thermal_tsvs_per_grid
      << " power_source=" << options_.power_source << '\n';
  out << "# stack layer thermal_x thermal_y thermal_z tile_x tile_y grid_x "
         "grid_y tile_id temperature_c energy_pj events thermal_cols "
         "thermal_rows "
      << "ch pc sid rank bg bank row col subarray mat_x mat_y mat_id cell_x "
         "cell_y microbump_x microbump_y address_kind\n";

  std::vector<ThermalGridKey> keys;
  keys.reserve(thermal_tiles_.size());
  for (const auto &[key, tile] : thermal_tiles_) {
    (void)tile;
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end(),
            [](const ThermalGridKey &a, const ThermalGridKey &b) {
              if (a.stack != b.stack)
                return a.stack < b.stack;
              if (a.layer != b.layer)
                return a.layer < b.layer;
              if (a.y != b.y)
                return a.y < b.y;
              return a.x < b.x;
            });

  out << std::fixed << std::setprecision(4);
  for (const auto &key : keys) {
    const ThermalTileState &tile = thermal_tiles_.at(key);
    const PhysicalAddress &p = tile.physical;
    out << key.stack << ' ' << key.layer << ' ' << key.x << ' ' << key.y << ' '
        << p.thermal_z << ' ' << p.tile_x << ' ' << p.tile_y << ' '
        << p.thermal_grid_x << ' ' << p.thermal_grid_y << ' ' << p.tile_id
        << ' ' << tile.temperature_c << ' ' << tile.energy_pj << ' '
        << tile.events << ' ' << p.thermal_cols << ' ' << p.thermal_rows << ' '
        << p.channel << ' ' << p.pseudo_channel << ' ' << p.sid << ' ' << p.rank
        << ' ' << p.bank_group << ' ' << p.bank << ' ' << p.row << ' '
        << p.column << ' ' << p.subarray << ' ' << p.mat_x << ' ' << p.mat_y
        << ' ' << p.mat_id << ' ' << p.cell_x << ' ' << p.cell_y << ' '
        << p.microbump_x << ' ' << p.microbump_y << ' '
        << (tile.events == 0 ? "coupling_only" : "direct_event") << '\n';
  }
}

ByteVector parse_hex_bytes(const std::string &text) {
  std::string hex = compact_hex(text);
  if (hex.empty()) {
    return {};
  }
  if (hex.size() % 2 != 0) {
    hex.insert(hex.begin(), '0');
  }

  ByteVector out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int hi = hex_value(hex[i]);
    int lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      throw std::invalid_argument("invalid hex byte string: " + text);
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

std::string bytes_to_hex(const ByteVector &bytes) {
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (std::uint8_t byte : bytes) {
    os << std::setw(2) << static_cast<unsigned>(byte);
  }
  return os.str();
}

ByteVector make_request_payload(Address address, std::uint64_t request_id,
                                std::size_t size) {
  ByteVector out(size, 0);
  std::uint64_t state =
      address ^ (request_id * 0x9e3779b97f4a7c15ull) ^ 0xd1b54a32d192ed03ull;
  for (std::size_t i = 0; i < size; i++) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    out[i] = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dull) >> 56);
  }
  return out;
}

ByteVector normalize_mask(const ByteVector &mask, std::size_t size) {
  if (mask.empty()) {
    return ByteVector(size, 0xff);
  }
  ByteVector out(size, 0);
  for (std::size_t i = 0; i < size; i++) {
    out[i] = i < mask.size() && mask[i] != 0 ? 0xff : 0x00;
  }
  return out;
}

std::string format_address(Address address) {
  std::ostringstream os;
  os << "0x" << std::hex << std::setfill('0') << std::setw(16) << address;
  return os.str();
}

} // namespace hbm_sim
