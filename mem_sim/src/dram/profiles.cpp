// Timing profile 数据库：按 standard/speed-bin/density/stack-height/mode
// 展开 HBM3/HBM4/LPDDR5/LPDDR6 的可配置 timing 表。
//
// 这里的 generic profile 不是具体厂商保证值；它用于把 JEDEC/vendor 表的维度
// 正式建模出来。真正做数值级对比时，应设置 vendor_profile 并继续把本文件中
// 对应 profile 行替换为目标 datasheet 的数值。
#include "hbm_sim/dram/profiles.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include "hbm_sim/dram/jedec.hpp"

namespace hbm_sim {
namespace {

int tck_ps_for_speed(int data_rate_mbps) {
  if (data_rate_mbps <= 0) {
    return 0;
  }
  return static_cast<int>(std::llround(4000000.0 / static_cast<double>(data_rate_mbps)));
}



void mark(DramSpec& spec, const char* name, TimingValueSource source, const std::string& note) {
  set_timing_source(spec, name, source, note);
}

void mark_many(DramSpec& spec,
               std::initializer_list<const char*> names,
               TimingValueSource source,
               const std::string& note) {
  for (const char* name : names) {
    mark(spec, name, source, note);
  }
}

struct TimingNs {
  double value = 0.0;
  bool standard_defined = true;
};

TimingNs hbm3_trfcab_ns(double density_gb, int stack_height) {
  // JESD238B.01 表 93 按裸片密度和堆叠高度给出 tRFCab。标准中标记 TBD 的项
  // 使用最近的非 TBD 研究值，使仿真器可运行，同时保留密度和堆叠高度输入。
  const int stack = std::max(4, stack_height);
  const bool exact_height = stack_height == 4 || stack_height == 8 ||
                            stack_height == 12 || stack_height == 16;
  if (density_gb <= 8) {
    if (stack <= 4) return {260.0, false};
    if (stack <= 8) return {260.0, density_gb == 8 && exact_height};
    if (stack <= 12) return {310.0, density_gb == 8 && exact_height};
    return {350.0, density_gb == 8 && exact_height};
  }
  if (density_gb <= 16) {
    if (stack <= 4) return {260.0, density_gb == 16 && exact_height};
    if (stack <= 8) return {350.0, density_gb == 16 && exact_height};
    if (stack <= 12) return {410.0, density_gb == 16 && exact_height};
    return {450.0, density_gb == 16 && exact_height};
  }
  if (density_gb <= 24) {
    if (stack <= 4) return {310.0, density_gb == 24 && exact_height};
    if (stack <= 8) return {410.0, density_gb == 24 && exact_height};
    if (stack <= 12) return {450.0, density_gb == 24 && exact_height};
    return {450.0, false};
  }
  if (stack <= 8) return {450.0, false};
  return {520.0, false};
}

TimingNs hbm3_trfcpb_ns(double density_gb) {
  // JESD238B.01 表 93 给出 16Gb 和 24Gb tRFCpb；8Gb/32Gb 仍为 TBD。
  if (density_gb <= 8) return {200.0, false};
  if (density_gb <= 16) return {200.0, density_gb == 16};
  if (density_gb <= 24) return {240.0, density_gb == 24};
  return {240.0, false};
}

struct Lpddr6CoreTimingNs {
  double rcd_write = 0.0;
  double rcd_read = 0.0;
  double tacu = 0.0;
  double rp_ab = 0.0;
  double rp_pb = 0.0;
  double rtp_tail = 0.0;
  double wtp = 0.0;
  double wtr_s = 0.0;
  double wtr_l = 0.0;
};

int lpddr6_nacu_for_speed(int data_rate_mbps, bool dvfsl_enabled) {
  // JESD209-6 表 415 和 420 按数据速率区间给出 nACU。10667Mbps 以上仍为
  // TBD，因此沿用最后一个已定义区间。
  struct Band {
    int upper_mbps;
    int nacu_no_dvfsl;
    int nacu_dvfsl;
  };
  static constexpr Band bands[] = {
      {1067, 6, 7},    {1600, 9, 11},   {2133, 12, 14},  {2750, 16, 18},
      {3200, 18, 21},  {3750, 21, 24},  {4267, 24, 27},  {4800, 27, 31},
      {5500, 31, 35},  {6400, 36, 41},  {7500, 42, 48},  {8533, 47, 54},
      {9600, 53, 61},  {10667, 59, 68}, {14400, 59, 68},
  };
  for (const auto& band : bands) {
    if (data_rate_mbps <= band.upper_mbps) {
      return dvfsl_enabled ? band.nacu_dvfsl : band.nacu_no_dvfsl;
    }
  }
  return dvfsl_enabled ? 68 : 59;
}

Lpddr6CoreTimingNs lpddr6_core_timing_ns(bool dvfsl_enabled, bool link_protection, bool efficiency_mode) {
  // JESD209-6 表 414/416/417/418/419/421/422/423 按 DVFSL、链路保护和
  // 静态/动态 efficiency mode 划分核心时序。这里只映射控制器可见时序。
  if (!dvfsl_enabled) {
    Lpddr6CoreTimingNs t{8.0, 18.0, 22.0, 21.0, 18.0, 1.25, 12.0, 6.25, 12.0};
    if (!link_protection && efficiency_mode) {
      t.wtp = 14.0;
      t.wtr_s = 8.25;
      t.wtr_l = 14.0;
    } else if (link_protection && !efficiency_mode) {
      t.wtp = 16.0;
      t.wtr_s = 10.25;
      t.wtr_l = 16.0;
    } else if (link_protection && efficiency_mode) {
      t.wtp = 18.0;
      t.wtr_s = 12.25;
      t.wtr_l = 18.0;
    }
    return t;
  }

  Lpddr6CoreTimingNs t{9.2, 20.7, 25.3, 24.2, 20.7, 1.5, 13.8, 7.2, 13.8};
  if (!link_protection && efficiency_mode) {
    t.wtp = 16.1;
    t.wtr_s = 9.5;
    t.wtr_l = 16.1;
  } else if (link_protection && !efficiency_mode) {
    t.wtp = 18.4;
    t.wtr_s = 11.8;
    t.wtr_l = 18.4;
  } else if (link_protection && efficiency_mode) {
    t.wtp = 20.7;
    t.wtr_s = 14.1;
    t.wtr_l = 20.7;
  }
  return t;
}

TimingNs lpddr6_trfcab_ns(double density_gb) {
  // JESD209-6 表 302 按每两个子通道的密度给出刷新恢复时间。
  if (density_gb <= 8) return {210.0, density_gb == 6 || density_gb == 8};
  if (density_gb <= 16) return {280.0, density_gb == 12 || density_gb == 16};
  if (density_gb <= 32) return {380.0, density_gb == 24 || density_gb == 32};
  return {380.0, false};
}

TimingNs lpddr6_trfcdb_ns(double density_gb) {
  if (density_gb <= 8) return {140.0, density_gb == 6 || density_gb == 8};
  if (density_gb <= 16) return {160.0, density_gb == 12 || density_gb == 16};
  if (density_gb <= 32) return {210.0, density_gb == 24 || density_gb == 32};
  return {210.0, false};
}

void apply_hbm4_profile(DramSpec& spec, double resolved_tck_ps) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 8000;
  if (spec.density_gb <= 0) spec.density_gb = 32;
  if (spec.stack_height <= 0) spec.stack_height = 8;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.name = "HBM4";
  spec.data_rate_mbps = spec.speed_bin_mbps;
  spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.speed_bin_mbps));
  if (resolved_tck_ps > 0) spec.timing.tCK_ps = resolved_tck_ps;
  spec.full_stack_model = true;
  spec.hbm_full_32_channel_stack = true;
  spec.org.channels = 32;
  spec.org.pseudo_channels = 2;
  spec.org.sids = std::max(1, spec.stack_height / 4);
  spec.org.ranks = 1;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 8;
  spec.org.rows = 1 << 14;
  spec.org.columns = 32;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 32;
  spec.data_bus_bits = 2048;
  spec.internal_prefetch_size = 8;
  spec.tick_multiplier = 2;
  spec.hbm_sid_mapping = spec.stack_height >= 16 ? "sid_per_4hi_slice" : "sid_pair_8hi";
  spec.hbm_ras_policy = spec.hbm_ras_metadata_bits_per_request > 0 ? "ras_metadata_sideband" : "counter_only";
  if (spec.rfm_act_threshold <= 0) {
    spec.rfm_act_threshold = 256;
  }
  if (spec.rfm_decrement <= 0) {
    spec.rfm_decrement = spec.rfm_act_threshold;
  }
  if (spec.hbm_link_crc_mode == "crc16" && spec.hbm_link_crc_bits_per_request == 0) {
    spec.hbm_link_crc_bits_per_request = 16;
  }
  spec.column_bus_scope = spec.hbm_sid_interleave ? TimingScope::Sid : TimingScope::PseudoChannel;
  if (spec.supports_ecc && spec.hbm_ecc_bits_per_request == 0 && spec.ecc_bits_per_request == 0) {
    spec.hbm_ecc_bits_per_request = 16;
  }
  spec.hbm_ecc_scheme =
      spec.hbm_ecc_bits_per_request > 0 ? "metadata_16b_per_32B_transaction" : "none";

  const int speed = spec.speed_bin_mbps;
  if (speed <= 6400) {
    spec.timing.nCL = 26;
    spec.timing.nCWL = 8;
    spec.timing.nRCDRD = 26;
    spec.timing.nRCDWR = 14;
    spec.timing.nRP = 26;
    spec.timing.nRAS = 48;
    spec.timing.nRTP = 6;
    spec.timing.nWR = 24;
    spec.timing.nRRDS = 8;
    spec.timing.nRRDL = 8;
    spec.timing.nFAW = 24;
    spec.timing.nRTW = 18;
    spec.timing.nWTRS = 8;
    spec.timing.nWTRL = 12;
  } else if (speed <= 8000) {
    spec.timing.nCL = 30;
    spec.timing.nCWL = 10;
    spec.timing.nRCDRD = 30;
    spec.timing.nRCDWR = 16;
    spec.timing.nRP = 30;
    spec.timing.nRAS = 54;
    spec.timing.nRTP = 8;
    spec.timing.nWR = 28;
    spec.timing.nRRDS = 8;
    spec.timing.nRRDL = 10;
    spec.timing.nFAW = 28;
    spec.timing.nRTW = 25;
    spec.timing.nWTRS = 9;
    spec.timing.nWTRL = 13;
  } else {
    spec.timing.nCL = 34;
    spec.timing.nCWL = 12;
    spec.timing.nRCDRD = 34;
    spec.timing.nRCDWR = 18;
    spec.timing.nRP = 34;
    spec.timing.nRAS = 60;
    spec.timing.nRTP = 8;
    spec.timing.nWR = 32;
    spec.timing.nRRDS = 10;
    spec.timing.nRRDL = 12;
    spec.timing.nFAW = 32;
    spec.timing.nRTW = 28;
    spec.timing.nWTRS = 10;
    spec.timing.nWTRL = 14;
  }

  spec.timing.nBL = 2;
  spec.timing.nRC = spec.timing.nRAS + spec.timing.nRP;
  spec.timing.nCCDS = 2;
  spec.timing.nCCDL = jedec::hbm_tccdl_nck(spec.timing.tCK_ps);
  spec.timing.nCCDR = spec.hbm_sid_interleave ? spec.timing.nCCDS + 1 : spec.timing.nCCDS;
  spec.timing.nAADMin = 1;
  spec.timing.nAADMax = 8;
  spec.timing.nWCK2CK = 1;
  spec.timing.nWCKPST = 8;
  spec.timing.nCAS = 0;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nRPab = spec.timing.nRP;

  double trfc_ab_ns = 450.0;
  double trfc_pb_ns = 280.0;
  const bool standard_refresh_density = spec.density_gb == 24 || spec.density_gb == 32;
  const bool standard_refresh_height = spec.stack_height == 4 || spec.stack_height == 8 ||
                                       spec.stack_height == 12 || spec.stack_height == 16;
  if (standard_refresh_density && standard_refresh_height) {
    // JESD270-4A Table 108, pp.195-196: density is per die; RFCab also
    // depends on stack height. Other geometries use research fallbacks below.
    const int height_index = spec.stack_height / 4 - 1;
    const double per24[] = {360, 410, 450, 490};
    const double per32[] = {400, 450, 490, 530};
    trfc_ab_ns = spec.density_gb == 24 ? per24[height_index] : per32[height_index];
    trfc_pb_ns = spec.density_gb == 24 ? 240 : 280;
  } else if (spec.density_gb <= 24) {
    trfc_ab_ns = 380.0;
    trfc_pb_ns = 240.0;
  } else if (spec.density_gb >= 48) {
    trfc_ab_ns = 560.0;
    trfc_pb_ns = 350.0;
  }
  spec.timing.nRFC = jedec::ns_to_nck(trfc_ab_ns, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(trfc_pb_ns, spec.timing.tCK_ps);
  spec.timing.nRFMab = spec.timing.nRFC;
  spec.timing.nRFMpb = spec.timing.nRFCpb;
  spec.timing.nRREFD = jedec::max_ns_or_nck(8.0, 3, spec.timing.tCK_ps);
  spec.timing.nREFI = jedec::us_to_nck(3.9, spec.timing.tCK_ps);
  int rotation_banks = std::max(1, spec.stack_height) * 4;
  spec.timing.nREFIpb = jedec::hbm_trefipb_nck(spec.timing.tCK_ps, rotation_banks);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  // 这些字段对 HBM 核心命令约束不生效，但保留旧 preset 的控制/低功耗默认值，
  // 使重构前后的输出和低功耗实验保持一致。
  spec.timing.nWCKSYNC = 8;
  spec.timing.nWCKTRAIN = 64;
  spec.timing.nDVFS = 128;
  spec.timing.nPDEX = 8;
  spec.timing.nSREFEX = 256;
  spec.timing.nECCSCRUB = spec.supports_ecc ? jedec::ns_to_nck(64.0, spec.timing.tCK_ps) : 0;
  spec.timing.nRASERR = spec.hbm_link_retry_enabled ? jedec::ns_to_nck(32.0, spec.timing.tCK_ps) : 0;
  spec.timing.nLINKRETRY = spec.hbm_link_retry_enabled ? jedec::ns_to_nck(16.0, spec.timing.tCK_ps) : 0;

  const std::string note = "HBM4 profile=" + spec.timing_profile + ", vendor=" + spec.vendor_profile +
                           ", speed=" + std::to_string(spec.speed_bin_mbps) +
                           ", density=" + std::to_string(spec.density_gb) +
                           "Gb, stack=" + std::to_string(spec.stack_height) + "Hi.";
  mark_many(spec,
            {"nBL", "nCCDS", "nCCDL", "nRREFD", "nREFI"},
            TimingValueSource::JEDEC,
            note);
  mark(spec, "nRFC", standard_refresh_density && standard_refresh_height
                           ? TimingValueSource::JEDEC : TimingValueSource::ResearchDefault,
       "JESD270-4A Table 108, density/height lookup; unmatched rows are research fallbacks.");
  mark(spec, "nRFCpb", standard_refresh_density
                             ? TimingValueSource::JEDEC : TimingValueSource::ResearchDefault,
       "JESD270-4A Table 108, 24/32 Gb per die; other densities are research fallbacks.");
  mark(spec, "nREFIpb", spec.stack_height == 8 || spec.stack_height == 12 || spec.stack_height == 16
                             ? TimingValueSource::JEDEC : TimingValueSource::ResearchDefault,
       "JESD270-4A Table 108 explicitly lists 8/12/16-High rotation intervals.");
  mark_many(spec, {"nRFMab", "nRFMpb"}, TimingValueSource::ResearchDefault,
            "Project RFM recovery defaults copied from RFC; requires device RFM/DRFM evidence.");
  mark_many(spec, {"nMRW", "nMRR", "nECCSCRUB"},
            TimingValueSource::ResearchDefault,
            "HBM4 control/RAS/link timing requires device mode table or vendor reliability guide.");
  if (spec.hbm_link_retry_enabled) {
    mark_many(spec, {"nRASERR", "nLINKRETRY"},
              TimingValueSource::ResearchDefault,
              "HBM4 enabled link-retry timing requires a vendor reliability guide.");
  }

}

void apply_hbm3_profile(DramSpec& spec, double resolved_tck_ps) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 6400;
  if (spec.density_gb <= 0) spec.density_gb = 16;
  if (spec.stack_height <= 0) spec.stack_height = 8;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.name = "HBM3";
  spec.data_rate_mbps = spec.speed_bin_mbps;
  spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.speed_bin_mbps));
  if (resolved_tck_ps > 0) spec.timing.tCK_ps = resolved_tck_ps;
  spec.full_stack_model = true;
  spec.org.channels = 16;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 2;
  spec.org.ranks = 1;
  spec.org.bank_groups = 4;
  spec.org.banks_per_group = 4;
  spec.org.rows = 1 << 14;
  spec.org.columns = 1 << 5;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 32;
  spec.data_bus_bits = 1024;
  spec.internal_prefetch_size = 8;
  spec.tick_multiplier = 2;
  spec.timing.nBL = 2;
  spec.timing.nCL = 20;
  spec.timing.nCWL = 10;
  spec.timing.nRCDRD = jedec::ns_to_nck(31 * 0.625, spec.timing.tCK_ps);
  spec.timing.nRCDWR = jedec::ns_to_nck(15 * 0.625, spec.timing.tCK_ps);
  spec.timing.nRP = jedec::ns_to_nck(26 * 0.625, spec.timing.tCK_ps);
  spec.timing.nRAS = jedec::ns_to_nck(45 * 0.625, spec.timing.tCK_ps);
  spec.timing.nRC = std::max(spec.timing.nRAS + spec.timing.nRP,
                             jedec::ns_to_nck(72 * 0.625, spec.timing.tCK_ps));
  spec.timing.nRTP = jedec::ns_to_nck(9 * 0.625, spec.timing.tCK_ps);
  spec.timing.nWR = jedec::ns_to_nck(33 * 0.625, spec.timing.tCK_ps);
  spec.timing.nCCDS = 2;
  spec.timing.nCCDL = jedec::hbm_tccdl_nck(spec.timing.tCK_ps);
  spec.timing.nCCDR = spec.timing.nCCDS + 1;
  spec.timing.nRRDS = jedec::ns_to_nck(4 * 0.625, spec.timing.tCK_ps);
  spec.timing.nRRDL = jedec::ns_to_nck(5 * 0.625, spec.timing.tCK_ps);
  spec.timing.nFAW = 24;
  spec.timing.nAADMin = 1;
  spec.timing.nAADMax = 8;
  spec.timing.nWCK2CK = 1;
  spec.timing.nWCKPST = 8;
  spec.timing.nCAS = 0;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nRPab = 20;
  spec.timing.nWTRS = jedec::ns_to_nck(7 * 0.625, spec.timing.tCK_ps);
  spec.timing.nWTRL = jedec::ns_to_nck(10 * 0.625, spec.timing.tCK_ps);
  spec.timing.nRTW = 20;
  const TimingNs trfc_ab = hbm3_trfcab_ns(spec.density_gb, spec.stack_height);
  const TimingNs trfc_pb = hbm3_trfcpb_ns(spec.density_gb);
  spec.timing.nRFC = jedec::ns_to_nck(trfc_ab.value, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(trfc_pb.value, spec.timing.tCK_ps);
  spec.timing.nRFMab = 0;
  spec.timing.nRFMpb = 0;
  spec.timing.nRREFD = jedec::ns_to_nck(8 * 0.625, spec.timing.tCK_ps);
  spec.timing.nREFDB2ACT = 0;
  spec.timing.nREFDB2REFDBS = 0;
  spec.timing.nREFDB2REFDBL = 0;
  spec.timing.nREFI = jedec::us_to_nck(3.9, spec.timing.tCK_ps);
  spec.timing.nREFIpb = jedec::hbm_trefipb_nck(spec.timing.tCK_ps, std::max(1, spec.stack_height) * 4);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  spec.timing.nWCKSYNC = 8;
  spec.timing.nWCKTRAIN = 64;
  spec.timing.nDVFS = 128;
  spec.timing.nPDEX = 8;
  spec.timing.nSREFEX = 256;
  spec.timing.nECCSCRUB = 0;
  spec.timing.nRASERR = 0;
  spec.timing.nLINKRETRY = 0;
  mark_many(spec, {"nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC",
                   "nRTP", "nWR", "nRRDS", "nRRDL", "nFAW", "nWTRS", "nWTRL",
                   "nRTW", "nMRW", "nMRR"},
            spec.data_rate_mbps == 6400 ? TimingValueSource::ExternalReference
                                       : TimingValueSource::ResearchDefault,
            "Ramulator HBM3 6400 baseline; time-based row constraints rescaled by CK. RL/WL at other rates are research assumptions.");
  const std::string note = "HBM3 JESD238B.01 Table 93 profile=" + spec.timing_profile +
                           ", density=" + std::to_string(spec.density_gb) +
                           "Gb, stack=" + std::to_string(spec.stack_height) + "Hi.";
  mark_many(spec,
            {"nBL", "nCCDS", "nCCDL", "nREFI", "nREFIpb"},
            TimingValueSource::JEDEC,
            note);
  mark_many(spec,
            {"nRFC", "nRFCpb"},
            (trfc_ab.standard_defined && trfc_pb.standard_defined) ? TimingValueSource::JEDEC
                                                                   : TimingValueSource::ResearchDefault,
            note);
  mark(spec, "nRREFD", TimingValueSource::ExternalReference,
       "Controlled HBM3 reference interval; not a JEDEC tRREFD claim.");

}

void apply_lpddr6_profile(DramSpec& spec, double resolved_tck_ps) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 10667;
  if (spec.density_gb <= 0) spec.density_gb = 16;
  if (spec.lpddr_dvfs_mode == LpddrDvfsMode::Low) {
    spec.data_rate_mbps = std::max(1, spec.lpddr_low_data_rate_mbps);
  } else if (spec.lpddr_dvfs_mode == LpddrDvfsMode::Disabled) {
    spec.data_rate_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : spec.speed_bin_mbps;
  } else {
    spec.data_rate_mbps = spec.speed_bin_mbps;
  }
  // The exported speed selector describes the selected operating point, including
  // low/disabled DVFS, just as it does on the shared config construction path.
  spec.speed_bin_mbps = spec.data_rate_mbps;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.timing.tCK_ps = static_cast<double>(tck_ps_for_speed(spec.data_rate_mbps));
  if (resolved_tck_ps > 0) spec.timing.tCK_ps = resolved_tck_ps;
  spec.name = "LPDDR6";
  spec.org.channels = 1;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 4;
  spec.org.banks_per_group = 4;
  spec.org.rows = 1 << 16;
  spec.org.columns = 1 << 6;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 32;
  spec.data_bus_bits = 24;
  // JESD209-6 x12 subchannel uses BL24: 256 data bits + 32 non-data bits.
  // The project stores the burst beat count here and the data payload in
  // dram_transaction_bytes.
  spec.internal_prefetch_size = 24;
  spec.tick_multiplier = 1;
  spec.full_stack_model = false;
  if (spec.rfm_act_threshold <= 0) {
    spec.rfm_act_threshold = 512;
  }
  if (spec.rfm_decrement <= 0) {
    spec.rfm_decrement = spec.rfm_act_threshold;
  }

  const int speed = spec.data_rate_mbps;
  const bool dvfsl_enabled = spec.lpddr_dvfs_mode != LpddrDvfsMode::Disabled;
  // Identity labels must not secretly enable protocol features.
  const bool ca_parity_requested = spec.lpddr_ca_parity_enabled;
  const bool link_protection = spec.lpddr_link_protection || spec.lpddr_link_ecc_enabled;
  const bool efficiency_mode = spec.lpddr_efficiency_mode != LpddrEfficiencyMode::Normal;
  const Lpddr6CoreTimingNs core = lpddr6_core_timing_ns(dvfsl_enabled, link_protection, efficiency_mode);

  spec.lpddr_link_protection = link_protection;
  spec.lpddr_link_ecc_enabled = spec.lpddr_link_ecc_enabled || link_protection;
  spec.lpddr_ca_parity_enabled = ca_parity_requested;
  // JESD209-6 Table 381: a BL24 DQ burst is 6 CK at every rate
  // (24 UI / (2 edges * WCK:CK=2)). This is NOT the same-BG array
  // cycle, which is selected separately from Table 382 below.
  spec.timing.nBL = 6;
  spec.timing.nCL = speed >= 10000 ? 62 : (speed >= 8533 ? 54 : 46);
  spec.timing.nCWL = speed >= 10000 ? 26 : (speed >= 8533 ? 22 : 18);
  spec.timing.nRCDRD = jedec::max_ns_or_nck(core.rcd_read, 2, spec.timing.tCK_ps);
  spec.timing.nRCDWR = jedec::max_ns_or_nck(core.rcd_write, 2, spec.timing.tCK_ps);
  const int nACU = lpddr6_nacu_for_speed(speed, dvfsl_enabled);
  spec.timing.nRP = nACU + jedec::max_ns_or_nck(core.rp_pb, 4, spec.timing.tCK_ps);
  spec.timing.nRPab = nACU + jedec::max_ns_or_nck(core.rp_ab, 4, spec.timing.tCK_ps);
  spec.timing.nRAS = jedec::max_ns_or_nck(20.0, 4, spec.timing.tCK_ps);
  spec.timing.nRC = spec.timing.nRAS + spec.timing.nRP;
  spec.timing.nRTP = spec.timing.nBL + jedec::ns_to_nck(core.rtp_tail, spec.timing.tCK_ps);
  spec.timing.nWR = jedec::max_ns_or_nck(core.wtp, 6, spec.timing.tCK_ps);
  spec.timing.nCCDS = 6;
  // Table 382 BL24 limits are inclusive at the upper data-rate bound.
  spec.timing.nCCDL = speed <= 6400 ? 6 : speed <= 8533 ? 8
                                      : speed <= 10667 ? 10 : 12;
  spec.timing.nRRDS = jedec::max_ns_or_nck(3.75, 4, spec.timing.tCK_ps);
  spec.timing.nRRDL = spec.timing.nRRDS;
  spec.timing.nFAW = 4 * spec.timing.nRRDS;
  spec.timing.nAADMin = 1;
  spec.timing.nAADMax = 8;
  spec.timing.nWCK2CK = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : (speed >= 10000 ? 22 : 18);
  spec.timing.nWCKPST = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : 3;
  spec.timing.nCAS = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : spec.timing.nWCK2CK;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nWTRS = jedec::max_ns_or_nck(core.wtr_s, 6, spec.timing.tCK_ps);
  spec.timing.nWTRL = jedec::max_ns_or_nck(core.wtr_l, 6, spec.timing.tCK_ps);
  spec.timing.nRTW = 16;
  spec.timing.nCCDR = 2;
  spec.timing.nRREFD = jedec::ns_to_nck(7.5, spec.timing.tCK_ps);
  spec.timing.nREFDB2ACT = spec.timing.nRREFD;
  spec.timing.nREFDB2REFDBS = jedec::ns_to_nck(47.0, spec.timing.tCK_ps);
  spec.timing.nREFDB2REFDBL = jedec::ns_to_nck(90.0, spec.timing.tCK_ps);
  // density_gb is per subchannel; Table 302 is explicitly per TWO
  // subchannels, independent of how many channels the experiment instantiates.
  const TimingNs trfc_ab = lpddr6_trfcab_ns(2.0 * spec.density_gb);
  const TimingNs trfc_db = lpddr6_trfcdb_ns(2.0 * spec.density_gb);
  spec.timing.nRFC = jedec::ns_to_nck(trfc_ab.value, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(trfc_db.value, spec.timing.tCK_ps);
  // JESD209-6 p396 Tables 366/367: five row-refresh spans, not tRFC.
  spec.timing.nRFMab = jedec::ns_to_nck(5.0 * 80.0, spec.timing.tCK_ps);
  spec.timing.nRFMpb = jedec::ns_to_nck(5.0 * 70.0, spec.timing.tCK_ps);
  spec.timing.nREFI = jedec::us_to_nck(3.906, spec.timing.tCK_ps);
  spec.timing.nREFIpb = jedec::ns_to_nck(spec.refresh_temperature_mode == RefreshTemperatureMode::Normal ? 488.0 : 244.0,
                                         spec.timing.tCK_ps);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  spec.timing.nWCKSYNC = spec.lpddr_wck_mode == LpddrWckMode::AlwaysOn ? 0 : spec.timing.nWCK2CK;
  spec.timing.nWCKTRAIN = spec.lpddr_wck_training_required ? jedec::ns_to_nck(64.0, spec.timing.tCK_ps) : 0;
  spec.timing.nDVFS = spec.lpddr_dvfs_mode == LpddrDvfsMode::Disabled ? 0 : jedec::ns_to_nck(128.0, spec.timing.tCK_ps);
  spec.timing.nPDEX = spec.low_power_exit_cycles > 0 ? spec.low_power_exit_cycles : 8;
  spec.timing.nSREFEX = spec.self_refresh_exit_cycles > 0 ? spec.self_refresh_exit_cycles : 256;
  spec.timing.nSREFEX = std::max(spec.timing.nPDEX, spec.timing.nSREFEX);
  spec.timing.nECCSCRUB = spec.lpddr_link_ecc_enabled ? jedec::ns_to_nck(32.0, spec.timing.tCK_ps) : 0;
  spec.timing.nRASERR = spec.lpddr_link_protection ? jedec::ns_to_nck(24.0, spec.timing.tCK_ps) : 0;
  spec.timing.nLINKRETRY = spec.lpddr_link_protection ? jedec::ns_to_nck(16.0, spec.timing.tCK_ps) : 0;

  if (spec.lpddr_link_protection && spec.lpddr_link_ecc_bits_per_request == 0) {
    spec.lpddr_link_ecc_bits_per_request = 16;
  }
  if (spec.lpddr_dbi_enabled && spec.lpddr_dbi_bits_per_request == 0) {
    spec.lpddr_dbi_bits_per_request = 8;
  }
  if (spec.lpddr_ca_parity_enabled && spec.lpddr_ca_parity_bits_per_command <= 0) {
    spec.lpddr_ca_parity_bits_per_command = 1;
  }
  spec.lpddr_link_protection_mode =
      spec.lpddr_link_protection ?
      (spec.lpddr_ca_parity_enabled ? "link_ecc_crc_retry_ca_parity" : "link_ecc_crc_retry") :
      (spec.lpddr_ca_parity_enabled ? "ca_parity_only" : "off");
  spec.lpddr_dvfs_transition_policy = dvfsl_enabled ? "idle_channel_nacu_guarded" : "disabled";
  spec.lpddr_wck_training_mode =
      spec.lpddr_wck_training_required ? "startup_and_dvfs_retrain" : "cas_sync_only";
  spec.lpddr_low_power_state_policy =
      spec.low_power_mode == LowPowerMode::SelfRefresh ? "self_refresh" :
      (spec.low_power_mode == LowPowerMode::PowerDown ? "power_down" : "controller_idle");

  mark_many(spec,
            {"nBL", "nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRPab", "nRAS",
             "nRTP", "nWR", "nCCDS", "nCCDL", "nRRDS", "nRRDL",
             "nAADMin", "nAADMax",
             "nWCK2CK", "nWCKPST", "nCAS", "nWTRS", "nWTRL", "nRREFD", "nRFC",
             "nREFDB2ACT", "nREFDB2REFDBS", "nREFDB2REFDBL",
             "nRFCpb", "nRFMab", "nRFMpb", "nREFI", "nREFIpb"},
            TimingValueSource::JEDEC,
            "LPDDR6 profile=" + spec.timing_profile + ", mode=" + spec.mode_profile +
                ", vendor=" + spec.vendor_profile +
                ", JESD209-6 Tables 300/301/302/414-423.");
  mark_many(spec,
            {"nMRW", "nMRR", "nWCKSYNC", "nWCKTRAIN", "nDVFS", "nPDEX", "nSREFEX"},
            TimingValueSource::JEDEC,
            "LPDDR6 control/link timing profile for MR programming, WCK training, DVFS and link protection.");
  if (spec.lpddr_link_protection || spec.lpddr_link_ecc_enabled) {
    mark_many(spec, {"nECCSCRUB", "nRASERR", "nLINKRETRY"},
              TimingValueSource::ResearchDefault,
              "LPDDR6 enabled link-protection recovery timing requires a vendor reliability guide.");
  }
  if (!trfc_ab.standard_defined || !trfc_db.standard_defined) {
    mark_many(spec, {"nRFC", "nRFCpb"}, TimingValueSource::ResearchDefault,
              "LPDDR6 density per two subchannels is absent/TBD in Table 302; nearest research value, not interpolation guaranteed by JEDEC.");
  }
  mark_many(spec, {"nRFMab", "nRFMpb"}, TimingValueSource::JEDEC,
            "JESD209-6 p396 Tables 366/367: tRFMab=5*80ns, tRFMpb=5*70ns; independent of tRFC density lookup.");
  mark_many(spec, {"nAADMin"}, TimingValueSource::Derived,
            "Project ACT1/ACT2 scheduling granularity, not the JEDEC tAAD maximum deadline.");
  mark_many(spec, {"nBL", "nCCDS"}, TimingValueSource::JEDEC,
            "JESD209-6 Table 381: BL24 minimum DQ transfer / different-BG column interval = 6 CK; not BL48 or BL/n_max.");
  mark_many(spec, {"nCCDL"}, speed <= 12800 ? TimingValueSource::JEDEC
                                          : TimingValueSource::ResearchDefault,
            speed <= 12800
                ? "JESD209-6 Table 382: BL24 same-BG column interval at the selected data rate."
                : "Beyond Table 382's 12800 Mb/s range: retained 12-CK research fallback, not a verified high-rate array cycle.");
  { // A vendor_profile name alone is not calibration evidence.
    mark_many(spec, {"nWCK2CK", "nWCKPST", "nCAS", "nWCKSYNC", "nWCKTRAIN",
                     "nDVFS", "nMRW", "nMRR", "nPDEX", "nSREFEX"},
              TimingValueSource::ResearchDefault,
              "Behavioral controller/PHY control window; not pin-level tWCK2CK or a verified vendor training table.");
  }
}

void apply_lpddr5_profile(DramSpec& spec, double resolved_tck_ps) {
  if (spec.speed_bin_mbps <= 0) spec.speed_bin_mbps = spec.data_rate_mbps > 0 ? spec.data_rate_mbps : 6400;
  if (spec.density_gb <= 0) spec.density_gb = 16;

  spec.org = Organization{};
  spec.timing = Timing{};
  spec.name = "LPDDR5";
  spec.data_rate_mbps = spec.speed_bin_mbps;
  // LPDDR5 preset 的 CK:WCK 关系与 HBM/LPDDR6 数据率换算不同；6400Mbps
  // 对应当前模型使用的 1.25ns CK。
  spec.timing.tCK_ps = 8000000.0 / static_cast<double>(spec.data_rate_mbps);
  if (resolved_tck_ps > 0) spec.timing.tCK_ps = resolved_tck_ps;
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 4;
  spec.org.banks_per_group = 4;
  // 本项目的 column 单位是一条 DRAM transaction，不是外部配置中的
  // 原始 column-address 编码。x16 BL16 一次传 32B；16 banks × 64K rows
  // × 64 transaction columns × 32B = 2 GiB，对应 16 Gibit device。
  spec.org.rows = 1 << 16;
  spec.org.columns = 1 << 6;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 32;
  spec.data_bus_bits = 16;
  spec.internal_prefetch_size = 16;
  spec.tick_multiplier = 1;
  spec.full_stack_model = false;

  spec.timing.nBL = 2;
  spec.timing.nCL = 17;
  spec.timing.nCWL = 9;
  spec.timing.nRCDRD = 15;
  spec.timing.nRCDWR = 15;
  spec.timing.nRP = 15;
  spec.timing.nRAS = 34;
  spec.timing.nRC = 49;
  spec.timing.nRTP = 8;
  spec.timing.nWR = 28;
  spec.timing.nCCDS = 2;
  spec.timing.nCCDL = 4;
  spec.timing.nRRDS = 4;
  spec.timing.nRRDL = 4;
  spec.timing.nFAW = 16;
  spec.timing.nAADMin = 1;
  spec.timing.nAADMax = 8;
  spec.timing.nWCK2CK = 1;
  spec.timing.nWCKPST = 1;
  spec.timing.nCAS = 0;
  spec.timing.nCS = 2;
  spec.timing.nPPD = 2;
  spec.timing.nRPab = 17;
  spec.timing.nWTRS = 5;
  spec.timing.nWTRL = 10;
  spec.timing.nRTW = 16;
  spec.timing.nCCDR = 2;
  spec.timing.nRFC = jedec::ns_to_nck(280.0, spec.timing.tCK_ps);
  spec.timing.nRFCpb = jedec::ns_to_nck(140.0, spec.timing.tCK_ps);
  spec.timing.nRFMab = 0;
  spec.timing.nRFMpb = 0;
  spec.timing.nRREFD = 0;
  spec.timing.nREFDB2ACT = 0;
  spec.timing.nREFDB2REFDBS = 0;
  spec.timing.nREFDB2REFDBL = 0;
  spec.timing.nREFI = jedec::ns_to_nck(3906.25, spec.timing.tCK_ps);
  spec.timing.nREFIpb = jedec::ns_to_nck(488.28125, spec.timing.tCK_ps);
  spec.timing.nMRW = 8;
  spec.timing.nMRR = 8;
  spec.timing.nWCKSYNC = spec.timing.nWCK2CK;
  spec.timing.nWCKTRAIN = 0;
  spec.timing.nDVFS = 0;
  spec.timing.nPDEX = spec.low_power_mode == LowPowerMode::Off ? 0 : 8;
  spec.timing.nSREFEX = spec.low_power_mode == LowPowerMode::Off ? 0 : 256;
  spec.timing.nECCSCRUB = 64;
  spec.timing.nRASERR = 64;
  spec.timing.nLINKRETRY = 16;

  mark_many(spec,
            {"nBL", "nCCDS", "nCCDL", "nRRDS", "nRRDL", "nAADMin",
             "nAADMax",
             "nWCK2CK", "nWCKPST", "nCAS", "nCS", "nPPD", "nREFI", "nREFIpb"},
            TimingValueSource::JEDEC,
            "LPDDR5 generic profile; select a vendor profile for device-specific RL/WL and row timing.");
  mark_many(spec, {"nCL", "nCWL", "nRCDRD", "nRCDWR", "nRP", "nRPab", "nRAS",
                   "nRC", "nRTP", "nWR", "nWTRS", "nWTRL", "nRFC", "nRFCpb",
                   "nWCKPST", "nREFI", "nREFIpb", "nPDEX", "nSREFEX"},
            spec.data_rate_mbps == 6400 && spec.density_gb == 16
                ? TimingValueSource::ExternalReference : TimingValueSource::ResearchDefault,
            "Ramulator LPDDR5 6400/16Gb baseline; other speed/density choices need device timing. No inferred density refresh table.");

}

}  // namespace

void apply_standard_timing_profile(DramSpec& spec, double resolved_tck_ps) {
  // 重新应用 profile 时丢弃上一轮 profile 的来源标记，避免
  // vendor -> generic 切换后遗留 Vendor 标签。配置的逐项 source override
  // 总是在本函数之后应用。
  spec.timing_source_overrides.clear();

  switch (spec.standard) {
    case DramStandard::Hbm4:
      apply_hbm4_profile(spec, resolved_tck_ps);
      break;
    case DramStandard::Hbm3:
      apply_hbm3_profile(spec, resolved_tck_ps);
      break;
    case DramStandard::Lpddr6:
      apply_lpddr6_profile(spec, resolved_tck_ps);
      break;
    case DramStandard::Lpddr5:
      apply_lpddr5_profile(spec, resolved_tck_ps);
      break;
    case DramStandard::Unknown:
      throw std::invalid_argument("cannot apply timing profile without standard traits");
  }

}

}  // namespace hbm_sim
