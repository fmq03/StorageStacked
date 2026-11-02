#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/common.hpp"
#include "hbm_sim/core/memory_backend.hpp"
#include "hbm_sim/core/storage_key.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

struct Stats;

struct DataCheckResult {
  bool matched = true;
  bool initialized = true;
  ByteVector actual;
  std::string message;
};

struct FloorplanKey {
  int stack = 0;
  int layer = 0;
  int tile_x = 0;
  int tile_y = 0;

  bool operator==(const FloorplanKey &other) const {
    return stack == other.stack && layer == other.layer &&
           tile_x == other.tile_x && tile_y == other.tile_y;
  }
};

struct FloorplanKeyHash {
  std::size_t operator()(const FloorplanKey &key) const;
};

struct ThermalGridKey {
  int stack = 0;
  int layer = 0;
  int x = 0;
  int y = 0;

  bool operator==(const ThermalGridKey &other) const {
    return stack == other.stack && layer == other.layer && x == other.x &&
           y == other.y;
  }
};

struct ThermalGridKeyHash {
  std::size_t operator()(const ThermalGridKey &key) const;
};

// 一个逻辑数据块在堆叠存储空间中的位置。数据保存在 MemoryImage 中，
// 该坐标连接 channel/bank/row 研究和热模型物理布局。
struct PhysicalAddress {
  Address logical_line_base = 0;
  std::size_t byte_offset = 0;
  int stack = 0;
  int die = 0;
  int layer = 0;
  int channel = 0;
  int pseudo_channel = 0;
  int sid = 0;
  int rank = 0;
  int bank_group = 0;
  int bank = 0;
  int row = 0;
  int column = 0;
  int tile_x = 0;
  int tile_y = 0;
  int tile_z = 0;
  int tile_id = 0;
  int floorplan_cols = 1;
  int floorplan_rows = 1;
  int thermal_x = 0;
  int thermal_y = 0;
  int thermal_z = 0;
  int thermal_grid_x = 0;
  int thermal_grid_y = 0;
  int thermal_cols = 1;
  int thermal_rows = 1;
  int subarray = 0;
  int mat_x = 0;
  int mat_y = 0;
  int mat_id = 0;
  int cell_x = 0;
  int cell_y = 0;
  int microbump_x = 0;
  int microbump_y = 0;
  int subarrays_per_bank = 1;
  int mats_per_subarray_x = 1;
  int mats_per_subarray_y = 1;
};

struct PhysicalStorageStats {
  std::uint64_t lines_allocated = 0;
  std::uint64_t unique_written_lines = 0;
  std::uint64_t bytes_allocated = 0;
  std::uint64_t topology_lines_scanned = 0;
  std::uint64_t topology_scan_skipped = 0;
  std::uint64_t stacks_touched = 0;
  std::uint64_t dies_touched = 0;
  std::uint64_t layers_touched = 0;
  std::uint64_t channels_touched = 0;
  std::uint64_t pseudo_channels_touched = 0;
  std::uint64_t sids_touched = 0;
  std::uint64_t ranks_touched = 0;
  std::uint64_t bank_groups_touched = 0;
  std::uint64_t banks_touched = 0;
  std::uint64_t rows_touched = 0;
  std::uint64_t columns_touched = 0;
  std::uint64_t subarrays_touched = 0;
  std::uint64_t mats_touched = 0;
  std::uint64_t cells_touched = 0;
  std::uint64_t microbumps_touched = 0;
  std::uint64_t floorplan_tiles_touched = 0;
  std::uint64_t thermal_tiles_touched = 0;
  std::uint64_t thermal_grid_cells_touched = 0;
  std::uint64_t read_line_accesses = 0;
  std::uint64_t write_line_accesses = 0;
  std::uint64_t row_buffer_activations = 0;
  std::uint64_t row_buffer_precharges = 0;
  std::uint64_t row_buffer_dirty_writebacks = 0;
  std::uint64_t row_buffer_clean_precharges = 0;
  std::uint64_t row_buffer_hits = 0;
  std::uint64_t row_buffer_misses = 0;
  std::uint64_t row_buffer_lazy_loads = 0;
  std::uint64_t row_buffer_reads = 0;
  std::uint64_t row_buffer_writes = 0;
  std::uint64_t row_buffer_forced_closes = 0;
  std::uint64_t row_buffer_open_rows = 0;
  std::uint64_t row_buffer_dirty_rows = 0;
  std::uint64_t power_events = 0;
  std::uint64_t thermal_updates = 0;
  double power_energy_pj = 0.0;
  double power_act_energy_pj = 0.0;
  double power_pre_energy_pj = 0.0;
  double power_read_energy_pj = 0.0;
  double power_write_energy_pj = 0.0;
  double power_refresh_energy_pj = 0.0;
  double power_rfm_energy_pj = 0.0;
  double power_control_energy_pj = 0.0;
  double thermal_peak_temp_c = 40.0;
  double thermal_avg_temp_c = 40.0;
  int thermal_hotspot_layer = -1;
  int thermal_hotspot_x = -1;
  int thermal_hotspot_y = -1;
  std::uint64_t thermal_lateral_transfers = 0;
  std::uint64_t thermal_vertical_transfers = 0;
  std::uint64_t thermal_tsv_transfers = 0;
  double thermal_coupled_delta_c = 0.0;
  std::uint64_t ecc_shadow_updates = 0;
  std::uint64_t ecc_checked_reads = 0;
  std::uint64_t ecc_corrected_errors = 0;
  std::uint64_t ecc_uncorrectable_errors = 0;
  std::uint64_t ecc_injected_errors = 0;
  std::uint64_t ecc_parity_repairs = 0;
};

// 把物理存储快照覆盖到公共 Stats。Controller 和 MemorySystem 必须共用
// 这一入口，避免两份字段映射在新增统计项后发生漂移。
void apply_physical_storage_stats(Stats &stats,
                                  const PhysicalStorageStats &storage);

struct StorageModelOptions {
  MemoryBackendOptions memory_backend;
  // 当前 MemoryImage 所代表的被动 stack 实例。单 stack standalone 模式保持 0；
  // 多 stack 模型会为每个 StackModel 设置不同 stack_id。
  int stack_id = 0;
  // sparse 后端覆盖率达到该值时输出层可提示切换 file-backed 后端。
  double sparse_density_warning_pct = 30.0;
  // 详细物理拓扑统计需要为每个层级建立去重集合。0 表示不限制。
  std::uint64_t topology_stats_scan_limit = 100000;
  bool floorplan_enabled = true;
  bool power_enabled = true;
  bool thermal_enabled = true;
  // configured_pj 使用下方显式命令能量；dramsim3_idd 使用 DRAMsim3 风格
  // VDD/IDD 公式推导 ACT/RD/WR/REF 能量，并保留 HBM 缺失参数的可审计性。
  std::string power_source = "configured_pj";
  double power_scale = 1.0;
  double thermal_ambient_c = 40.0;
  double thermal_cooling_per_cycle = 0.00025;
  double thermal_rise_c_per_pj = 0.00002;
  int thermal_grid_cols_per_tile = 1;
  int thermal_grid_rows_per_tile = 1;
  bool thermal_coupling_enabled = true;
  double thermal_lateral_coupling = 0.02;
  double thermal_vertical_coupling = 0.012;
  double thermal_tsv_coupling_scale = 0.03;
  int thermal_tsvs_per_grid = 4;
  double thermal_chip_dim_x_m = 0.01;
  double thermal_chip_dim_y_m = 0.01;
  double thermal_tsv_radius_m = 5e-6;
  double thermal_k_silicon = 148.0;
  double thermal_k_copper = 401.0;
  int subarrays_per_bank = 16;
  int mats_per_subarray_x = 4;
  int mats_per_subarray_y = 4;
  int cells_per_mat_x = 512;
  int cells_per_mat_y = 512;
  int microbumps_x = 8;
  int microbumps_y = 8;
  bool ecc_shadow_enabled = true;
  bool ecc_check_on_read = true;
  bool ecc_correct_single_bit = true;
  int ecc_inject_period = 0;
  double idd_vdd = 1.2;
  double idd0_ma = 65.0;
  double idd2n_ma = 40.0;
  double idd3n_ma = 55.0;
  double idd4r_ma = 390.0;
  double idd4w_ma = 500.0;
  double idd5ab_ma = 250.0;
  double idd5pb_ma = 5.0;
  double idd6x_ma = 31.0;
  double idd_devices_per_rank = 1.0;
  double idd_burst_cycles = 0.0;
  double act_energy_pj = 520.0;
  double act1_energy_pj = 280.0;
  double act2_energy_pj = 280.0;
  double pre_energy_pj = 190.0;
  double preab_energy_pj = 1200.0;
  double cas_energy_pj = 45.0;
  double read_energy_pj = 340.0;
  double read_energy_per_byte_pj = 2.0;
  double write_energy_pj = 390.0;
  double write_energy_per_byte_pj = 2.4;
  double refpb_energy_pj = 950.0;
  double refdb_energy_pj = 1500.0;
  double refab_energy_pj = 4200.0;
  double rfmpb_energy_pj = 1250.0;
  double rfmab_energy_pj = 5200.0;
  double control_energy_pj = 120.0;
};

struct DataBlock {
  ByteVector bytes;
  ByteVector byte_mask;
  ByteVector initialized_mask;
  bool initialized = false;
  std::uint64_t version = 0;
  std::uint64_t last_writer_request_id = 0;
  Cycle last_write_cycle = 0;
  Cycle last_access_cycle = 0;
  std::uint32_t checksum = 0;
  std::uint64_t ecc_hamming = 0;
  bool ecc_overall = false;
  int ecc_parity_bits = 0;
  bool ecc_valid = false;
  bool ecc_uncorrectable = false;
  std::uint64_t ecc_error_injections = 0;
  PhysicalAddress physical;
  StorageKey storage_key;
};

struct DataBlockMetadata {
  bool initialized = false;
  std::uint64_t version = 0;
  std::uint64_t last_writer_request_id = 0;
  Cycle last_write_cycle = 0;
  std::uint32_t checksum = 0;
  std::uint64_t ecc_hamming = 0;
  bool ecc_overall = false;
  int ecc_parity_bits = 0;
  bool ecc_valid = false;
  bool ecc_uncorrectable = false;
  PhysicalAddress physical;
  StorageKey storage_key;
};

struct DataMismatchRecord {
  Cycle cycle = 0;
  std::uint64_t request_id = 0;
  Address address = 0;
  bool initialized = true;
  bool forwarded = false;
  ByteVector expected;
  ByteVector actual;
  std::optional<DataBlockMetadata> block;
  PhysicalAddress physical;
};

class DataValidator {
public:
  DataCheckResult check_read(Cycle cycle, std::uint64_t request_id,
                             Address address, const PhysicalAddress &physical,
                             const ByteVector &expected,
                             const ByteVector &actual, bool initialized,
                             bool forwarded,
                             std::optional<DataBlockMetadata> block);

  std::size_t mismatch_count() const { return mismatches_.size(); }
  const std::vector<DataMismatchRecord> &mismatches() const {
    return mismatches_;
  }
  void dump_text(const std::string &path) const;

private:
  std::vector<DataMismatchRecord> mismatches_;
};

// Hot read paths only need these monotonic counters to annotate responses.
// This snapshot deliberately excludes topology, thermal and backend scans.
struct EccStatusCounters {
  std::uint64_t ecc_corrected_errors = 0;
  std::uint64_t ecc_uncorrectable_errors = 0;
};

class MemoryImage {
public:
  explicit MemoryImage(std::size_t line_size = 64,
                       std::uint8_t default_value = 0);
  explicit MemoryImage(DramSpec spec, std::uint8_t default_value = 0,
                       StorageModelOptions options = {});
  ~MemoryImage();

  std::size_t line_size() const { return line_size_; }
  std::size_t allocated_lines() const {
    return static_cast<std::size_t>(backend_->allocated_lines());
  }
  std::size_t allocated_bytes() const;
  std::uint64_t capacity_bytes() const { return backend_->capacity_bytes(); }
  std::vector<Address> all_addresses() const;
  const StorageModelOptions &options() const { return options_; }
  MemoryBackendKind backend_kind() const { return backend_->kind(); }
  void flush_backend();

  PhysicalAddress
  physical_address(Address address,
                   const DecodedAddress *decoded = nullptr) const;
  StorageKey storage_key(Address address,
                         const DecodedAddress *decoded = nullptr) const;
  PhysicalStorageStats storage_stats() const;
  EccStatusCounters ecc_status_counters() const noexcept {
    return {ecc_corrected_errors_, ecc_uncorrectable_errors_};
  }
  std::optional<DataBlockMetadata>
  metadata(Address address, const DecodedAddress *decoded = nullptr) const;
  std::optional<Address> address_for_storage_key(const StorageKey &key) const;
  std::size_t bank_storage_blocks() const { return allocated_lines(); }
  void activate_row(const DecodedAddress &decoded, Cycle cycle);
  void precharge_bank(const DecodedAddress &decoded, Cycle cycle);
  void precharge_all(const DecodedAddress &decoded, Cycle cycle);
  // Checkpoint/正常运行收尾只把 dirty payload 写入持久化后端，保持 DRAM
  // row-open 状态不变，使同一 Controller 后续继续 enqueue 时状态一致。
  void flush_dirty_row_buffers(Cycle cycle);
  // 析构/最终销毁路径关闭全部行为级 row buffer。
  void flush_all_row_buffers(Cycle cycle);
  // 把所有已创建的稀疏热节点同步冷却到同一查询周期。历史峰值不变；
  // 结束时调用后，thermal_avg_temp_c 和 thermal map 才具有统一时间截面。
  void advance_thermal(Cycle cycle);
  void record_command_event(Command command, const DecodedAddress &decoded,
                            Cycle cycle, std::size_t payload_bytes = 0);

  ByteVector read(Address address, std::size_t size,
                  bool *initialized = nullptr,
                  const DecodedAddress *decoded = nullptr);
  ByteVector
  read_initialized_mask(Address address, std::size_t size,
                        const DecodedAddress *decoded = nullptr) const;
  void write(Address address, const ByteVector &data,
             const ByteVector *mask = nullptr,
             const DecodedAddress *decoded = nullptr,
             std::uint64_t request_id = 0, Cycle cycle = 0);
  void load_text(const std::string &path);
  void dump_text(const std::string &path) const;
  void dump_csv(const std::string &path) const;
  void dump_thermal_text(const std::string &path) const;

  // 二进制持久化：稀疏格式，只存储写过数据的 cache line。
  // 适合大规模仿真场景的 checkpoint 和 golden 验证。
  void load_binary(const std::string &path);
  void dump_binary(const std::string &path) const;

  // 自动检测格式加载：通过文件头 "HBMS" magic 探测二进制格式，否则回退到文本
  void load_file(const std::string &path);
  // 按扩展名选择导出格式：.bin → binary, .txt → text, .csv → csv
  void dump_file(const std::string &path) const;

private:
  struct RowBufferEntry {
    bool open = false;
    bool dirty = false;
    StorageKey bank_key;
    StorageKey row_key;
    Cycle opened_cycle = 0;
    Cycle last_access_cycle = 0;
    std::unordered_map<int, DataBlock> columns;
  };

  struct ThermalTileState {
    PhysicalAddress physical;
    Cycle last_cycle = 0;
    double temperature_c = 40.0;
    double energy_pj = 0.0;
    std::uint64_t events = 0;
  };

  Address line_base(Address address) const;
  std::size_t line_offset(Address address) const;
  void validate_decoded_address(const DecodedAddress &decoded) const;
  DataBlock make_line(Address base, const DecodedAddress *decoded) const;
  void refresh_line_metadata(DataBlock &block);
  bool load_backend_line(Address base, DataBlock &block,
                         const DecodedAddress *decoded = nullptr) const;
  bool store_backend_line(Address base, const DataBlock &block);
  FloorplanKey floorplan_key(const PhysicalAddress &physical) const;
  ThermalGridKey thermal_grid_key(const PhysicalAddress &physical) const;
  StorageKey bank_key_from_decoded(const DecodedAddress &decoded) const;
  StorageKey row_key_from_decoded(const DecodedAddress &decoded) const;
  DataBlock *row_buffer_block(Address base, const DecodedAddress *decoded,
                              Cycle cycle, bool create);
  const DataBlock *row_buffer_block(Address base,
                                    const DecodedAddress *decoded) const;
  void writeback_row_buffer(StorageKey bank_key, Cycle cycle);
  void refresh_ecc_shadow(DataBlock &block);
  void maybe_inject_ecc_error(DataBlock &block);
  void check_ecc_shadow(DataBlock &block);
  void relax_thermal_tile(ThermalTileState &tile, Cycle cycle);
  double vertical_coupling_alpha(const PhysicalAddress &physical) const;
  void couple_thermal_neighbor(const ThermalGridKey &source_key, int dx, int dy,
                               int dz, double alpha, Cycle cycle,
                               bool tsv_path);
  void record_thermal_peak(const ThermalTileState &tile);
  void apply_thermal_event(const PhysicalAddress &physical, Cycle cycle,
                           double energy_pj);

  std::size_t line_size_ = 64;
  std::uint8_t default_value_ = 0;
  std::optional<DramSpec> spec_;
  StorageModelOptions options_;
  std::unique_ptr<MemoryBackend> backend_;
  std::uint64_t next_version_ = 1;
  std::uint64_t read_line_accesses_ = 0;
  std::uint64_t write_line_accesses_ = 0;
  std::uint64_t row_buffer_activations_ = 0;
  std::uint64_t row_buffer_precharges_ = 0;
  std::uint64_t row_buffer_dirty_writebacks_ = 0;
  std::uint64_t row_buffer_clean_precharges_ = 0;
  std::uint64_t row_buffer_hits_ = 0;
  std::uint64_t row_buffer_misses_ = 0;
  std::uint64_t row_buffer_lazy_loads_ = 0;
  std::uint64_t row_buffer_reads_ = 0;
  std::uint64_t row_buffer_writes_ = 0;
  std::uint64_t row_buffer_forced_closes_ = 0;
  std::uint64_t power_events_ = 0;
  std::uint64_t thermal_updates_ = 0;
  double power_energy_pj_ = 0.0;
  double power_act_energy_pj_ = 0.0;
  double power_pre_energy_pj_ = 0.0;
  double power_read_energy_pj_ = 0.0;
  double power_write_energy_pj_ = 0.0;
  double power_refresh_energy_pj_ = 0.0;
  double power_rfm_energy_pj_ = 0.0;
  double power_control_energy_pj_ = 0.0;
  std::uint64_t thermal_lateral_transfers_ = 0;
  std::uint64_t thermal_vertical_transfers_ = 0;
  std::uint64_t thermal_tsv_transfers_ = 0;
  double thermal_coupled_delta_c_ = 0.0;
  // 历史峰值与最终空间平均温度是不同口径。peak 在每次热更新时累计，不能
  // 在 storage_stats() 中用已经冷却后的最终 tile 温度重新计算后覆盖。
  double thermal_peak_temp_c_ = 40.0;
  int thermal_hotspot_layer_ = -1;
  int thermal_hotspot_x_ = -1;
  int thermal_hotspot_y_ = -1;
  std::uint64_t ecc_shadow_updates_ = 0;
  std::uint64_t ecc_checked_reads_ = 0;
  std::uint64_t ecc_corrected_errors_ = 0;
  std::uint64_t ecc_uncorrectable_errors_ = 0;
  std::uint64_t ecc_injected_errors_ = 0;
  std::uint64_t ecc_parity_repairs_ = 0;
  std::unordered_map<StorageKey, RowBufferEntry, StorageKeyHash> row_buffers_;
  std::unordered_map<ThermalGridKey, ThermalTileState, ThermalGridKeyHash>
      thermal_tiles_;
};

ByteVector parse_hex_bytes(const std::string &text);
std::string bytes_to_hex(const ByteVector &bytes);
std::string format_address(Address address);
ByteVector make_request_payload(Address address, std::uint64_t request_id,
                                std::size_t size);
ByteVector normalize_mask(const ByteVector &mask, std::size_t size);

} // namespace hbm_sim
