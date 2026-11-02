// DRAM 规格派生层：把 profile 生成的 Timing 转成 timing table 和表驱动约束。
// 凡是来自 JEDEC/vendor/research/derived 的 timing，都应在 TimingTableEntry 中
// 留下 source，方便数值级对比时审计。
#include "hbm_sim/dram/spec.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "hbm_sim/dram/jedec.hpp"
#include "hbm_sim/dram/profiles.hpp"

namespace hbm_sim {
namespace {

TimingConstraint tc(TimingScope scope, std::vector<Command> preceding,
                    std::vector<Command> following, int latency,
                    std::string parameter, int window = 0,
                    std::string note = {}) {
  // tc() 是构造 timing constraint 的小工厂。约束语义是：
  // 在同一个 scope bucket 内，若 preceding 中任一命令于 cycle T 发出，
  // following 中每条命令最早只能在 T + latency 发出。
  //
  // window 目前只用于标记类似 tFAW 的窗口约束；真正 tFAW 路径由 TimingEngine
  // 的 recent_acts 队列实现，避免把“ready time”和“窗口计数”混成一种结构。
  TimingConstraint constraint;
  constraint.scope = scope;
  constraint.preceding = std::move(preceding);
  constraint.following = std::move(following);
  constraint.parameter = std::move(parameter);
  constraint.latency = latency;
  constraint.window = window;
  constraint.note = std::move(note);
  return constraint;
}

TimingConstraint tc_sibling(TimingScope scope, std::vector<Command> preceding,
                            std::vector<Command> following, int latency,
                            std::string parameter, std::string note = {}) {
  TimingConstraint constraint =
      tc(scope, std::move(preceding), std::move(following), latency,
         std::move(parameter), 0, std::move(note));
  constraint.sibling = true;
  return constraint;
}

TimingTableEntry tv(const char *name, int value, TimingValueSource source,
                    bool vendor_required = false, std::string note = {},
                    bool required_for_model = true) {
  // tv() 把一个 Timing 字段转成可导出的 timing-table 行。
  // 注意 TimingTable 不只是打印用途：validate_timing_table() 会用
  // required_for_model 和 vendor_required_for_numeric
  // 判断模型完整性与数值级校准状态。
  TimingTableEntry entry;
  entry.name = name;
  entry.value_nck = value;
  entry.source = source;
  entry.vendor_required_for_numeric = vendor_required;
  entry.required_for_model = required_for_model;
  entry.note = std::move(note);
  return entry;
}

const TimingSourceOverride *source_override_for(const DramSpec &spec,
                                                const std::string &name) {
  // source override 采用按名称查找，而不是在 Timing 结构里为每个字段再放
  // source， 是为了保持热路径的 Timing 简洁；source 只在配置审计/CSV
  // 导出时使用。
  auto it = std::find_if(spec.timing_source_overrides.begin(),
                         spec.timing_source_overrides.end(),
                         [&](const TimingSourceOverride &source_override) {
                           return source_override.name == name;
                         });
  return it == spec.timing_source_overrides.end() ? nullptr : &*it;
}

void apply_timing_source_overrides(const DramSpec &spec, TimingTable &table) {
  for (auto &entry : table.entries) {
    const TimingSourceOverride *source_override =
        source_override_for(spec, entry.name);
    if (source_override == nullptr) {
      continue;
    }
    entry.source = source_override->source;
    // 用户覆盖成 Vendor/JEDEC/Derived 后，strict 模式不再把该项视为缺口。
    // ResearchDefault 则保留 vendor_required 标志，表示数值级对比仍需补真实表。
    if (source_override->source == TimingValueSource::Vendor ||
        source_override->source == TimingValueSource::JEDEC ||
        source_override->source == TimingValueSource::Derived) {
      entry.vendor_required_for_numeric = false;
    }
    if (!source_override->note.empty()) {
      if (!entry.note.empty()) {
        entry.note += " ";
      }
      entry.note += source_override->note;
    }
  }
}

TimingTable build_timing_table(const DramSpec &spec) {
  const Timing &t = spec.timing;
  TimingTable table;
  table.preset_name = spec.name;

  const bool hbm = !spec.lpddr_family;
  const bool lpddr6 = spec.name == "LPDDR6";
  // source 标注规则：
  // - JEDEC: 当前 preset 中直接来自标准固定项或标准公式；
  // - Derived: 由其他 timing 算出，例如 nRC；
  // - ResearchDefault: 目前为了可运行放入的占位值，做真实器件对比前必须替换。
  auto jedec = TimingValueSource::JEDEC;
  auto derived = TimingValueSource::Derived;
  auto research = TimingValueSource::ResearchDefault;

  // 这张表覆盖 Controller 当前会读取的全部 Timing 字段。source 不是装饰：
  // strict 校验会用它提醒哪些值仍需 vendor/density/speed-bin 数据替换。
  table.entries = {
      tv("nBL", t.nBL, jedec),
      tv("nCL", t.nCL, hbm ? research : jedec, hbm,
         hbm ? "HBM RL/CWL 通常依赖器件 speed-bin。" : ""),
      tv("nCWL", t.nCWL, hbm ? research : jedec, hbm,
         hbm ? "HBM WL 需要目标器件表校准。" : ""),
      tv("nRCDRD", t.nRCDRD, hbm ? research : jedec, hbm),
      tv("nRCDWR", t.nRCDWR, hbm ? research : jedec, hbm),
      tv("nRP", t.nRP, hbm ? research : jedec, hbm),
      tv("nRAS", t.nRAS, hbm ? research : jedec, hbm),
      tv("nRC", t.nRC, derived),
      tv("nRTP", t.nRTP, hbm ? research : jedec, hbm),
      tv("nWR", t.nWR, hbm ? research : jedec, hbm),
      tv("nCCDS", t.nCCDS, jedec),
      tv("nCCDL", t.nCCDL, jedec),
      tv("nRRDS", t.nRRDS, hbm ? research : jedec, hbm),
      tv("nRRDL", t.nRRDL, hbm ? research : jedec, hbm),
      tv("nFAW", t.nFAW, hbm ? research : derived, hbm),
      tv("nAADMin", t.nAADMin, derived, false,
         "ACT2 earliest issue offset; project execution granularity.",
         spec.split_activate),
      tv("nAADMax", t.nAADMax, spec.split_activate ? jedec : derived, false,
         "JEDEC ACT1->ACT2 maximum deadline.", spec.split_activate),
      tv("nWCK2CK", t.nWCK2CK, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nWCKPST", t.nWCKPST, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nCAS", t.nCAS, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nCS", t.nCS, spec.lpddr_family ? jedec : derived, false,
         "CS pin waveform timing is metadata until pin-level CS scheduling is "
         "enabled.",
         false),
      tv("nPPD", t.nPPD, jedec),
      tv("nRPab", t.nRPab, hbm ? research : jedec, hbm),
      tv("nWTRS", t.nWTRS, hbm ? research : jedec, hbm),
      tv("nWTRL", t.nWTRL, hbm ? research : jedec, hbm),
      tv("nRTW", t.nRTW, hbm ? research : derived, hbm),
      tv("nCCDR", t.nCCDR, hbm ? research : derived, hbm,
         "跨 SID 列间隔通常需要 vendor 范围/器件表。"),
      tv("nRFC", t.nRFC, (hbm || lpddr6) ? jedec : research,
         spec.name == "LPDDR5"),
      tv("nRFCpb", t.nRFCpb, (hbm || lpddr6) ? jedec : research,
         spec.name == "LPDDR5"),
      tv("nRFMab", t.nRFMab, spec.supports_rfm ? jedec : derived, false, "",
         spec.supports_rfm),
      tv("nRFMpb", t.nRFMpb, spec.supports_rfm ? jedec : derived, false, "",
         spec.supports_rfm),
      tv("nRREFD", t.nRREFD, jedec),
      tv("nREFDB2ACT", t.nREFDB2ACT, lpddr6 ? jedec : derived, false,
         "LPDDR6 REFdb->ACT different-bank recovery.", lpddr6),
      tv("nREFDB2REFDBS", t.nREFDB2REFDBS, lpddr6 ? jedec : derived, false,
         "LPDDR6 REFdb->REFdb interval within the same refresh-row counter.", lpddr6),
      tv("nREFDB2REFDBL", t.nREFDB2REFDBL, lpddr6 ? jedec : derived, false,
         "LPDDR6 REFdb->REFdb interval across a refresh-row counter boundary.",
         lpddr6),
      tv("nREFI", t.nREFI, jedec),
      tv("nREFIpb", t.nREFIpb, jedec),
      tv("nMRW", t.nMRW, spec.lpddr_family ? jedec : research, hbm),
      tv("nMRR", t.nMRR, spec.lpddr_family ? jedec : research, hbm),
      tv("nWCKSYNC", t.nWCKSYNC, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nWCKTRAIN", t.nWCKTRAIN, spec.lpddr_family ? jedec : derived, false,
         "", spec.lpddr_family),
      tv("nDVFS", t.nDVFS, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nPDEX", t.nPDEX, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nSREFEX", t.nSREFEX, spec.lpddr_family ? jedec : derived, false, "",
         spec.lpddr_family),
      tv("nECCSCRUB", t.nECCSCRUB,
         spec.supports_ecc || spec.lpddr_link_ecc_enabled ? research : derived,
         spec.supports_ecc,
         "ECC scrub/repair latency is vendor/RAS-policy dependent.",
         spec.supports_ecc || spec.lpddr_link_ecc_enabled),
      tv("nRASERR", t.nRASERR,
         spec.hbm_link_retry_enabled || spec.lpddr_link_protection ? research
                                                                   : derived,
         spec.hbm_link_retry_enabled,
         "RAS/link retry recovery is vendor/RAS-policy dependent.",
         spec.hbm_link_retry_enabled || spec.lpddr_link_protection),
      tv("nLINKRETRY", t.nLINKRETRY,
         spec.hbm_link_retry_enabled || spec.lpddr_link_protection ? research
                                                                   : derived,
         spec.hbm_link_retry_enabled,
         "Link retry/replay timing is vendor/link-training dependent.",
         spec.hbm_link_retry_enabled || spec.lpddr_link_protection),
  };
  apply_timing_source_overrides(spec, table);
  return table;
}

std::vector<TimingConstraint> make_hbm_constraints(const Timing &t) {
  const int nccdr = t.nCCDR > 0 ? t.nCCDR : t.nCCDS;
  const int nrrefd = t.nRREFD > 0 ? t.nRREFD : t.nRFCpb;
  return {
      // HBM 行列双总线的核心：列命令在 pseudo-channel 范围内共享 column bus，
      // same SID 由 nCCDS/nCCDL 控制；different SID 另外使用 nCCDR。
      tc(TimingScope::PseudoChannel, {Command::RD, Command::RDA},
         {Command::RD, Command::RDA}, t.nBL, "nBL"),
      tc(TimingScope::PseudoChannel, {Command::WR, Command::WRA},
         {Command::WR, Command::WRA}, t.nBL, "nBL"),
      tc(TimingScope::PseudoChannel, {Command::RD, Command::RDA},
         {Command::WR, Command::WRA}, t.nRTW, "nRTW"),
      tc(TimingScope::PseudoChannel, {Command::WR, Command::WRA},
         {Command::RD, Command::RDA}, t.nCWL + t.nBL + t.nWTRS,
         "nCWL+nBL+nWTRS"),
      // activation 同时受短间隔 nRRDS 和四激活窗口 nFAW 控制。nFAW 使用
      // window=4
      // 标记，在线执行由 TimingEngine::record_activate/faw_ready 维护。
      tc(TimingScope::PseudoChannel, {Command::ACT}, {Command::ACT}, t.nRRDS,
         "nRRDS"),
      tc(TimingScope::PseudoChannel, {Command::ACT}, {Command::ACT}, t.nFAW,
         "nFAW", 4),
      tc(TimingScope::Sid, {Command::RD, Command::RDA},
         {Command::RD, Command::RDA}, t.nCCDS, "nCCDS"),
      tc_sibling(TimingScope::Sid, {Command::RD, Command::RDA},
                 {Command::RD, Command::RDA}, nccdr, "nCCDR",
                 "Only consecutive READs to different SID."),
      tc(TimingScope::Sid, {Command::WR, Command::WRA},
         {Command::WR, Command::WRA}, t.nCCDS, "nCCDS"),
      tc(TimingScope::BankGroup, {Command::RD, Command::RDA},
         {Command::RD, Command::RDA}, t.nCCDL, "nCCDL"),
      tc(TimingScope::BankGroup, {Command::WR, Command::WRA},
         {Command::WR, Command::WRA}, t.nCCDL, "nCCDL"),
      tc(TimingScope::BankGroup, {Command::WR, Command::WRA},
         {Command::RD, Command::RDA}, t.nCWL + t.nBL + t.nWTRL,
         "nCWL+nBL+nWTRL"),
      tc(TimingScope::BankGroup, {Command::ACT}, {Command::ACT}, t.nRRDL,
         "nRRDL"),
      tc(TimingScope::PseudoChannel,
         {Command::PRE, Command::PREPB, Command::PREAB},
         {Command::PRE, Command::PREPB, Command::PREAB}, t.nPPD, "nPPD"),
      tc(TimingScope::Bank, {Command::ACT}, {Command::ACT}, t.nRC, "nRC"),
      tc(TimingScope::Bank, {Command::ACT}, {Command::RD, Command::RDA},
         t.nRCDRD, "nRCDRD"),
      tc(TimingScope::Bank, {Command::ACT}, {Command::WR, Command::WRA},
         t.nRCDWR, "nRCDWR"),
      tc(TimingScope::Bank, {Command::ACT}, {Command::PREPB}, t.nRAS, "nRAS"),
      tc(TimingScope::Bank, {Command::PREPB}, {Command::ACT}, t.nRP, "nRP"),
      tc(TimingScope::Bank, {Command::RD}, {Command::PREPB}, t.nRTP, "nRTP"),
      tc(TimingScope::Bank, {Command::WR}, {Command::PREPB},
         t.nCWL + t.nBL + t.nWR, "nCWL+nBL+nWR"),
      // maintenance 规则按命令本身占用的 bank/PC scope 写入。Controller 会在
      // 状态不空闲时先插入 PREpb/PREab，因此这里主要表达 REF/RFM 后到 ACT
      // 的恢复。
      tc(TimingScope::Bank, {Command::REFPB}, {Command::ACT}, t.nRFCpb,
         "nRFCpb"),
      tc(TimingScope::PseudoChannel, {Command::REFPB}, {Command::ACT}, nrrefd,
         "nRREFD_or_nRFCpb"),
      tc(TimingScope::PseudoChannel, {Command::REFAB},
         {Command::ACT, Command::PREPB, Command::REFPB}, t.nRFC, "nRFC"),
      tc(TimingScope::Bank, {Command::RFMPB}, {Command::ACT}, t.nRFMpb,
         "nRFMpb"),
      tc(TimingScope::PseudoChannel, {Command::RFMAB},
         {Command::ACT, Command::PREPB, Command::RFMPB}, t.nRFMab, "nRFMab"),
      tc(TimingScope::Channel, {Command::MRW},
         {Command::ACT, Command::RD, Command::WR, Command::MRW, Command::MRR},
         t.nMRW, "nMRW"),
      tc(TimingScope::Channel, {Command::MRR},
         {Command::ACT, Command::RD, Command::WR, Command::MRW, Command::MRR},
         t.nMRR, "nMRR"),
      tc(TimingScope::Channel, {Command::ECCSCRUB, Command::RASERR},
         {Command::ACT, Command::REFPB, Command::RFMPB, Command::MRW,
          Command::MRR},
         std::max(t.nECCSCRUB, t.nRASERR), "max(nECCSCRUB,nRASERR)"),
      tc(TimingScope::Channel, {Command::RASERR}, {Command::RD, Command::WR},
         t.nLINKRETRY, "nLINKRETRY"),
  };
}

std::vector<TimingConstraint> make_lpddr_constraints(const Timing &t) {
  const int nrefdb2act =
      t.nREFDB2ACT > 0 ? t.nREFDB2ACT : (t.nRREFD > 0 ? t.nRREFD : t.nRRDS);
  return {
      // LPDDR 当前使用统一命令总线，因此 RD/WR 首先在 Channel scope 上受 burst
      // 间隔限制；LPDDR6 preset 会把部分 scope 调到 pseudo-channel 来表达
      // subchannel。
      tc(TimingScope::Channel, {Command::RD, Command::RDA},
         {Command::RD, Command::RDA}, t.nBL, "nBL"),
      tc(TimingScope::Channel, {Command::WR, Command::WRA},
         {Command::WR, Command::WRA}, t.nBL, "nBL"),
      // CAS_RD/CAS_WR 是 WCK 同步命令，真正数据命令还需要等待 nCAS/nWCK2CK。
      // 这里的 CAS->RD/WR 约束与 TimingEngine 中的 WCK ready window
      // 是两道独立检查。
      tc(TimingScope::Bank, {Command::CASRD}, {Command::RD, Command::RDA},
         std::max(0, t.nCAS), "nCAS"),
      tc(TimingScope::Bank, {Command::CASWR}, {Command::WR, Command::WRA},
         std::max(0, t.nCAS), "nCAS"),
      tc(TimingScope::Rank, {Command::RD, Command::RDA},
         {Command::RD, Command::RDA}, t.nCCDS, "nCCDS"),
      tc(TimingScope::Rank, {Command::WR, Command::WRA},
         {Command::WR, Command::WRA}, t.nCCDS, "nCCDS"),
      tc(TimingScope::Rank, {Command::RD, Command::RDA},
         {Command::WR, Command::WRA}, t.nCL + t.nBL + 2 - t.nCWL,
         "nCL+nBL+2-nCWL"),
      tc(TimingScope::Rank, {Command::WR, Command::WRA},
         {Command::RD, Command::RDA}, t.nCWL + t.nBL + t.nWTRS,
         "nCWL+nBL+nWTRS"),
      tc(TimingScope::Rank, {Command::ACT1}, {Command::ACT1}, t.nRRDS, "nRRDS"),
      tc(TimingScope::Rank, {Command::ACT1}, {Command::ACT1}, t.nFAW, "nFAW",
         4),
      tc(TimingScope::BankGroup, {Command::RD, Command::RDA},
         {Command::RD, Command::RDA}, t.nCCDL, "nCCDL"),
      tc(TimingScope::BankGroup, {Command::WR, Command::WRA},
         {Command::WR, Command::WRA}, t.nCCDL, "nCCDL"),
      tc(TimingScope::BankGroup, {Command::WR, Command::WRA},
         {Command::RD, Command::RDA}, t.nCWL + t.nBL + t.nWTRL,
         "nCWL+nBL+nWTRL"),
      tc(TimingScope::BankGroup, {Command::ACT1}, {Command::ACT1}, t.nRRDL,
         "nRRDL"),
      tc(TimingScope::Channel, {Command::PRE, Command::PREPB, Command::PREAB},
         {Command::PRE, Command::PREPB, Command::PREAB}, t.nPPD, "nPPD"),
      tc(TimingScope::Bank, {Command::ACT1}, {Command::ACT1}, t.nRC, "nRC"),
      tc(TimingScope::Bank, {Command::ACT1}, {Command::RD, Command::RDA},
         t.nRCDRD, "nRCDRD"),
      tc(TimingScope::Bank, {Command::ACT1}, {Command::WR, Command::WRA},
         t.nRCDWR, "nRCDWR"),
      tc(TimingScope::Bank, {Command::ACT1}, {Command::PREPB}, t.nRAS, "nRAS"),
      tc(TimingScope::Bank, {Command::PREPB}, {Command::ACT1}, t.nRP, "nRP"),
      tc(TimingScope::Bank, {Command::PREPB}, {Command::REFDB}, t.nRP, "nRP"),
      tc(TimingScope::Rank, {Command::PREAB}, {Command::REFDB}, t.nRPab, "nRPab"),
      tc(TimingScope::Bank, {Command::RD}, {Command::PREPB}, t.nRTP, "nRTP"),
      tc(TimingScope::Bank, {Command::WR}, {Command::PREPB},
         t.nCWL + t.nBL + t.nWR, "nCWL+nBL+nWR"),
      // LPDDR6 REFdb 用 REFDB 命令表示 dual-bank refresh。bank scope 约束目标
      // bank 的恢复，PseudoChannel scope 约束同一 subchannel 的 refresh 间隔。
      tc(TimingScope::Bank, {Command::REFPB}, {Command::ACT1}, t.nRFCpb,
         "nRFCpb"),
      tc(TimingScope::Bank, {Command::REFDB}, {Command::ACT1}, t.nRFCpb,
         "nRFCpb"),
      tc(TimingScope::Bank, {Command::RFMPB}, {Command::ACT1},
         t.nRFMpb > 0 ? t.nRFMpb : t.nRFCpb, "nRFMpb_or_nRFCpb"),
      tc(TimingScope::PseudoChannel, {Command::REFDB}, {Command::ACT1},
         nrefdb2act, "nREFDB2ACT"),
      // S/L selection depends on the refresh-row bank counter, not the
      // addressed bank. TimingEngine and the independent validator replay it.
      tc(TimingScope::Bank, {Command::REFDB}, {Command::REFDB}, t.nRFCpb,
         "nRFCpb"),
      tc(TimingScope::Rank, {Command::REFDB}, {Command::REFAB}, t.nRFCpb,
         "nRFCpb"),
      tc(TimingScope::Rank, {Command::REFAB}, {Command::REFDB, Command::REFAB},
         t.nRFC, "nRFC"),
      tc(TimingScope::PseudoChannel, {Command::RFMPB}, {Command::ACT1},
         nrefdb2act, "nREFDB2ACT"),
      tc(TimingScope::PseudoChannel, {Command::RFMPB}, {Command::RFMPB},
         t.nRRDL, "nRRDL"),
      tc(TimingScope::Rank, {Command::REFAB}, {Command::ACT1, Command::PREAB},
         t.nRFC, "nRFC"),
      tc(TimingScope::Rank, {Command::RFMAB},
         {Command::ACT1, Command::PREAB, Command::RFMPB},
         t.nRFMab > 0 ? t.nRFMab : t.nRFC, "nRFMab_or_nRFC"),
      tc(TimingScope::Channel, {Command::MRW},
         {Command::ACT1, Command::CASRD, Command::CASWR, Command::MRW,
          Command::MRR},
         t.nMRW, "nMRW"),
      tc(TimingScope::Channel, {Command::MRR},
         {Command::ACT1, Command::CASRD, Command::CASWR, Command::MRW,
          Command::MRR},
         t.nMRR, "nMRR"),
      tc(TimingScope::PseudoChannel, {Command::WCKSYNC},
         {Command::RD, Command::WR}, t.nWCKSYNC, "nWCKSYNC"),
      tc(TimingScope::Channel, {Command::WCKTRAIN},
         {Command::ACT1, Command::CASRD, Command::CASWR}, t.nWCKTRAIN,
         "nWCKTRAIN"),
      tc(TimingScope::Channel, {Command::DVFS},
         {Command::ACT1, Command::CASRD, Command::CASWR, Command::MRW}, t.nDVFS,
         "nDVFS"),
      tc(TimingScope::Channel, {Command::PDE, Command::PDX},
         {Command::ACT1, Command::CASRD, Command::CASWR}, t.nPDEX, "nPDEX"),
      tc(TimingScope::Channel, {Command::SREFEN, Command::SREFEX},
         {Command::ACT1, Command::REFDB, Command::RFMPB}, t.nSREFEX, "nSREFEX"),
      tc(TimingScope::Channel, {Command::ECCSCRUB, Command::RASERR},
         {Command::ACT1, Command::REFDB, Command::RFMPB, Command::CASRD,
          Command::CASWR},
         std::max(t.nECCSCRUB, t.nRASERR), "max(nECCSCRUB,nRASERR)"),
  };
}

} // namespace

DramSpec make_spec(const std::string &name) {
  DramSpec spec = make_spec_draft(name);
  apply_standard_timing_profile(spec);
  finalize_spec(spec);
  return spec;
}

double density_gbit_from_geometry(double capacity_bytes, bool lpddr_family,
                                 int stack_height, int channels,
                                 int subchannels, int ranks) {
  if (!std::isfinite(capacity_bytes) || capacity_bytes <= 0 ||
      (lpddr_family ? (channels <= 0 || subchannels <= 0 || ranks <= 0)
                    : stack_height <= 0)) {
    throw std::invalid_argument("invalid geometry for density calculation");
  }
  const double count = lpddr_family
      ? static_cast<double>(channels) * subchannels * ranks : stack_height;
  return capacity_bytes / 134217728.0 / count;
}

void validate_spec(const DramSpec &spec) {
  // 这些检查不能只留在 CLI：DramSpec 也是公开库接口，SystemC/UCIe
  // 适配器可以绕过命令行直接构造规格。未实现的模式若在这里静默通过，
  // 后续会退化成另一种可执行语义并生成“看似合理”的错误结果。
  if (spec.org.channels <= 0 || spec.org.pseudo_channels <= 0 ||
      spec.org.sids <= 0 || spec.org.ranks <= 0 ||
      spec.org.bank_groups <= 0 || spec.org.banks_per_group <= 0 ||
      spec.org.rows <= 0 || spec.org.columns <= 0 ||
      spec.org.line_size <= 0) {
    throw std::invalid_argument("all DRAM organization dimensions must be > 0");
  }
  if (spec.org.dram_transaction_bytes < 0 ||
      (spec.org.dram_transaction_bytes > 0 &&
       spec.org.line_size % spec.org.dram_transaction_bytes != 0)) {
    throw std::invalid_argument(
        "dram_transaction_bytes must be 0 or a positive divisor of line_size");
  }
  if (spec.data_rate_mbps <= 0 || spec.data_bus_bits <= 0 ||
      spec.internal_prefetch_size <= 0 || spec.speed_bin_mbps <= 0 ||
      spec.density_gb <= 0 || !std::isfinite(spec.density_gb) || spec.timing.tCK_ps <= 0.0 ||
      !std::isfinite(spec.timing.tCK_ps) || spec.tick_multiplier <= 0) {
    throw std::invalid_argument(
        "data rate, bus width, prefetch, speed bin, density, tCK and "
        "tick_multiplier must be > 0");
  }
  if ((!spec.lpddr_family && spec.stack_height <= 0) ||
      (spec.lpddr_family && spec.stack_height < 0)) {
    throw std::invalid_argument(
        "HBM stack_height must be > 0; LPDDR stack_height must be >= 0");
  }
  const auto capacity = spec.addressable_capacity_bytes();
  if (capacity == 0)
    throw std::invalid_argument("DRAM geometry capacity overflow");
  if (spec.density_reference_channels < 0 ||
      (spec.density_reference_channels > 0 && spec.org.channels != 1))
    throw std::invalid_argument("invalid channel-local density reference");
  const int reference_channels = spec.density_reference_channels > 0
      ? spec.density_reference_channels : spec.org.channels;
  const double reference_capacity = static_cast<double>(capacity) *
      reference_channels / spec.org.channels;
  const double density = density_gbit_from_geometry(
      reference_capacity, spec.lpddr_family, spec.stack_height,
      reference_channels, spec.org.pseudo_channels, spec.org.ranks);
  if (std::abs(spec.density_gb - density) > 1e-9 * std::max(1.0, density))
    throw std::invalid_argument("density_gb conflicts with geometry-derived density; use the config model resolver after changing organization");
  if (spec.speed_bin_mbps != spec.data_rate_mbps)
    throw std::invalid_argument("speed_bin_mbps must equal data_rate_mbps");
  const int ratio = spec.standard == DramStandard::Lpddr5 ? spec.lpddr_wck_ratio : 2;
  if ((spec.standard == DramStandard::Lpddr5 && ratio != 2 && ratio != 4) ||
      (spec.standard == DramStandard::Lpddr6 && spec.lpddr_wck_ratio != 2))
    throw std::invalid_argument("unsupported LPDDR WCK:CK ratio");
  const double expected_tck = 2000000.0 * ratio / spec.data_rate_mbps;
  if (std::abs(spec.timing.tCK_ps - expected_tck) > 0.5 + 1e-9)
    throw std::invalid_argument("tCK_ps conflicts with data_rate_mbps and protocol clock ratio (maximum rounding error 0.5 ps)");
  const std::initializer_list<std::pair<const char *, int>> non_negative = {
      {"metadata_bits_per_request", spec.metadata_bits_per_request},
      {"ecc_bits_per_request", spec.ecc_bits_per_request},
      {"hbm_link_crc_bits_per_request", spec.hbm_link_crc_bits_per_request},
      {"hbm_ras_metadata_bits_per_request",
       spec.hbm_ras_metadata_bits_per_request},
      {"hbm_ecc_bits_per_request", spec.hbm_ecc_bits_per_request},
      {"lpddr_dbi_bits_per_request", spec.lpddr_dbi_bits_per_request},
      {"lpddr_link_ecc_bits_per_request",
       spec.lpddr_link_ecc_bits_per_request},
      {"lpddr_ca_parity_bits_per_command",
       spec.lpddr_ca_parity_bits_per_command},
      {"low_power_entry_cycles", spec.low_power_entry_cycles},
      {"low_power_exit_cycles", spec.low_power_exit_cycles},
      {"self_refresh_exit_cycles", spec.self_refresh_exit_cycles},
      {"refresh_postpone_limit", spec.refresh_postpone_limit},
      {"refresh_pullin_limit", spec.refresh_pullin_limit},
      {"refresh_credit_limit", spec.refresh_credit_limit},
  };
  for (const auto &[name, value] : non_negative) {
    if (value < 0) {
      throw std::invalid_argument(std::string(name) + " must be >= 0");
    }
  }
  if (spec.refresh_high_temp_multiplier <= 0) {
    throw std::invalid_argument("refresh_high_temp_multiplier must be > 0");
  }
  if (spec.lpddr_family && spec.lpddr_wck_ratio <= 0) {
    throw std::invalid_argument("lpddr_wck_ratio must be > 0");
  }
  if (spec.supports_rfm &&
      (spec.rfm_act_threshold <= 0 || spec.rfm_decrement <= 0)) {
    throw std::invalid_argument(
        "RFM-enabled models require rfm_act_threshold and rfm_decrement > 0");
  }
  if (spec.lpddr_wck_mode == LpddrWckMode::BurstSync) {
    throw std::invalid_argument(
        "lpddr_wck_mode=burst_sync is reserved and has no executable "
        "semantics");
  }
  if (spec.split_activate) {
    if (spec.timing.nAADMin <= 0) {
      throw std::invalid_argument("nAADMin must be > 0 for split activate");
    }
    if (spec.timing.nAADMax < spec.timing.nAADMin) {
      throw std::invalid_argument(
          "nAADMax must be >= nAADMin for split activate");
    }
  }
  if (spec.lpddr_dual_bank_refresh &&
      (spec.org.bank_groups < 2 || (spec.org.bank_groups % 2) != 0)) {
    throw std::invalid_argument(
        "LPDDR REFdb adjacent-BG pair table requires an even bank_groups >= "
        "2");
  }
  const auto row_cycle = static_cast<std::int64_t>(spec.timing.nRAS) + spec.timing.nRP;
  if (spec.timing.nRAS < 0 || spec.timing.nRP < 0 || spec.timing.nRC < row_cycle) {
    throw std::invalid_argument("nRC must be >= nRAS + nRP (non-negative model row timings)");
  }
}

void finalize_spec(DramSpec &spec) {
  // profile/配置覆盖全部结束后统一派生约束和审计表。Controller 不缓存
  // constraint 指针，因此这里可以直接覆盖 vector。
  validate_spec(spec);
  spec.timing_constraints = spec.lpddr_family
                                ? make_lpddr_constraints(spec.timing)
                                : make_hbm_constraints(spec.timing);
  refresh_timing_table(spec);
}

void refresh_timing_constraints(DramSpec &spec) {
  // 兼容现有直接修改 DramSpec 的调用方；新构建链应优先在末尾调用
  // finalize_spec()。
  finalize_spec(spec);
}

void refresh_timing_table(DramSpec &spec) {
  // Timing table 是 DramSpec 当前状态的快照；每次用户覆盖 timing/source
  // 后都要重建， 否则 CLI 输出和 --dump-timing-table 会显示旧值。
  spec.timing_table = build_timing_table(spec);
}

void set_timing_source(DramSpec &spec, const std::string &name,
                       TimingValueSource source, std::string note) {
  auto it = std::find_if(spec.timing_source_overrides.begin(),
                         spec.timing_source_overrides.end(),
                         [&](const TimingSourceOverride &source_override) {
                           return source_override.name == name;
                         });
  if (it == spec.timing_source_overrides.end()) {
    spec.timing_source_overrides.push_back(
        TimingSourceOverride{name, source, std::move(note)});
  } else {
    it->source = source;
    it->note = std::move(note);
  }
}

std::vector<std::string> validate_timing_table(const DramSpec &spec,
                                               bool require_vendor_values) {
  std::vector<std::string> errors;
  if (spec.timing_table.entries.empty()) {
    errors.push_back("timing table is empty");
    return errors;
  }
  for (const auto &entry : spec.timing_table.entries) {
    // required_for_model 检查“当前代码会不会读到无效 timing”。这比完整 JEDEC
    // 检查更窄，但能保证模拟器内部状态机不会因缺失值跑出无意义结果。
    if (entry.required_for_model && entry.value_nck < 0) {
      errors.push_back(entry.name + " is required but negative");
    }
    // require_vendor_values 是数值级对比开关：如果 HBM4 行时序仍是研究默认值，
    // strict 模式会直接失败，提醒用户必须补目标器件表。
    if (require_vendor_values && entry.vendor_required_for_numeric) {
      errors.push_back(entry.name +
                       " still needs JEDEC/vendor calibrated value");
    }
  }
  return errors;
}

} // namespace hbm_sim
