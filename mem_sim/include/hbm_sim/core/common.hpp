#pragma once

// 公共基础类型：全项目共享的 cycle/address、命令枚举、策略枚举和字符串转换。
// 这个头文件不依赖任何项目内其他头，避免形成 include 环；新增全局概念时
// 优先考虑是否真的需要放在这里，还是应该放入更具体的模块头。

#include <cstdint>
#include <cstddef>

namespace hbm_sim {

using Cycle = std::uint64_t;
using Address = std::uint64_t;

// 上层请求类型。注意这里的 RequestType 是“frontend 看到的请求意图”，
// 不是 DRAM 命令。一个 Read 可能展开为 ACT -> RD，也可能展开为
// ACT1 -> ACT2 -> CAS_RD -> RD；一个 Maintenance 可能展开为 PREab -> REFab。
// 这样分层后，frontend 不需要理解 DRAM 命令序列，Controller 也可以把
// 普通请求和内部维护请求放进同一套调度/状态/timing 路径。
enum class RequestType {
  // 读请求最终由 RD 完成，并计入 completed_reads、read_bytes 和读延迟。
  Read,
  // 写请求最终由 WR/WRA 完成。Direct PHY 在发出后 1 cycle 完成；Behavioral
  // PHY 还会计入写数据时序与已配置的 pipeline latency。
  Write,
  // 控制器内部维护请求，例如 REFpb/RFMpb。它不来自上层 workload，也不计入
  // read/write bandwidth，但会占用命令总线和 timing scope。
  Maintenance,
};

// 控制器发往 DRAM 的抽象命令集合。
//
// 这不是某一份 JEDEC 文档的逐字命令表，而是为了 HBM/LPDDR 控制器研究
// 选出的“可调抽象命令集”：
// - HBM3/HBM4 用 ACT/PREpb/RD/WR/REFpb/RFMpb 等表达行列双总线行为；
// - LPDDR5/LPDDR6 用 ACT1/ACT2 和 CAS_RD/CAS_WR 表达 split activate 与 WCK；
// - REFab/RFMab/PREab 保留 all-bank 维护路径，便于研究 per-bank/all-bank 策略。
//
// 命令类别不要在各处用 if/else 重新猜测，应通过 command_semantics.hpp 的
// CommandMeta 查询，这能降低后续补命令时漏改某个模块的概率。
enum class Command {
  // HBM-like 标准使用的普通 activate：一条命令直接打开目标 row。
  ACT,
  // LPDDR split activate 第一阶段：进入 activating 状态，但 row 尚不可访问。
  ACT1,
  // LPDDR split activate 第二阶段：完成 row 打开，随后等待剩余 nRCD。
  ACT2,
  // 抽象 precharge：保留给简化模型和兼容旧测试；JEDEC 级路径优先使用 PREpb/PREab。
  PRE,
  // Per-bank precharge：关闭目标 bank 的 open row。
  PREPB,
  // All-bank precharge：关闭作用域内全部 bank，当前框架先作为命令类型保留。
  PREAB,
  // LPDDR read CAS/WCK 同步命令；它不搬运数据，只准备后续 RD。
  CASRD,
  // LPDDR write CAS/WCK 同步命令；它不搬运数据，只准备后续 WR。
  CASWR,
  // 真正的数据读列命令。发出后，请求进入 pending_ 等待读完成延迟。
  RD,
  // 真正的数据写列命令。发出后，请求立即按简化写完成路径统计。
  WR,
  // Read/write with auto-precharge。closed_page/closed_cap 行策略会主动生成，
  // 数据完成后关闭目标 bank。
  RDA,
  WRA,
  // All-bank / per-bank refresh。REFPB 主要用于 HBM per-bank refresh。
  REFAB,
  REFPB,
  // LPDDR6 dual-bank refresh。它刷新一组 dual-bank，当前控制器用目标
  // DecodedAddress 标识这组 bank 中的代表 bank。
  REFDB,
  // HBM4 row-fault mitigation 维护命令。
  RFMAB,
  RFMPB,
  // Mode register access。MRW/MRR 目前作为控制命令建模，用于表达 LPDDR
  // mode register programming、HBM mode/repair 控制和初始化序列的占用。
  MRW,
  MRR,
  // LPDDR6 WCK sync/training。CAS_RD/CAS_WR 是访问前的轻量同步；WCK_SYNC/
  // WCK_TRAIN 表示显式训练或重新同步流程，通常要求 bank idle。
  WCKSYNC,
  WCKTRAIN,
  // LPDDR DVFS 频率切换控制命令，状态机要求进入切换前 channel idle。
  DVFS,
  // 低功耗状态入口/出口。PDE/PDX 表示 power-down，SREFEN/SREFEX 表示 self-refresh。
  PDE,
  PDX,
  SREFEN,
  SREFEX,
  // RAS/ECC/link 维护入口。ECCSCRUB 表示后台 scrub/repair，RASERR 表示错误恢复
  // 或 link retry 占用窗口；具体错误注入后续可继续细化。
  ECCSCRUB,
  RASERR,
  // 占位命令，用于初始化字段或表示本 cycle 没有可发命令。
  NOP,
};

inline constexpr std::size_t kCommandCount = 29;

// command_index() 只用于数组下标，不参与协议语义。所有保存每条命令 ready time
// 或计数的数组都使用这个函数，保证 enum 顺序变化时只有这一处抽象边界。
inline constexpr std::size_t command_index(Command cmd) {
  return static_cast<std::size_t>(cmd);
}

// 命令总线类别。HBM-like 标准启用 Row/Column 双总线；LPDDR-like 标准在
// 当前模型中使用 Unified 单总线。
enum class BusClass {
  // 行命令通道：ACT/ACT1/ACT2/PRE。
  Row,
  // 列命令通道：CAS_RD/CAS_WR/RD/WR。
  Column,
  // 统一命令通道：LPDDR 模式下一次只能发一条任意命令。
  Unified,
};

// SchedulerKind 描述“在一组候选请求之间怎么排序”，不负责判断请求下一条
// 应发什么命令，也不负责 timing 是否 ready。Controller 会先生成候选视图，
// Scheduler 只在这个视图上执行 Ramulator 风格的选择语义。
enum class SchedulerKind {
  // First-ready first-come-first-served：优先 timing-ready 请求，再按 arrival 打破平局。
  FRFCFS,
  // First-come-first-served：最早到达请求若未 ready，则本队列不向后偷跑。
  FCFS,
};

enum class RowPolicyKind {
  // 保持 row open，后续同 row 请求可以形成 row hit。
  OpenPage,
  // 数据命令使用 auto-precharge 路径，完成后关闭 row。
  ClosedPage,
  // Ramulator2.1 ClosedCAP 风格：同一 bank 连续列访问达到 cap 后，
  // 优先把 RD/WR 升级为 RDA/WRA；若 AP 不可发，再注入显式 PREpb。
  ClosedCap,
};

// ChannelMapperKind 是 MemorySystem 级别的 channel 选择策略。
// 它和 AddressMappingKind 不同：AddressMappingKind 决定地址位如何映射到
// decoded 字段；ChannelMapperKind 可以在请求进入多 controller system 时
// 进一步覆盖 decoded.channel，用于压力测试或对齐外部实验。
enum class ChannelMapperKind {
  // 使用 AddressMapper 已经填好的 decoded.channel。
  Decoded,
  // 按请求进入 memory system 的顺序轮转 channel，适合压力测试并行 controller。
  RoundRobin,
  // 用 cache-line index 的高低位异或重新选择 channel，减少简单 stream 只打一个 channel。
  Xor,
};

// AddressMappingKind 描述 cache-line index 的位域拆分顺序。
// 模板名按论文/Ramulator 常见习惯写作“高位到低位”，实现时则按低位依次取字段。
// 例如 RoBaRaCoCh 的低位字段是 Ch，所以连续 cache line 会先跨 channel 分散。
enum class AddressMappingKind {
  // 保持历史行为：低位到高位依次为 column, bank, bank_group, pseudo-channel,
  // SID, rank, channel, row，适合观察顺序流量的 open-row 收益。
  Default,
  // 下列模板名按常见论文/Ramulator 习惯表示“高位到低位”的字段顺序。
  // 例如 RoBaRaCoCh 表示低位优先取 Ch, Co, Ra, Ba, Ro。
  RoBaRaCoCh,
  ChRaBaRoCo,
  RoCoRaBaCh,
};

enum class LpddrEfficiencyMode {
  // 两个 subchannel 都接受命令，当前 LPDDR6 默认模式。
  Normal,
  // Primary subchannel 访问两组 bank，secondary interface 关闭。
  Static,
  // 可进入/退出的 efficiency 模式；本模型用配置固定运行期状态。
  Dynamic,
};

enum class LpddrDvfsMode {
  // 使用 nominal speed-bin。
  Nominal,
  // 使用低速/低功耗 speed-bin。timing profile 会按 low_data_rate_mbps 更新 tCK。
  Low,
  // 关闭 DVFS 行为，保持当前配置给出的 data_rate/tCK。
  Disabled,
};

enum class LpddrWckMode {
  // 维持当前 CAS_RD/CAS_WR 建立 WCK window 的行为。
  CasSync,
  // WCK 视为持续开启，row-hit 后可直接发 RD/WR。用于比较 always-on WCK 开销/收益。
  AlwaysOn,
  // 预留 burst-sync 模式；没有独立状态机前配置层必须拒绝，不能静默按
  // CAS sync 执行并产生看似有效的结果。
  BurstSync,
};

enum class LowPowerMode {
  // 不自动进入低功耗状态。
  Off,
  // 空闲一段时间后进入 power-down；新请求到来时支付 exit latency。
  PowerDown,
  // 空闲一段时间后进入 self-refresh；当前用更长 exit latency 近似。
  SelfRefresh,
};

enum class MaintenancePolicyKind {
  // 按 bank/dual-bank 轮转刷新或 RFM，适合观察 per-bank 维护与普通请求交错。
  PerBank,
  // 用 all-bank 命令维护整个 channel。命令状态机会要求该 channel 内 bank idle。
  AllBank,
};

enum class RefreshTemperatureMode {
  // 常温 refresh interval。
  Normal,
  // 高温 refresh，interval 按 DramSpec::refresh_high_temp_multiplier 加速。
  High,
  // 扩展温度/保守模式，当前与 High 同口径，便于配置区分。
  Extended,
};

// TimingScope 是本项目向 Ramulator 风格靠拢的核心抽象：一条 timing 规则
// 明确作用在哪个层级，而不是隐含在全局变量或单个 bank 状态里。
enum class TimingScope {
  // 整个 channel 共用一个 timing 状态，例如 LPDDR5 的粗粒度命令总线。
  Channel,
  // 每个 pseudo-channel/subchannel 各有 timing 状态，适合 HBM/LPDDR6。
  PseudoChannel,
  // HBM4 的 SID 层级；LPDDR/HBM3 可把 sid 数量保持为 1。
  Sid,
  // 每个 rank 各有 timing 状态，常用于 activation/tFAW 这类约束。
  Rank,
  // 每个 bank group 各有 timing 状态，用于区分 same/different BG 间隔。
  BankGroup,
  // 每个 bank 各有 timing 状态；当前 bank 私有时序主要放在 BankState。
  Bank,
};

inline const char* to_string(RequestType type) {
  switch (type) {
    case RequestType::Read: return "Read";
    case RequestType::Write: return "Write";
    case RequestType::Maintenance: return "Maintenance";
  }
  return "UNKNOWN";
}

inline const char* to_string(Command cmd) {
  switch (cmd) {
    case Command::ACT: return "ACT";
    case Command::ACT1: return "ACT1";
    case Command::ACT2: return "ACT2";
    case Command::PRE: return "PRE";
    case Command::PREPB: return "PREpb";
    case Command::PREAB: return "PREab";
    case Command::CASRD: return "CAS_RD";
    case Command::CASWR: return "CAS_WR";
    case Command::RD: return "RD";
    case Command::WR: return "WR";
    case Command::RDA: return "RDA";
    case Command::WRA: return "WRA";
    case Command::REFAB: return "REFab";
    case Command::REFPB: return "REFpb";
    case Command::REFDB: return "REFdb";
    case Command::RFMAB: return "RFMab";
    case Command::RFMPB: return "RFMpb";
    case Command::MRW: return "MRW";
    case Command::MRR: return "MRR";
    case Command::WCKSYNC: return "WCK_SYNC";
    case Command::WCKTRAIN: return "WCK_TRAIN";
    case Command::DVFS: return "DVFS";
    case Command::PDE: return "PDE";
    case Command::PDX: return "PDX";
    case Command::SREFEN: return "SREFEN";
    case Command::SREFEX: return "SREFEX";
    case Command::ECCSCRUB: return "ECC_SCRUB";
    case Command::RASERR: return "RAS_ERR";
    case Command::NOP: return "NOP";
  }
  return "UNKNOWN";
}

inline const char* to_string(BusClass bus) {
  switch (bus) {
    case BusClass::Row: return "Row";
    case BusClass::Column: return "Column";
    case BusClass::Unified: return "Unified";
  }
  return "UNKNOWN";
}

inline const char* to_string(TimingScope scope) {
  switch (scope) {
    case TimingScope::Channel: return "Channel";
    case TimingScope::PseudoChannel: return "PseudoChannel";
    case TimingScope::Sid: return "Sid";
    case TimingScope::Rank: return "Rank";
    case TimingScope::BankGroup: return "BankGroup";
    case TimingScope::Bank: return "Bank";
  }
  return "UNKNOWN";
}

inline const char* to_string(SchedulerKind kind) {
  switch (kind) {
    case SchedulerKind::FRFCFS: return "FRFCFS";
    case SchedulerKind::FCFS: return "FCFS";
  }
  return "UNKNOWN";
}

inline const char* to_string(RowPolicyKind kind) {
  switch (kind) {
    case RowPolicyKind::OpenPage: return "open_page";
    case RowPolicyKind::ClosedPage: return "closed_page";
    case RowPolicyKind::ClosedCap: return "closed_cap";
  }
  return "UNKNOWN";
}

inline const char* to_string(ChannelMapperKind kind) {
  switch (kind) {
    case ChannelMapperKind::Decoded: return "decoded";
    case ChannelMapperKind::RoundRobin: return "round_robin";
    case ChannelMapperKind::Xor: return "xor";
  }
  return "UNKNOWN";
}

inline const char* to_string(AddressMappingKind kind) {
  switch (kind) {
    case AddressMappingKind::Default: return "default";
    case AddressMappingKind::RoBaRaCoCh: return "RoBaRaCoCh";
    case AddressMappingKind::ChRaBaRoCo: return "ChRaBaRoCo";
    case AddressMappingKind::RoCoRaBaCh: return "RoCoRaBaCh";
  }
  return "UNKNOWN";
}

inline const char* to_string(LpddrDvfsMode mode) {
  switch (mode) {
    case LpddrDvfsMode::Nominal: return "nominal";
    case LpddrDvfsMode::Low: return "low";
    case LpddrDvfsMode::Disabled: return "disabled";
  }
  return "UNKNOWN";
}

inline const char* to_string(LpddrWckMode mode) {
  switch (mode) {
    case LpddrWckMode::CasSync: return "cas_sync";
    case LpddrWckMode::AlwaysOn: return "always_on";
    case LpddrWckMode::BurstSync: return "burst_sync";
  }
  return "UNKNOWN";
}

inline const char* to_string(LowPowerMode mode) {
  switch (mode) {
    case LowPowerMode::Off: return "off";
    case LowPowerMode::PowerDown: return "power_down";
    case LowPowerMode::SelfRefresh: return "self_refresh";
  }
  return "UNKNOWN";
}

inline const char* to_string(LpddrEfficiencyMode mode) {
  switch (mode) {
    case LpddrEfficiencyMode::Normal: return "normal";
    case LpddrEfficiencyMode::Static: return "static";
    case LpddrEfficiencyMode::Dynamic: return "dynamic";
  }
  return "UNKNOWN";
}

inline const char* to_string(MaintenancePolicyKind kind) {
  switch (kind) {
    case MaintenancePolicyKind::PerBank: return "per_bank";
    case MaintenancePolicyKind::AllBank: return "all_bank";
  }
  return "UNKNOWN";
}

inline const char* to_string(RefreshTemperatureMode mode) {
  switch (mode) {
    case RefreshTemperatureMode::Normal: return "normal";
    case RefreshTemperatureMode::High: return "high";
    case RefreshTemperatureMode::Extended: return "extended";
  }
  return "UNKNOWN";
}

}  // namespace hbm_sim
