#pragma once
#include "hbm_sim/config/parse.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include <ostream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <string_view>
#include <variant>
#include <vector>

namespace hbm_sim::config {
// Scalars have one typed mapping for assignment and export. Coupled formulas,
// enum side effects and timing provenance remain in the model resolver.
using ModelFieldValue = std::variant<int, double, bool, std::string>;
struct ModelField {
  std::vector<std::string_view> keys;
  void (*set)(DramSpec&, const std::string&);
  ModelFieldValue (*get)(const DramSpec&);
  bool profile_selector = false;
};
inline const std::vector<ModelField>& model_fields() {
  static const std::vector<ModelField> fields = {
    {{"timing_profile"},
      [](DramSpec& spec, const std::string& value) { spec.timing_profile = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.timing_profile; }, true},
    {{"vendor_profile"},
      [](DramSpec& spec, const std::string& value) { spec.vendor_profile = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.vendor_profile; }, true},
    {{"mode_profile"},
      [](DramSpec& spec, const std::string& value) { spec.mode_profile = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.mode_profile; }, true},
    {{"speed_bin_mbps"},
      [](DramSpec& spec, const std::string& value) { spec.speed_bin_mbps = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.speed_bin_mbps; }, true},
    {{"density_gb"},
      [](DramSpec& spec, const std::string& value) { spec.density_gb = parse_double(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.density_gb; }, true},
    {{"stack_height"},
      [](DramSpec& spec, const std::string& value) { spec.stack_height = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.stack_height; }, true},
    {{"data_rate_mbps"},
      [](DramSpec& spec, const std::string& value) { spec.data_rate_mbps = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.data_rate_mbps; }, true},
    {{"data_bus_bits"},
      [](DramSpec& spec, const std::string& value) { spec.data_bus_bits = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.data_bus_bits; }},
    {{"prefetch_size", "internal_prefetch_size"},
      [](DramSpec& spec, const std::string& value) { spec.internal_prefetch_size = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.internal_prefetch_size; }},
    {{"dfi_phase_count"},
      [](DramSpec& spec, const std::string& value) { spec.dfi_phase_count = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.dfi_phase_count; }},
    {{"dfi_data_lane_bytes"},
      [](DramSpec& spec, const std::string& value) { spec.dfi_data_lane_bytes = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.dfi_data_lane_bytes; }},
    {{"dfi_read_latency_nck"},
      [](DramSpec& spec, const std::string& value) { spec.dfi_read_latency_nck = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.dfi_read_latency_nck; }},
    {{"dfi_write_latency_nck"},
      [](DramSpec& spec, const std::string& value) { spec.dfi_write_latency_nck = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.dfi_write_latency_nck; }},
    {{"tick_multiplier"},
      [](DramSpec& spec, const std::string& value) { spec.tick_multiplier = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.tick_multiplier; }},
    {{"full_stack_model"},
      [](DramSpec& spec, const std::string& value) { spec.full_stack_model = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.full_stack_model; }},
    {{"supports_refresh"},
      [](DramSpec& spec, const std::string& value) { spec.supports_refresh = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.supports_refresh; }},
    {{"lpddr_dual_bank_refresh"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_dual_bank_refresh = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_dual_bank_refresh; }},
    {{"supports_rfm"},
      [](DramSpec& spec, const std::string& value) { spec.supports_rfm = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.supports_rfm; }},
    {{"supports_ecc"},
      [](DramSpec& spec, const std::string& value) { spec.supports_ecc = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.supports_ecc; }},
    {{"hbm_full_32_channel_stack"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_full_32_channel_stack = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_full_32_channel_stack; }},
    {{"hbm_pc_interleave"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_pc_interleave = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_pc_interleave; }},
    {{"hbm_edge_pairing"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_edge_pairing = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_edge_pairing; }},
    {{"hbm_strict_edge_pairing"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_strict_edge_pairing = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_strict_edge_pairing; }},
    {{"hbm_edge_pairing_matrix"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_edge_pairing_matrix = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_edge_pairing_matrix; }},
    {{"hbm_sid_mapping"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_sid_mapping = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_sid_mapping; }},
    {{"hbm_ecc_scheme"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_ecc_scheme = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_ecc_scheme; }},
    {{"hbm_ras_policy"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_ras_policy = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_ras_policy; }},
    {{"hbm_link_crc_mode"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_link_crc_mode = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_link_crc_mode; }, true},
    {{"hbm_link_retry_enabled"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_link_retry_enabled = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_link_retry_enabled; }, true},
    {{"hbm_link_crc_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_link_crc_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_link_crc_bits_per_request; }},
    {{"hbm_ras_metadata_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_ras_metadata_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_ras_metadata_bits_per_request; }},
    {{"hbm_ecc_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.hbm_ecc_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.hbm_ecc_bits_per_request; }},
    {{"rfm_act_threshold"},
      [](DramSpec& spec, const std::string& value) { spec.rfm_act_threshold = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.rfm_act_threshold; }},
    {{"rfm_decrement"},
      [](DramSpec& spec, const std::string& value) { spec.rfm_decrement = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.rfm_decrement; }},
    {{"lpddr_link_protection"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_link_protection = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_link_protection; }, true},
    {{"lpddr_low_data_rate_mbps"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_low_data_rate_mbps = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_low_data_rate_mbps; }, true},
    {{"lpddr_wck_ratio"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_wck_ratio = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_wck_ratio; }},
    {{"lpddr_mode_register_profile"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_mode_register_profile = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_mode_register_profile; }},
    {{"lpddr_wck_training_mode"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_wck_training_mode = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_wck_training_mode; }},
    {{"lpddr_dvfs_transition_policy"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_dvfs_transition_policy = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_dvfs_transition_policy; }},
    {{"lpddr_link_protection_mode"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_link_protection_mode = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_link_protection_mode; }},
    {{"lpddr_low_power_state_policy"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_low_power_state_policy = value; },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_low_power_state_policy; }},
    {{"lpddr_wck_training_required"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_wck_training_required = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_wck_training_required; }, true},
    {{"lpddr_dbi_enabled"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_dbi_enabled = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_dbi_enabled; }, true},
    {{"lpddr_link_ecc_enabled"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_link_ecc_enabled = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_link_ecc_enabled; }, true},
    {{"lpddr_ca_parity_enabled"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_ca_parity_enabled = parse_bool(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_ca_parity_enabled; }, true},
    {{"lpddr_dbi_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_dbi_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_dbi_bits_per_request; }},
    {{"lpddr_link_ecc_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_link_ecc_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_link_ecc_bits_per_request; }},
    {{"lpddr_ca_parity_bits_per_command"},
      [](DramSpec& spec, const std::string& value) { spec.lpddr_ca_parity_bits_per_command = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.lpddr_ca_parity_bits_per_command; }},
    {{"low_power_entry_cycles"},
      [](DramSpec& spec, const std::string& value) { spec.low_power_entry_cycles = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.low_power_entry_cycles; }},
    {{"low_power_exit_cycles"},
      [](DramSpec& spec, const std::string& value) { spec.low_power_exit_cycles = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.low_power_exit_cycles; }, true},
    {{"self_refresh_exit_cycles"},
      [](DramSpec& spec, const std::string& value) { spec.self_refresh_exit_cycles = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.self_refresh_exit_cycles; }, true},
    {{"refresh_postpone_limit"},
      [](DramSpec& spec, const std::string& value) { spec.refresh_postpone_limit = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.refresh_postpone_limit; }},
    {{"refresh_pullin_limit"},
      [](DramSpec& spec, const std::string& value) { spec.refresh_pullin_limit = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.refresh_pullin_limit; }},
    {{"refresh_credit_limit"},
      [](DramSpec& spec, const std::string& value) { spec.refresh_credit_limit = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.refresh_credit_limit; }},
    {{"refresh_high_temp_multiplier"},
      [](DramSpec& spec, const std::string& value) { spec.refresh_high_temp_multiplier = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.refresh_high_temp_multiplier; }},
    {{"metadata_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.metadata_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.metadata_bits_per_request; }},
    {{"ecc_bits_per_request"},
      [](DramSpec& spec, const std::string& value) { spec.ecc_bits_per_request = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.ecc_bits_per_request; }},
    {{"channels"},
      [](DramSpec& spec, const std::string& value) { spec.org.channels = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.channels; }},
    {{"pseudo_channels"},
      [](DramSpec& spec, const std::string& value) { spec.org.pseudo_channels = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.pseudo_channels; }},
    {{"sids"},
      [](DramSpec& spec, const std::string& value) { spec.org.sids = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.sids; }},
    {{"ranks"},
      [](DramSpec& spec, const std::string& value) { spec.org.ranks = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.ranks; }},
    {{"bank_groups"},
      [](DramSpec& spec, const std::string& value) { spec.org.bank_groups = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.bank_groups; }},
    {{"banks_per_group"},
      [](DramSpec& spec, const std::string& value) { spec.org.banks_per_group = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.banks_per_group; }},
    {{"rows"},
      [](DramSpec& spec, const std::string& value) { spec.org.rows = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.rows; }},
    {{"columns"},
      [](DramSpec& spec, const std::string& value) { spec.org.columns = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.columns; }},
    {{"line_size"},
      [](DramSpec& spec, const std::string& value) { spec.org.line_size = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.line_size; }},
    {{"dram_transaction_bytes", "transaction_size"},
      [](DramSpec& spec, const std::string& value) { spec.org.dram_transaction_bytes = parse_int(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.org.dram_transaction_bytes; }},
    {{"tck_ps"},
      [](DramSpec& spec, const std::string& value) { spec.timing.tCK_ps = parse_double(value); },
      [](const DramSpec& spec) -> ModelFieldValue { return spec.timing.tCK_ps; }},
  };
  return fields;
}
inline const ModelField* find_model_field(std::string_view key) {
  for (const auto& field : model_fields())
    for (auto alias : field.keys) if (alias == key) return &field;
  return nullptr;
}
inline bool assign_model_field(DramSpec& spec, const std::string& key,
                               const std::string& value) {
  if (const auto* field = find_model_field(key)) {
    field->set(spec, value);
    return true;
  }
  return false;
}
inline ModelFieldValue model_field_value(const DramSpec& spec, std::string_view key) {
  const auto* field = find_model_field(key);
  if (!field) throw std::invalid_argument("unknown model field: " + std::string(key));
  // A resolved snapshot must contain the effective transaction size, not auto=0.
  if (field->keys.front() == "dram_transaction_bytes")
    return spec.transaction_bytes();
  return field->get(spec);
}
inline void write_model_field(std::ostream& out, const DramSpec& spec,
                              std::string_view key) {
  out << key << " = ";
  std::visit([&](const auto& value) { out << value; }, model_field_value(spec, key));
  out << '\n';
}
inline std::string model_field_text(const DramSpec& spec, std::string_view key) {
  std::ostringstream out;
  out << std::boolalpha << std::setprecision(std::numeric_limits<double>::max_digits10);
  std::visit([&](const auto& value) { out << value; }, model_field_value(spec, key));
  return out.str();
}

// Each timing member owns its canonical provenance name and all input aliases.
// The suffix fixes the input unit; nCK values stay integral.
struct TimingField {
  std::string_view name;
  int Timing::*member;
  std::vector<std::string_view> keys;
};
inline const std::vector<TimingField>& timing_fields() {
  static const std::vector<TimingField> fields = {
    {"nBL", &Timing::nBL, {"nbl"}},
    {"nCL", &Timing::nCL, {"ncl"}},
    {"nCWL", &Timing::nCWL, {"ncwl"}},
    {"nRCDRD", &Timing::nRCDRD, {"nrcdrd", "trcdrd_ns", "trcd_rd_ns"}},
    {"nRCDWR", &Timing::nRCDWR, {"nrcdwr", "trcdwr_ns", "trcd_wr_ns"}},
    {"nRP", &Timing::nRP, {"nrp", "trp_ns", "trppb_ns"}},
    {"nRAS", &Timing::nRAS, {"nras", "tras_ns"}},
    {"nRC", &Timing::nRC, {"nrc", "trc_ns"}},
    {"nRTP", &Timing::nRTP, {"nrtp", "trtp_ns"}},
    {"nWR", &Timing::nWR, {"nwr", "twr_ns", "twtp_ns"}},
    {"nCCDS", &Timing::nCCDS, {"nccds", "tccds_ns"}},
    {"nCCDL", &Timing::nCCDL, {"nccdl", "tccdl_ns"}},
    {"nRRDS", &Timing::nRRDS, {"nrrds", "trrds_ns", "trrd_s_ns", "trrd_ns"}},
    {"nRRDL", &Timing::nRRDL, {"nrrdl", "trrdl_ns", "trrd_l_ns"}},
    {"nFAW", &Timing::nFAW, {"nfaw", "tfaw_ns"}},
    {"nAADMin", &Timing::nAADMin, {"naadmin", "taad_min_ns"}},
    {"nAADMax", &Timing::nAADMax, {"naad", "naadmax", "taad_ns", "taad_max_ns"}},
    {"nWCK2CK", &Timing::nWCK2CK, {"nwck2ck", "twck2ck_ns"}},
    {"nWCKPST", &Timing::nWCKPST, {"nwckpst", "twckpst_ns"}},
    {"nCAS", &Timing::nCAS, {"ncas", "tcas_ns"}},
    {"nCS", &Timing::nCS, {"ncs", "tcs_ns"}},
    {"nPPD", &Timing::nPPD, {"nppd", "tppd_ns"}},
    {"nRPab", &Timing::nRPab, {"nrpab", "trpab_ns"}},
    {"nWTRS", &Timing::nWTRS, {"nwtrs", "twtrs_ns", "twtr_s_ns"}},
    {"nWTRL", &Timing::nWTRL, {"nwtrl", "twtrl_ns", "twtr_l_ns"}},
    {"nRTW", &Timing::nRTW, {"nrtw", "trtw_ns"}},
    {"nCCDR", &Timing::nCCDR, {"nccdr", "tccdr_ns"}},
    {"nRFC", &Timing::nRFC, {"nrfc", "trfc_ns", "trfcab_ns"}},
    {"nRFCpb", &Timing::nRFCpb, {"nrfcpb", "trfcpb_ns", "trfcdb_ns"}},
    {"nRFMab", &Timing::nRFMab, {"nrfmab", "trfmab_ns"}},
    {"nRFMpb", &Timing::nRFMpb, {"nrfmpb", "trfmpb_ns"}},
    {"nRREFD", &Timing::nRREFD, {"nrrefd", "trrefd_ns"}},
    {"nREFDB2ACT", &Timing::nREFDB2ACT, {"nrefdb2act", "tdbr2act_ns", "trefdb2act_ns"}},
    {"nREFDB2REFDBS", &Timing::nREFDB2REFDBS, {"nrefdb2refdbs", "tdbr2dbr_s_ns", "trefdb2refdb_s_ns"}},
    {"nREFDB2REFDBL", &Timing::nREFDB2REFDBL, {"nrefdb2refdbl", "tdbr2dbr_l_ns", "trefdb2refdb_l_ns"}},
    {"nREFI", &Timing::nREFI, {"nrefi", "trefi_us"}},
    {"nREFIpb", &Timing::nREFIpb, {"nrefipb", "trefipb_us", "trefipb_ns", "trefidb_ns"}},
    {"nMRW", &Timing::nMRW, {"nmrw", "tmrw_ns"}},
    {"nMRR", &Timing::nMRR, {"nmrr", "tmrr_ns"}},
    {"nWCKSYNC", &Timing::nWCKSYNC, {"nwcksync", "twcksync_ns"}},
    {"nWCKTRAIN", &Timing::nWCKTRAIN, {"nwcktrain", "twcktrain_ns"}},
    {"nDVFS", &Timing::nDVFS, {"ndvfs", "tdvfs_ns"}},
    {"nPDEX", &Timing::nPDEX, {"npdex", "tpdex_ns", "txp_ns"}},
    {"nSREFEX", &Timing::nSREFEX, {"nsrefex", "tsrefex_ns", "txs_ns"}},
    {"nECCSCRUB", &Timing::nECCSCRUB, {"neccscrub", "teccscrub_ns"}},
    {"nRASERR", &Timing::nRASERR, {"nraserr", "traserr_ns"}},
    {"nLINKRETRY", &Timing::nLINKRETRY, {"nlinkretry", "tlinkretry_ns"}},
  };
  return fields;
}
inline const TimingField* find_timing_field(std::string_view key) {
  for (const auto& field : timing_fields())
    for (auto alias : field.keys) if (alias == key) return &field;
  return nullptr;
}
inline std::string canonical_timing_name_for_key(const std::string& key) {
  const auto* field = find_timing_field(key);
  return field ? std::string(field->name) : std::string{};
}
inline bool assign_timing_field(DramSpec& spec, const std::string& key,
                                const std::string& value) {
  const auto* field = find_timing_field(key);
  if (!field) return false;
  spec.timing.*(field->member) = key.ends_with("_ns")
      ? jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps)
      : key.ends_with("_us")
          ? jedec::us_to_nck(parse_double(value), spec.timing.tCK_ps)
          : parse_int(value);
  return true;
}
}  // namespace hbm_sim::config
