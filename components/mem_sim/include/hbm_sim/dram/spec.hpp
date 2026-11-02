#pragma once

// DRAM 规格描述：组织结构、timing table、约束作用域和构建结果都在这里定义。
// 后续要补 JEDEC/vendor 表时，应优先扩展 DramSpec/TimingTable，而不是在
// Controller 中硬编码标准差异。

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

enum class DramStandard {
  Unknown,
  Hbm3,
  Hbm4,
  Lpddr5,
  Lpddr6,
};

enum class DramFamily {
  Hbm,
  Lpddr,
};

enum class TimingValueSource {
  // 直接由 JEDEC 表或 JEDEC 公式换算得到。
  JEDEC,
  // 目标器件或供应商数据表给出的值。没有供应商表时不能假装完整。
  Vendor,
  // 由其他 timing 推导得到，例如 nRC = nRAS + nRP。
  Derived,
  // 来自 Ramulator/DRAMsim 等外部参考模型；适合差分和研究基线，但不等于
  // JEDEC 或目标器件校准数据。
  ExternalReference,
  // 研究默认值：用于让模拟器可运行，但做数值级对比前必须替换。
  ResearchDefault,
};

// TimingValueSource 会打印到 timing table CSV 中。后续做数值级对比时，
// 先看 source 分布比只看 nCK 数字更重要：research_default 项意味着“模型能跑，
// 但不能声称对应真实器件”；vendor 项意味着用户已经用目标 speed-bin/density
// 数据覆盖过该 timing。
inline const char *to_string(TimingValueSource source) {
  switch (source) {
  case TimingValueSource::JEDEC:
    return "JEDEC";
  case TimingValueSource::Vendor:
    return "vendor";
  case TimingValueSource::Derived:
    return "derived";
  case TimingValueSource::ExternalReference:
    return "external_reference";
  case TimingValueSource::ResearchDefault:
    return "research_default";
  }
  return "UNKNOWN";
}

struct TimingTableEntry {
  // timing 名称使用项目内部统一写法，例如 nRCDRD、nRFCpb。配置文件中的
  // tRCD_RD_ns 等 JEDEC 风格 key 会在 main.cpp 中映射到这些名称。
  std::string name;
  // 已换算到 nCK 的整数值。Controller 再乘 tick_multiplier 得到内部 tick。
  int value_nck = 0;
  // 数值来源。它影响 strict timing 检查和输出审计，不改变仿真本身。
  TimingValueSource source = TimingValueSource::ResearchDefault;
  // true 表示当前控制器实现会读取这个字段；如果字段暂未被模型使用，
  // 可以设为 false，避免未建模 timing 阻塞构建。
  bool required_for_model = true;
  // true 表示即便模型可运行，做真实器件数值对比前仍必须用 vendor/device
  // 表覆盖。典型例子是 HBM 行时序、RL/WL、部分 density-dependent timing。
  bool vendor_required_for_numeric = false;
  // 给研究者看的来源说明或注意事项，会随 --dump-timing-table 导出。
  std::string note;
};

struct TimingTable {
  // preset_name 用于让 CSV 自描述，避免多个表合并后丢失来源标准。
  std::string preset_name;
  // entries 覆盖 Controller 当前可能读取的 Timing 字段，而不是完整 JEDEC 全表。
  // 这是一个渐进式模型：新增命令/状态后，应同步把相关 timing 加进这里。
  std::vector<TimingTableEntry> entries;

  int source_count(TimingValueSource source) const {
    return static_cast<int>(
        std::count_if(entries.begin(), entries.end(),
                      [source](const TimingTableEntry &entry) {
                        return entry.source == source;
                      }));
  }

  int vendor_required_count() const {
    return static_cast<int>(std::count_if(
        entries.begin(), entries.end(), [](const TimingTableEntry &entry) {
          return entry.vendor_required_for_numeric;
        }));
  }

  int provisional_count() const {
    return static_cast<int>(std::count_if(
        entries.begin(), entries.end(), [](const TimingTableEntry &entry) {
          return entry.vendor_required_for_numeric ||
                 (entry.required_for_model &&
                  (entry.source == TimingValueSource::ResearchDefault ||
                   entry.source == TimingValueSource::ExternalReference));
        }));
  }

  bool complete_for_model() const {
    return std::all_of(
        entries.begin(), entries.end(), [](const TimingTableEntry &entry) {
          return !entry.required_for_model || entry.value_nck > 0;
        });
  }
};

// 所有时序值都以 nCK 为单位保存；Controller 会再乘 tick_multiplier 得到
// 内部调度 cycle。JEDEC 表格中的 ns/us 值统一通过 jedec.hpp 换算进来。
struct Timing {
  // Burst length。当前模型按 cache-line 请求统计字节数，nBL 主要参与读延迟
  // 和写恢复路径，而不展开到每个 beat。
  int nBL = 2;
  // Read CAS latency。read_latency() 用 nCL + nBL 估算读完成时间。
  int nCL = 20;
  // Write CAS latency。WR 后到可 PRE 的路径会用到 nCWL + nBL + nWR。
  int nCWL = 10;
  // ACT/ACT1 序列起点到 RD 的最短等待。LPDDR 的 ACT2 可以在 tAAD
  // 合法窗口内发出，因而不能再从这里扣除一个固定 nAAD。
  int nRCDRD = 20;
  // ACT/ACT2 后到 WR 的最短等待。读写分开便于研究不对称时序。
  int nRCDWR = 14;
  // PRE 后到下一次 ACT 的等待。
  int nRP = 20;
  // ACT 后 row 必须保持打开的最短时间，限制过早 PRE。
  int nRAS = 42;
  // 同一 bank 连续两次 ACT 的最短间隔。
  int nRC = 62;
  // RD 后到 PRE 的最短间隔。
  int nRTP = 8;
  // 写恢复时间，和 nCWL/nBL 一起限制 WR 后 PRE。
  int nWR = 18;
  // Different bank-group 或更松 scope 下的短列命令间隔。
  int nCCDS = 2;
  // Same bank-group 或更紧 scope 下的长列命令间隔。
  int nCCDL = 4;
  // Different bank-group 或更松 scope 下的短 activate 间隔。
  int nRRDS = 4;
  // Same bank-group 或更紧 scope 下的长 activate 间隔。
  int nRRDL = 6;
  // Four activate window。当前模型在 activation_scope 内最多允许 4 次 ACT。
  int nFAW = 16;

  // LPDDR split activate 的 ACT1 -> ACT2 允许窗口。JEDEC 的 tAAD 是最晚
  // deadline，不是固定等待；最早值单独保留，便于研究命令总线插空。
  // 旧配置键 nAAD/tAAD_ns 兼容映射到 nAADMax。
  int nAADMin = 1;
  int nAADMax = 8;

  // LPDDR5/LPDDR6 的 WCK/CAS 简化模型：CAS_RD/CAS_WR 打开 WCK 相位，
  // nWCK2CK 后允许 RD/WR，nWCKPST 表示本次 WCK 同步可覆盖的后续窗口。
  int nWCK2CK = 1;
  int nWCKPST = 8;
  int nCAS = 0;
  int nCS = 2;
  int nPPD = 2;
  int nRPab = 20;
  int nWTRS = 8;
  int nWTRL = 12;
  int nRTW = 16;
  int nCCDR = 2;
  int nRFC = 0;
  int nRFCpb = 0;
  int nRFMab = 0;
  int nRFMpb = 0;
  int nRREFD = 0;
  // LPDDR6 REFdb 专用恢复/间隔。nRREFD 保留给通用 refresh->ACT
  // 近似；这三个字段直接对应 dual-bank refresh 路径，避免把 REFdb->ACT
  // 和 REFdb->REFdb 的 short/long 间隔混成一个数。
  int nREFDB2ACT = 0;
  int nREFDB2REFDBS = 0;
  int nREFDB2REFDBL = 0;
  int nREFI = 0;
  int nREFIpb = 0;
  // 控制/训练/低功耗/RAS 相关 timing。它们不会替代核心 ACT/RD/WR 时序，
  // 而是给完整初始化、DVFS、MR programming、WCK training、ECC/RAS 恢复序列
  // 提供可审计的表项。
  int nMRW = 8;
  int nMRR = 8;
  int nWCKSYNC = 8;
  int nWCKTRAIN = 64;
  int nDVFS = 128;
  int nPDEX = 8;
  int nSREFEX = 256;
  int nECCSCRUB = 64;
  int nRASERR = 64;
  int nLINKRETRY = 16;
  double tCK_ps = 500.0;

  int read_latency() const { return nCL + nBL; }
};

struct TimingConstraint {
  TimingScope scope = TimingScope::Bank;
  std::vector<Command> preceding;
  std::vector<Command> following;
  int latency = 0;
  // window > 0 表示这是 nFAW 这类“窗口内最多 N 次”的约束，而不是普通
  // preceding->following ready time。当前由 TimingEngine 用专门的 tFAW
  // 窗口执行。
  int window = 0;
  // false：preceding 只限制相同 scope bucket；true：只限制同一父级中的
  // 其他 sibling bucket。例如 HBM tCCDR 在 SID scope 上只约束 different SID，
  // 不应错误覆盖 consecutive READs to the same SID。
  bool sibling = false;
  std::string note;
  // 可审计的约束名称，例如 nRCDRD 或 nCWL+nBL+nWR。放在聚合尾部
  // 以保持旧测试中 TimingConstraint 初始化器的字段顺序兼容。
  std::string parameter;
};

struct TimingSourceOverride {
  // name 必须匹配 TimingTableEntry::name。用户通过 config/CLI 覆盖 timing 后，
  // main.cpp 会把对应字段登记为 override。
  std::string name;
  // 默认标为 Vendor，因为外部覆盖通常来自目标器件手册或实验 speed-bin。
  TimingValueSource source = TimingValueSource::Vendor;
  // 记录覆盖原因，例如 “Overridden by config/CLI.”，导出 CSV 时保留。
  std::string note;
};

// DRAM 组织结构。地址映射按 column -> bank -> bank_group -> pseudo_channel
// -> rank -> channel -> row 的低位到高位顺序拆分 cache-line 编号。
struct Organization {
  // 当前模型允许多 channel，但 preset 默认保持小规模，方便调试。
  int channels = 1;
  // HBM pseudo-channel 或 LPDDR6 subchannel 的紧凑抽象。
  int pseudo_channels = 1;
  // HBM4 stack 中的 stack id / pseudo-channel 内部层级。非 HBM4 可保持 1。
  int sids = 1;
  // rank 数；多 rank 时 activation_scope 可以细化到 rank。
  int ranks = 1;
  // bank group 数，决定 nRRDS/nRRDL 和 nCCDS/nCCDL 的差异是否明显。
  int bank_groups = 4;
  // 每个 bank group 内的 bank 数。
  int banks_per_group = 4;
  // 每个 bank 的 row 数。地址映射会把高位映射到 row。
  int rows = 1 << 15;
  // 每个 row 内可寻址的 column transaction 数。
  int columns = 1 << 6;
  // 请求粒度。当前一个 Request 默认表示一个 cache line。
  int line_size = 64;
  // 单条 DRAM RD/WR 命令承载的 payload 字节数。0 表示沿用 line_size。
  // HBM4 单 pseudo-channel 的 BL8 为 32 DQ * 8 UI = 32B，因此可以保留
  // 64B host/cache line，同时把它拆成两个 32B DRAM transaction。
  int dram_transaction_bytes = 0;
};

// StandardTraits 只描述标准身份、协议能力和默认 profile 选择。
// default speed/density/stack 只是选择 profile 的维度；organization/timing
// 数值属于 profiles.cpp，不放在 traits 中。
struct StandardTraits {
  DramStandard standard = DramStandard::Unknown;
  DramFamily family = DramFamily::Hbm;
  std::string_view canonical_key;
  std::string_view display_name;
  std::array<std::string_view, 3> aliases{};
  std::size_t alias_count = 0;

  std::string_view default_timing_profile;
  std::string_view default_mode_profile = "default";
  int default_speed_bin_mbps = 0;
  int default_density_gb = 0;
  int default_stack_height = 0;

  bool supports_rfm = false;
  bool supports_ecc = false;
  bool lpddr_dual_bank_refresh = false;
  bool lpddr_wck_training_required = true;

  std::string_view hbm_edge_pairing_matrix = "hbm4_core_pre_pairing";
  std::string_view hbm_sid_mapping = "stack_height_div4";
  std::string_view hbm_ecc_scheme = "none";
  std::string_view hbm_ras_policy = "metadata_only";
  std::string_view lpddr_mode_register_profile = "default";

  TimingScope activation_scope = TimingScope::Rank;
  TimingScope row_bus_scope = TimingScope::PseudoChannel;
  TimingScope column_bus_scope = TimingScope::PseudoChannel;
  TimingScope wck_scope = TimingScope::PseudoChannel;
};

struct DramSpec {
  DramStandard standard = DramStandard::Unknown;
  DramFamily family = DramFamily::Hbm;
  // 标准名称会打印到 CLI 输出，便于结果文件自描述。
  std::string name;
  // 组织结构和 timing table 分开保存，便于替换器件参数。
  Organization org;
  Timing timing;
  // 标准/供应商 timing profile 选择维度。profile 先按 standard + speed-bin +
  // density + stack-height + mode 展开成 timing，之后配置文件仍可逐项覆盖。
  std::string timing_profile = "generic";
  std::string vendor_profile = "generic";
  std::string mode_profile = "default";
  int speed_bin_mbps = 0;
  // Gibit per die (HBM) or per subchannel/rank (LPDDR). Fractional research
  // geometries must not be rounded to an unrelated density table row.
  double density_gb = 0;
  // Internal channel-local views retain their parent device's timing/density.
  // Zero denotes a complete user model; otherwise this is the parent's Channel
  // count after MemorySystem localizes org.channels to one. Not a cfg key.
  int density_reference_channels = 0;
  int stack_height = 0;
  // true 时，tick() 每周期分别尝试一条 column 和一条 row 命令。
  bool dual_command_bus = false;
  // true 时，ACT 被拆成 ACT1/ACT2，用于 LPDDR。
  bool split_activate = false;
  // true 时，row hit 的 RD/WR 前需要检查 WCK/CAS 窗口。
  bool lpddr_family = false;
  // 外部接口每 pin 数据速率，单位 Mbps。它用于 peak_bandwidth_GBps，
  // 不直接决定 controller tick；tick 由 timing.tCK_ps 与 tick_multiplier 控制。
  int data_rate_mbps = 6400;

  // 当前建模 slice 的聚合数据宽度。它是描述性字段；实际输出带宽由完成请求
  // 的字节数统计得到。
  int data_bus_bits = 128;
  int internal_prefetch_size = 8;
  // DFI 6.x-oriented 行为抽象。可供在线 MemPhy 和 command/data beat trace 使用
  // 以及 DFI-like signal CSV；完整 pin-level DFI 协议仍在 validation 层外。
  // phase_count=0 表示按 tick_multiplier 自动派生。
  int dfi_phase_count = 0;
  int dfi_data_lane_bytes = 0;
  int dfi_read_latency_nck = 0;
  int dfi_write_latency_nck = 0;
  // tick_multiplier 用来表达半周期或更细粒度发射。例如 HBM edge pairing 下
  // 一个 nCK 可以拆成 rising/falling 两个 controller tick。
  int tick_multiplier = 1;
  // full_stack_model 是结果自描述字段：true 表示 preset 试图表达完整 stack
  // 级别接口和 channel 数，而不是早期紧凑 slice。
  bool full_stack_model = false;
  // supports_refresh 控制 RefreshManager 是否定期产生维护请求。
  bool supports_refresh = false;
  // refresh_policy 决定 refresh 维护以 per-bank/dual-bank 轮转还是 all-bank
  // 命令发出。
  MaintenancePolicyKind refresh_policy = MaintenancePolicyKind::PerBank;
  // LPDDR6 REFdb 会刷新一个 dual-bank pair；HBM/HBM3/HBM4 通常保持 false。
  bool lpddr_dual_bank_refresh = false;
  // supports_rfm 控制 ACT 计数达到阈值后是否触发 RFM/PRAC 维护请求。
  bool supports_rfm = false;
  // rfm_policy 决定 RFM 使用 RFMpb 还是 RFMab。RFMab 要求目标 rank 内全部
  // bank idle；不同 rank 保持独立状态。
  MaintenancePolicyKind rfm_policy = MaintenancePolicyKind::PerBank;
  // supports_ecc 当前只作为接口开关和输出自描述；具体 ECC 码型仍待补。
  bool supports_ecc = false;
  // HBM4 full-stack 细节开关。它们主要影响地址/统计/trace 自描述与接口开销；
  // 具体命令约束仍通过 timing scope 和 controller path 执行。
  bool hbm_full_32_channel_stack = false;
  bool hbm_sid_interleave = true;
  bool hbm_pc_interleave = true;
  // hbm_edge_pairing 为 true 时，Controller 会区分 rising/falling edge，并限制
  // falling edge 上只能出现满足 pairing 规则的 PREpb/PREab。
  bool hbm_edge_pairing = false;
  bool hbm_strict_edge_pairing = true;
  // HBM4 细协议的命名化配置。它们不会把 JEDEC 表硬编码成 if/else，
  // 而是让实验结果记录当前使用的 pairing/link/RAS/ECC 解释。
  std::string hbm_edge_pairing_matrix = "hbm4_core_pre_pairing";
  std::string hbm_sid_mapping = "stack_height_div4";
  std::string hbm_ecc_scheme = "none";
  std::string hbm_ras_policy = "metadata_only";
  std::string hbm_link_crc_mode = "off";
  bool hbm_link_retry_enabled = false;
  int hbm_link_crc_bits_per_request = 0;
  int hbm_ras_metadata_bits_per_request = 0;
  int hbm_ecc_bits_per_request = 0;
  // RAA/PRAC 触发阈值。达到该 ACT 次数后 RfmManager 会产生 RFM 维护请求。
  int rfm_act_threshold = 0;
  // RFM 发出后对 ACT 计数的递减值。默认等于阈值时表示一次 RFM
  // 清掉一次阈值积累。
  int rfm_decrement = 0;
  // LPDDR link protection/efficiency 当前主要影响配置自描述和接口 overhead
  // 入口。
  bool lpddr_link_protection = false;
  bool lpddr_dynamic_efficiency = false;
  LpddrEfficiencyMode lpddr_efficiency_mode = LpddrEfficiencyMode::Normal;
  LpddrDvfsMode lpddr_dvfs_mode = LpddrDvfsMode::Nominal;
  int lpddr_low_data_rate_mbps = 3200;
  LpddrWckMode lpddr_wck_mode = LpddrWckMode::CasSync;
  int lpddr_wck_ratio = 4;
  std::string lpddr_mode_register_profile = "default";
  // LPDDR6 更完整行为的配置入口：MR profile 描述 RL/WL/BL 等寄存器组合；
  // WCK training/DVFS/link/low-power policy 记录控制器采用的状态序列模型。
  std::string lpddr_wck_training_mode = "startup_only";
  std::string lpddr_dvfs_transition_policy = "idle_channel";
  std::string lpddr_link_protection_mode = "off";
  std::string lpddr_low_power_state_policy = "controller_idle";
  bool lpddr_wck_training_required = true;
  bool lpddr_dbi_enabled = false;
  bool lpddr_link_ecc_enabled = false;
  // LPDDR6 CA parity 是命令/地址总线保护特性。手册要求启用时 WCK Always ON，
  // 且不能运行时 on-the-fly 切换；本模型把它作为静态 mode 配置，并按每条
  // LPDDR 命令统计 CA parity 接口开销。
  bool lpddr_ca_parity_enabled = false;
  int lpddr_dbi_bits_per_request = 0;
  int lpddr_link_ecc_bits_per_request = 0;
  int lpddr_ca_parity_bits_per_command = 1;
  LowPowerMode low_power_mode = LowPowerMode::Off;
  int low_power_entry_cycles = 0;
  int low_power_exit_cycles = 0;
  int self_refresh_exit_cycles = 0;
  // Refresh credit/postpone/pull-in 和温度策略。credit 模型在 RefreshManager
  // 中维护， 用于研究 refresh 与普通请求的仲裁，而不是只按固定间隔硬插 REF。
  int refresh_postpone_limit = 0;
  int refresh_pullin_limit = 0;
  int refresh_credit_limit = 0;
  RefreshTemperatureMode refresh_temperature_mode =
      RefreshTemperatureMode::Normal;
  int refresh_high_temp_multiplier = 2;
  // 地址映射模板。做 Ramulator2.1/论文对比时必须保持和对方相同，否则 row hit
  // 和 bank/channel 分布会不可比。
  AddressMappingKind address_mapping = AddressMappingKind::Default;
  // 每个请求携带的协议 metadata/ECC overhead，参与 interface_* byte 统计。
  int metadata_bits_per_request = 0;
  int ecc_bits_per_request = 0;
  // 表驱动 timing 约束。Controller/TimingEngine 会按 scope 创建 ready-time
  // bucket。
  std::vector<TimingConstraint> timing_constraints;
  // 当前 preset 的 timing table 和来源元数据。输出、strict 检查和 CSV dump
  // 都读这里。
  TimingTable timing_table;
  // 用户覆盖 timing 后记录在这里，再由 refresh_timing_table() 合并到
  // timing_table。
  std::vector<TimingSourceOverride> timing_source_overrides;

  // Ramulator 风格的 timing 作用域声明。控制器会按这些层级创建独立状态，
  // 因此 HBM4/LPDDR 可以共享调度框架，但拥有不同的约束粒度。
  TimingScope activation_scope = TimingScope::Rank;
  TimingScope row_bus_scope = TimingScope::PseudoChannel;
  TimingScope column_bus_scope = TimingScope::PseudoChannel;
  TimingScope wck_scope = TimingScope::PseudoChannel;

  // 单 channel 内的 bank 数，包含 pseudo-channel/rank/bank-group 维度。
  int banks_per_channel() const {
    const std::uint64_t count =
        checked_product({org.pseudo_channels, org.sids, org.ranks,
                         org.bank_groups, org.banks_per_group},
                        "banks_per_channel");
    if (count > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      throw std::overflow_error("banks_per_channel exceeds int range");
    }
    return static_cast<int>(count);
  }

  // 控制器用扁平数组保存全部 bank 状态，因此需要全局 bank 数。
  int total_banks() const {
    const std::uint64_t count =
        checked_product({org.channels, org.pseudo_channels, org.sids, org.ranks,
                         org.bank_groups, org.banks_per_group},
                        "total_banks");
    if (count > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      throw std::overflow_error("total_banks exceeds int range");
    }
    return static_cast<int>(count);
  }

  // DRAM 地址几何中的 column transaction 数。每个位置对应一次单 PC/subchannel
  // RD/WR，而不是一个 host cache line。
  std::uint64_t total_addressable_transactions() const {
    return checked_product({org.channels, org.pseudo_channels, org.sids,
                            org.ranks, org.bank_groups, org.banks_per_group,
                            org.rows, org.columns},
                           "total_addressable_transactions");
  }

  // host/frontend 默认请求大小，仍按 cache line 计。
  int bytes_per_request() const { return org.line_size; }

  int transaction_bytes() const {
    return org.dram_transaction_bytes > 0 ? org.dram_transaction_bytes
                                          : std::max(1, org.line_size);
  }

  std::uint64_t addressable_capacity_bytes() const {
    const std::uint64_t transactions = total_addressable_transactions();
    const std::uint64_t bytes = static_cast<std::uint64_t>(transaction_bytes());
    if (transactions != 0 &&
        bytes > std::numeric_limits<std::uint64_t>::max() / transactions) {
      throw std::overflow_error(
          "addressable_capacity_bytes exceeds uint64 range");
    }
    return transactions * bytes;
  }

  // MemoryImage/backend 按 DRAM transaction 粒度分配块；对 HBM4 来说这是
  // 32B，而 frontend line_size 仍可保持 64B。
  std::uint64_t total_addressable_lines() const {
    return total_addressable_transactions();
  }

  double interface_transfer_rate_gbps() const {
    return static_cast<double>(data_rate_mbps) / 1000.0;
  }

  double peak_bandwidth_GBps() const {
    return static_cast<double>(data_rate_mbps) *
           static_cast<double>(data_bus_bits) / 8000.0;
  }

  bool lpddr_requires_wck_retrain_after_dvfs() const {
    // LPDDR6 手册里的 DVFS/WCK 行为和具体 MR profile、训练模式有关。这里不把
    // 每个 MR bit 展开成器件级状态机，而是先用 policy 字符串表达控制器是否认为
    // DVFS 后必须重新训练 WCK。默认 profile 使用 startup_and_dvfs_retrain，因此
    // 显式 DVFS 命令之后，CAS/RD/WR 必须等 WCK_TRAIN 清除该状态。
    if (!lpddr_family || !lpddr_wck_training_required ||
        lpddr_dvfs_mode == LpddrDvfsMode::Disabled) {
      return false;
    }
    return lpddr_wck_training_mode.find("dvfs") != std::string::npos ||
           lpddr_wck_training_mode.find("DVFS") != std::string::npos ||
           lpddr_wck_training_mode.find("retrain") != std::string::npos ||
           lpddr_wck_training_mode.find("Retrain") != std::string::npos;
  }

  double cycles_per_second() const {
    return timing.tCK_ps <= 0.0
               ? 0.0
               : (1.0e12 / timing.tCK_ps) * std::max(1, tick_multiplier);
  }

private:
  static std::uint64_t checked_product(std::initializer_list<int> factors,
                                       const char *context) {
    std::uint64_t result = 1;
    for (int factor : factors) {
      if (factor <= 0) {
        throw std::invalid_argument(std::string(context) +
                                    " requires positive dimensions");
      }
      const std::uint64_t value = static_cast<std::uint64_t>(factor);
      if (result > std::numeric_limits<std::uint64_t>::max() / value) {
        throw std::overflow_error(std::string(context) +
                                  " exceeds uint64 range");
      }
      result *= value;
    }
    return result;
  }
};

const StandardTraits &find_standard_traits(const std::string &name);
void apply_standard_traits(DramSpec &spec, const StandardTraits &traits);

// Draft 只应用 traits，供底层 profile 构造使用；用户配置优先走 config::build_model。
DramSpec make_spec_draft(const std::string &name);
// 完整默认模型：traits -> default profile -> finalize。
DramSpec make_spec(const std::string &name);
// Density lookup scope: HBM per physical die; LPDDR per channel/subchannel/rank.
// Capacity itself remains the integer geometry product, not this floating value.
double density_gbit_from_geometry(double capacity_bytes, bool lpddr_family,
                                 int stack_height, int channels,
                                 int subchannels, int ranks);
// 只检查当前规格是否合法，不修改 timing table/constraint；供 Controller、
// MemoryImage、PHY 等公开库入口防止绕过 CLI/finalize 后静默降级。
void validate_spec(const DramSpec &spec);
void finalize_spec(DramSpec &spec);
void refresh_timing_constraints(DramSpec &spec);
void refresh_timing_table(DramSpec &spec);
void set_timing_source(DramSpec &spec, const std::string &name,
                       TimingValueSource source, std::string note = {});
std::vector<std::string> validate_timing_table(const DramSpec &spec,
                                               bool require_vendor_values);
std::vector<std::string> supported_specs();

} // namespace hbm_sim
