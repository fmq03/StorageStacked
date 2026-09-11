// DRAM 命令语义元数据：集中描述每条抽象命令属于 row/column/maintenance 等哪类。
// Controller、Validator、RowPolicy
// 不再各自硬编码命令类别，减少协议扩展时的漏改。
#include "hbm_sim/dram/semantics.hpp"

#include <algorithm>
#include <array>

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {
namespace {

constexpr std::array<CommandMeta, kCommandCount> kMeta = {{
    // 字段顺序见 CommandMeta：
    // 字段顺序：名称、行命令、列命令、激活、预充电、CAS、数据、
    // 刷新、RFM、打开、关闭、访问、全 bank、自动预充电。
    // mode_register, training, power, ras。
    //
    // 这张表是命令类别的唯一事实源。新增命令时，先补这里，再让 Controller/
    // Validator/RowPolicy 通过 command_meta() 获得一致语义。
    {"ACT", true, false, true, false, false, false, false, false, true, false,
     false, false, false, false, false, false, false},
    {"ACT1", true, false, true, false, false, false, false, false, true, false,
     false, false, false, false, false, false, false},
    {"ACT2", true, false, false, false, false, false, false, false, false,
     false, false, false, false, false, false, false, false},
    {"PRE", true, false, false, true, false, false, false, false, false, true,
     false, false, false, false, false, false, false},
    {"PREpb", true, false, false, true, false, false, false, false, false, true,
     false, false, false, false, false, false, false},
    {"PREab", true, false, false, true, false, false, false, false, false, true,
     false, true, false, false, false, false, false},
    {"CAS_RD", false, true, false, false, true, false, false, false, false,
     false, false, false, false, false, false, false, false},
    {"CAS_WR", false, true, false, false, true, false, false, false, false,
     false, false, false, false, false, false, false, false},
    {"RD", false, true, false, false, false, true, false, false, false, false,
     true, false, false, false, false, false, false},
    {"WR", false, true, false, false, false, true, false, false, false, false,
     true, false, false, false, false, false, false},
    {"RDA", false, true, false, false, false, true, false, false, false, true,
     true, false, true, false, false, false, false},
    {"WRA", false, true, false, false, false, true, false, false, false, true,
     true, false, true, false, false, false, false},
    {"REFab", true, false, false, false, false, false, true, false, false,
     false, false, true, false, false, false, false, false},
    {"REFpb", true, false, false, false, false, false, true, false, false,
     false, false, false, false, false, false, false, false},
    {"REFdb", true, false, false, false, false, false, true, false, false,
     false, false, false, false, false, false, false, false},
    {"RFMab", true, false, false, false, false, false, false, true, false,
     false, false, true, false, false, false, false, false},
    {"RFMpb", true, false, false, false, false, false, false, true, false,
     false, false, false, false, false, false, false, false},
    {"MRW", true, false, false, false, false, false, false, false, false, false,
     false, true, false, true, false, false, false},
    {"MRR", true, false, false, false, false, false, false, false, false, false,
     false, true, false, true, false, false, false},
    {"WCK_SYNC", false, true, false, false, true, false, false, false, false,
     false, false, false, false, false, true, false, false},
    {"WCK_TRAIN", true, false, false, false, false, false, false, false, false,
     false, false, true, false, false, true, false, false},
    {"DVFS", true, false, false, false, false, false, false, false, false,
     false, false, true, false, false, false, true, false},
    {"PDE", true, false, false, false, false, false, false, false, false, false,
     false, true, false, false, false, true, false},
    {"PDX", true, false, false, false, false, false, false, false, false, false,
     false, true, false, false, false, true, false},
    {"SREFEN", true, false, false, false, false, false, false, false, false,
     false, false, true, false, false, false, true, false},
    {"SREFEX", true, false, false, false, false, false, false, false, false,
     false, false, true, false, false, false, true, false},
    {"ECC_SCRUB", true, false, false, false, false, false, false, false, false,
     false, false, true, false, false, false, false, true},
    {"RAS_ERR", true, false, false, false, false, false, false, false, false,
     false, false, true, false, false, false, false, true},
    {"NOP", false, false, false, false, false, false, false, false, false,
     false, false, false, false, false, false, false, false},
}};

} // namespace

const CommandMeta &command_meta(Command cmd) {
  // enum class Command 的顺序和 kMeta 一一对应，command_index()
  // 是唯一的索引转换入口。
  return kMeta[command_index(cmd)];
}

Command read_command_for(RowPolicyKind row_policy) {
  // ClosedPage 策略用 auto-precharge 数据命令，让 row 在数据访问后关闭；
  // OpenPage/ClosedCap 默认先发普通 RD，ClosedCap 可能在 Controller 中再升级成
  // RDA。
  return row_policy == RowPolicyKind::ClosedPage ? Command::RDA : Command::RD;
}

Command write_command_for(RowPolicyKind row_policy) {
  // 写路径和读路径对称：ClosedPage 直接选择 WRA，其余策略从 WR 开始。
  return row_policy == RowPolicyKind::ClosedPage ? Command::WRA : Command::WR;
}

DecodedAddress lpddr_refdb_partner(const DramSpec &spec,
                                   const DecodedAddress &decoded) {
  DecodedAddress partner = decoded;
  const int groups = std::max(1, spec.org.bank_groups);
  if (groups < 2)
    return partner;

  // REFdb command truth table supplies BG for the first target and dBG for the
  // second while sharing BA. The baseline scheduler uses the deterministic
  // adjacent-BG pair table below; odd BG counts are rejected by config/model
  // validation because they cannot form a complete disjoint refresh schedule.
  partner.bank_group = decoded.bank_group ^ 1;
  if (partner.bank_group >= groups)
    partner.bank_group = decoded.bank_group;
  return partner;
}

} // namespace hbm_sim
