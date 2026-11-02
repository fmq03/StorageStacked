#pragma once

// DRAM 命令语义表。命令是否属于 row/column/refresh/RFM/auto-precharge 等类别
// 都由这里定义，供 Controller、Validator、RowPolicy 共用。

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

struct DecodedAddress;
struct DramSpec;

struct CommandMeta {
  // 可读命令名，CSV trace 和统计输出都使用它。
  const char *name = "NOP";
  // 是否占用 row/command path。HBM dual bus 模式下 row_command 必须发到 Row
  // bus。
  bool row_command = false;
  // 是否占用 column/data command path。HBM dual bus 模式下 column_command
  // 必须发到 Column bus。
  bool column_command = false;
  // ACT/ACT1 类命令，用于 tFAW、RFM activation count 和 row policy 统计。
  bool activate = false;
  // PRE/PREpb/PREab 类命令。
  bool precharge = false;
  // LPDDR CAS_RD/CAS_WR，建立 WCK ready window 但不搬运数据。
  bool cas = false;
  // RD/WR/RDA/WRA 真正完成上层读写请求的数据命令。
  bool data = false;
  // REFab/REFpb/REFdb refresh 命令。
  bool refresh = false;
  // RFMab/RFMpb mitigation 命令。
  bool rfm = false;
  // 发出后会把请求推进 active_buffer 的命令，例如 ACT/ACT1。
  bool opening = false;
  // 发出后会关闭 row 的命令，例如 PREpb 或 RDA/WRA。
  bool closing = false;
  // 访问已打开 row 的命令，包括 RD/WR/RDA/WRA。
  bool accessing = false;
  // 命令作用于 channel 内全部 bank，例如 PREab/REFab/RFMab。
  bool all_bank = false;
  // 数据命令自带 auto-precharge。
  bool auto_precharge = false;
  // Mode register read/write/control command。
  bool mode_register = false;
  // WCK/PHY training or explicit synchronization command。
  bool training = false;
  // Power-state or DVFS control command。
  bool power = false;
  // RAS/ECC/link error handling command。
  bool ras = false;
};

// 返回命令语义元数据。所有模块判断命令类别都应走这个接口，避免重复硬编码。
const CommandMeta &command_meta(Command cmd);

// 根据 row policy 选择读/写终端命令。ClosedPage 直接用 auto-precharge；
// ClosedCap 的动态升级在 Controller::try_upgrade_row_policy_command() 中完成。
Command read_command_for(RowPolicyKind row_policy);
Command write_command_for(RowPolicyKind row_policy);

// LPDDR6 REFdb 的两个目标使用相同 BA、相邻的一对 BG（0<->1、2<->3）。
// 集中在语义层，避免 Controller、Executor 和离线 Validator 各维护一份不同表。
DecodedAddress lpddr_refdb_partner(const DramSpec &spec,
                                   const DecodedAddress &decoded);

} // namespace hbm_sim
