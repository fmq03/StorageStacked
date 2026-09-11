// CommandExecutor：执行一条已经通过调度和 timing gate 的 DRAM 命令。
// 这里集中更新 bank-local 状态、scope timing、WCK 窗口、RFM 计数和命令统计，
// 对应 Ramulator command lambda / state transition 的职责。
#include "hbm_sim/controller/executor.hpp"

#include <algorithm>

#include "hbm_sim/dram/semantics.hpp"

namespace hbm_sim {

CommandExecutor::CommandExecutor(const DramSpec &spec,
                                 std::vector<BankState> &banks,
                                 TimingEngine &timing_engine,
                                 RfmManager &rfm_manager, Stats &stats)
    : spec_(spec), banks_(banks), timing_engine_(timing_engine),
      rfm_manager_(rfm_manager), stats_(stats) {}

CommandExecutionResult CommandExecutor::issue(Request &req, Command issued,
                                              Cycle clk) {
  CommandExecutionResult result;
  // Executor 入口只接收已经通过 state_ok/timing_ok/bus_matches 的命令。
  // 因此这里不再做合法性判断，而是专注于副作用：bank 状态、scope gate、统计和
  // RFM。
  BankState &bank = banks_[req.decoded.flat_bank(spec_)];
  TimingScopeState &row = timing_engine_.row_state(spec_, req.decoded);
  TimingScopeState &col = timing_engine_.column_state(spec_, req.decoded);
  TimingScopeState &bg = timing_engine_.bank_group_state(spec_, req.decoded);
  TimingScopeState &wck = timing_engine_.wck_state(spec_, req.decoded);
  const Timing &t = spec_.timing;

  switch (issued) {
  case Command::ACT:
    // HBM-like 单阶段 activate：直接打开目标 row，并设置后续 RD/WR/PRE/ACT
    // gate。
    bank.open_row = req.decoded.row;
    // next_rd/next_wr 是 bank-local nRCD gate；row/bank-group scope 的 nRRD
    // 则写到 row.next_act/bg.next_act 和 TimingEngine constraint 中。
    bank.next_rd = clk + timing_delay(t.nRCDRD);
    bank.next_wr = clk + timing_delay(t.nRCDWR);
    bank.next_pre = clk + timing_delay(t.nRAS);
    bank.next_act = clk + timing_delay(t.nRC);
    row.next_row = clk + 1;
    row.next_act = clk + timing_delay(t.nRRDS);
    bg.next_act = clk + timing_delay(t.nRRDL);
    timing_engine_.record_activate(spec_, req.decoded, clk);
    result.rfm_command = rfm_manager_.on_activate(spec_, req.decoded, stats_);
    stats_.act++;
    break;
  case Command::ACT1:
    // LPDDR split activate 第一阶段：bank 进入 activating，row 尚不可访问。
    // owning request 必须后续发 ACT2；Controller 用 req.issued_first_activate
    // 追踪所有权。
    req.issued_first_activate = true;
    bank.activating = true;
    bank.next_act2 = clk + timing_delay(std::max(1, t.nAADMin));
    bank.act2_deadline = clk + timing_delay(std::max(t.nAADMin, t.nAADMax));
    // nRCD 从 ACT1 序列起点计时。ACT2 可以在 [nAADMin,nAADMax] 内任意
    // 合法 tick 发出，因而不能再假定它固定发生在 nAADMax 并扣减常数。
    bank.next_rd = clk + timing_delay(t.nRCDRD);
    bank.next_wr = clk + timing_delay(t.nRCDWR);
    bank.next_act = clk + timing_delay(t.nRC);
    bank.next_pre = clk + timing_delay(t.nRAS);
    row.next_row = clk + 1;
    row.next_act = clk + timing_delay(t.nRRDS);
    bg.next_act = clk + timing_delay(t.nRRDL);
    timing_engine_.record_activate(spec_, req.decoded, clk);
    result.rfm_command = rfm_manager_.on_activate(spec_, req.decoded, stats_);
    stats_.act1++;
    break;
  case Command::ACT2: {
    // LPDDR split activate 第二阶段：完成 row 打开。RD/WR 的绝对 gate 已在
    // ACT1 时写入，因此无论 ACT2 在窗口内何时发出，ACT1->RD/WR 都满足 nRCD。
    bank.activating = false;
    bank.open_row = req.decoded.row;
    bank.next_act2 = 0;
    bank.act2_deadline = 0;
    row.next_row = clk + 1;
    stats_.act2++;
    break;
  }
  case Command::PRE:
  case Command::PREPB:
    // PREpb 关闭目标 bank，并用 nRP 推迟下一次 ACT。open_row 清零后，
    // row policy 和 maintenance manager 都会把该 bank 视为 idle。
    bank.open_row = -1;
    bank.activating = false;
    bank.next_act = clk + timing_delay(t.nRP);
    row.next_row = clk + 1;
    if (issued == Command::PREPB) {
      stats_.prepb++;
    } else {
      stats_.pre++;
    }
    break;
  case Command::CASRD:
  case Command::CASWR:
    // Both CAS commands establish the same WCK window; only direction counts differ.
    col.next_col = clk + 1;
    wck.next_cas = clk + 1;
    wck.wck_ready_at = clk + timing_delay(std::max(1, t.nWCK2CK));
    wck.wck_active_until =
        clk + timing_delay(std::max(t.nWCKPST, t.nWCK2CK + 1));
    req.cas_sync_issued = true;
    if (issued == Command::CASRD) stats_.cas_rd++;
    else stats_.cas_wr++;
    stats_.wck_syncs++;
    break;
  case Command::RD:
  case Command::RDA:
    // RDA/WRA 是 auto-precharge 数据命令：数据命令发出后立即把 row 视为关闭，
    // 但下一次 ACT 仍要等到读恢复/写恢复和 nRP 路径结束。
    bank.next_any_col = clk + timing_delay(t.nCCDL);
    // bank.next_any_col 是同 bank 内保守列间隔；跨 bank-group 和 PC/SID
    // 的列间隔 还会通过 col/bg scope 与 timing_constraints 叠加。
    bank.next_rd = clk + timing_delay(t.nCCDL);
    bank.next_pre = std::max(bank.next_pre, clk + timing_delay(t.nRTP));
    if (issued == Command::RDA) {
      Cycle pre_at = clk + timing_delay(t.nRTP);
      bank.open_row = -1;
      bank.next_act = std::max(bank.next_act, pre_at + timing_delay(t.nRP));
    }
    col.next_col = clk + timing_delay(t.nCCDS);
    bg.next_col = clk + timing_delay(t.nCCDL);
    if (spec_.lpddr_family) {
      if (!req.cas_sync_issued) {
        // 正常 LPDDR 路径应先发 CAS_RD/CAS_WR。若 WCK 窗口仍有效，后续 row-hit
        // 数据命令可以复用该窗口，此时统计为 skip 而不是错误。
        stats_.wck_sync_skips++;
      }
      wck.wck_active_until = std::max(
          wck.wck_active_until, clk + timing_delay(t.nCL + t.nBL + t.nWCKPST));
      req.cas_sync_issued = false;
    }
    if (issued == Command::RDA) {
      stats_.rda++;
    } else {
      stats_.rd++;
    }
    break;
  case Command::WR:
  case Command::WRA:
    // 写路径的主要差异是 PRE gate 包含 nCWL + nBL + nWR，表达写数据和写恢复。
    bank.next_any_col = clk + timing_delay(t.nCCDL);
    bank.next_wr = clk + timing_delay(t.nCCDL);
    bank.next_pre =
        std::max(bank.next_pre, clk + timing_delay(t.nCWL + t.nBL + t.nWR));
    if (issued == Command::WRA) {
      Cycle pre_at = clk + timing_delay(t.nCWL + t.nBL + t.nWR);
      bank.open_row = -1;
      bank.next_act = std::max(bank.next_act, pre_at + timing_delay(t.nRP));
    }
    col.next_col = clk + timing_delay(t.nCCDS);
    bg.next_col = clk + timing_delay(t.nCCDL);
    if (spec_.lpddr_family) {
      if (!req.cas_sync_issued) {
        stats_.wck_sync_skips++;
      }
      wck.wck_active_until = std::max(
          wck.wck_active_until, clk + timing_delay(t.nCWL + t.nBL + t.nWCKPST));
      req.cas_sync_issued = false;
    }
    if (issued == Command::WRA) {
      stats_.wra++;
    } else {
      stats_.wr++;
    }
    break;
  case Command::REFPB:
    // Per-bank refresh 不打开 row，但会阻止目标 bank 在 nRFCpb 内再次 ACT。
    // 这里也记录一次 activate-window 事件，保守表达 refresh 对 array 的占用。
    bank.next_act = clk + timing_delay(t.nRFCpb);
    row.next_row = clk + 1;
    timing_engine_.record_activate(spec_, req.decoded, clk);
    stats_.refpb++;
    break;
  case Command::REFDB:
    // LPDDR6 dual-bank refresh 同时约束相同 BA、相邻 BG 表中的两个 bank。
    bank.next_act = clk + timing_delay(t.nRFCpb);
    {
      DecodedAddress partner = lpddr_refdb_partner(spec_, req.decoded);
      BankState &partner_bank = banks_[partner.flat_bank(spec_)];
      partner_bank.next_act =
          std::max(partner_bank.next_act, clk + timing_delay(t.nRFCpb));
    }
    row.next_row = clk + 1;
    timing_engine_.record_activate(spec_, req.decoded, clk);
    stats_.refdb++;
    break;
  case Command::RFMPB:
    // RFMpb 完成后，RfmManager 按 spec.rfm_decrement 递减对应 bank 的 RAA/PRAC
    // 计数。
    bank.next_act = clk + timing_delay(t.nRFMpb > 0 ? t.nRFMpb : t.nRFCpb);
    row.next_row = clk + 1;
    rfm_manager_.on_rfmpb(spec_, req.decoded, stats_);
    stats_.rfmpb++;
    break;
  case Command::PREAB:
    // PREab/REFab/RFMab 的 all-bank 是目标 rank 内的全部 bank，而不是
    // controller/channel 中的全部 rank。
    close_rank_banks(req.decoded, clk);
    row.next_row = clk + 1;
    stats_.preab++;
    break;
  case Command::REFAB:
    // REFab 不需要逐个修改 open_row，因为 state_ok/timing_ok 已要求目标 rank
    // idle； 它只需要把所有 bank 的 next_act 推迟到 nRFC 后。
    gate_rank_banks_after(req.decoded, clk + timing_delay(t.nRFC));
    row.next_row = clk + 1;
    stats_.refab++;
    break;
  case Command::RFMAB:
    // RFMab 和 REFab 类似，但完成后会递减所有 bank 的 RAA/PRAC 计数并清除
    // pending 标记。
    gate_rank_banks_after(req.decoded,
                          clk + timing_delay(t.nRFMab > 0 ? t.nRFMab : t.nRFC));
    rfm_manager_.on_rfmab(spec_, req.decoded, stats_);
    row.next_row = clk + 1;
    stats_.rfmab++;
    break;
  case Command::MRW:
    row.next_row = clk + timing_delay(std::max(1, t.nMRW));
    stats_.mrw++;
    stats_.mode_register_ops++;
    break;
  case Command::MRR:
    row.next_row = clk + timing_delay(std::max(1, t.nMRR));
    stats_.mrr++;
    stats_.mode_register_ops++;
    break;
  case Command::WCKSYNC:
    col.next_col = clk + 1;
    wck.next_cas = clk + timing_delay(std::max(1, t.nWCKSYNC));
    wck.wck_ready_at = clk + timing_delay(std::max(1, t.nWCKSYNC));
    wck.wck_active_until =
        std::max(wck.wck_active_until,
                 clk + timing_delay(std::max(t.nWCKPST, t.nWCKSYNC + 1)));
    stats_.wck_sync++;
    stats_.wck_syncs++;
    break;
  case Command::WCKTRAIN:
    row.next_row = clk + timing_delay(std::max(1, t.nWCKTRAIN));
    wck.next_cas = clk + timing_delay(std::max(1, t.nWCKTRAIN));
    wck.wck_ready_at = row.next_row;
    wck.wck_active_until = row.next_row + timing_delay(std::max(1, t.nWCKPST));
    stats_.wck_train++;
    stats_.wck_training_events++;
    break;
  case Command::DVFS:
    row.next_row = clk + timing_delay(std::max(1, t.nDVFS));
    col.next_col = row.next_row;
    wck.next_cas = row.next_row;
    stats_.dvfs++;
    stats_.dvfs_transitions++;
    break;
  case Command::PDE:
    row.next_row = clk + 1;
    stats_.pde++;
    break;
  case Command::PDX:
    row.next_row = clk + timing_delay(std::max(1, t.nPDEX));
    stats_.pdx++;
    break;
  case Command::SREFEN:
    row.next_row = clk + 1;
    stats_.srefen++;
    break;
  case Command::SREFEX:
    row.next_row = clk + timing_delay(std::max(1, t.nSREFEX));
    stats_.srefex++;
    break;
  case Command::ECCSCRUB:
    row.next_row = clk + timing_delay(std::max(1, t.nECCSCRUB));
    stats_.ecc_scrub++;
    stats_.ras_ecc_events++;
    break;
  case Command::RASERR:
    row.next_row = clk + timing_delay(std::max(1, t.nRASERR));
    col.next_col =
        std::max(col.next_col, clk + timing_delay(std::max(1, t.nLINKRETRY)));
    stats_.ras_err++;
    stats_.ras_ecc_events++;
    break;
  case Command::NOP:
    break;
  }

  // bank-local gate 只覆盖同 bank 状态；最后把表驱动 scope 约束统一交给
  // TimingEngine，例如 PC/SID/BG 范围的 nCCD/nRRD/nRFC 等。
  timing_engine_.apply_constraints(spec_, req.decoded, issued, clk);
  return result;
}

Cycle CommandExecutor::timing_delay(int cycles) const {
  if (cycles <= 0) {
    return 0;
  }
  return static_cast<Cycle>(cycles) *
         static_cast<Cycle>(std::max(1, spec_.tick_multiplier));
}

std::pair<int, int>
CommandExecutor::rank_bank_range(const DecodedAddress &decoded) const {
  const int banks_per_rank =
      std::max(1, spec_.org.bank_groups * spec_.org.banks_per_group);
  const int channel =
      std::clamp(decoded.channel, 0, std::max(1, spec_.org.channels) - 1);
  const int pc = std::clamp(decoded.pseudo_channel, 0,
                            std::max(1, spec_.org.pseudo_channels) - 1);
  const int sid = std::clamp(decoded.sid, 0, std::max(1, spec_.org.sids) - 1);
  const int rank =
      std::clamp(decoded.rank, 0, std::max(1, spec_.org.ranks) - 1);
  const int rank_index =
      ((channel * std::max(1, spec_.org.pseudo_channels) + pc) *
           std::max(1, spec_.org.sids) +
       sid) *
          std::max(1, spec_.org.ranks) +
      rank;
  const int begin = rank_index * banks_per_rank;
  return {begin,
          std::min(static_cast<int>(banks_.size()), begin + banks_per_rank)};
}

void CommandExecutor::close_rank_banks(const DecodedAddress &decoded,
                                       Cycle clk) {
  Cycle next_act =
      clk + timing_delay(spec_.timing.nRPab > 0 ? spec_.timing.nRPab
                                                : spec_.timing.nRP);
  const auto [begin, end] = rank_bank_range(decoded);
  for (int i = begin; i < end; i++) {
    BankState &bank = banks_[static_cast<std::size_t>(i)];
    bank.open_row = -1;
    bank.activating = false;
    bank.next_act = std::max(bank.next_act, next_act);
  }
}

void CommandExecutor::gate_rank_banks_after(const DecodedAddress &decoded,
                                            Cycle ready_at) {
  // REFab/RFMab 不改变 open_row，因为前置状态已经要求 idle；只需要为 rank 内
  // 每个 bank 写入恢复时间，阻止过早 ACT。
  const auto [begin, end] = rank_bank_range(decoded);
  for (int i = begin; i < end; i++) {
    BankState &bank = banks_[static_cast<std::size_t>(i)];
    bank.next_act = std::max(bank.next_act, ready_at);
  }
}

} // namespace hbm_sim
