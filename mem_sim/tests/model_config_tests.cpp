#include "hbm_sim/config/model.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/controller/timing.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace hbm_sim;
using namespace hbm_sim::config;

namespace {
void require(bool test, const char* message) {
  if (!test) throw std::runtime_error(message);
}
template<class Fn> void rejects(Fn fn) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error("contradictory input was not rejected");
}
}

int main() {
  try {
    for (const char* standard : {"hbm3", "hbm4", "lpddr5", "lpddr6"}) {
      auto legacy = build_model(standard);
      auto reference = make_spec(standard);
      require(legacy.addressable_capacity_bytes() == reference.addressable_capacity_bytes(),
              "extraction changed legacy capacity");
      require(legacy.timing.nCL == reference.timing.nCL, "extraction changed legacy RL");
      const auto resolved = build_model(standard, {}, 3);
      require(resolved.addressable_capacity_bytes() == legacy.addressable_capacity_bytes(),
              "auto resolution changed geometry");
      const auto half = build_model(standard, {{"columns", std::to_string(legacy.org.columns / 2)}}, 3);
      require(half.addressable_capacity_bytes() * 2 == legacy.addressable_capacity_bytes(),
              "column count must change geometry capacity");
      require(std::abs(half.density_gb * 2 - resolved.density_gb) < 1e-12,
              "geometry must change derived density");
      rejects([&] { build_model(standard, {{"columns", "0"}}, 3); });
      rejects([&] { build_model(standard, {{"density_gb", "999"}}, 3); });
      rejects([&] { build_model(standard, {{"data_rate_mbps", "6400"},
                                         {"speed_bin_mbps", "8000"}}, 3); });
      rejects([&] { build_model(standard, {{"tck_ps", "0"}}, 3); });
    }
    auto small = build_model("hbm3", {{"rows", "16"}, {"columns", "16"}}, 3);
    require(small.density_gb > 0 && small.density_gb < 1,
            "fractional geometry density must not be rounded to integer Gb");
    auto fast = build_model("hbm4", {{"data_rate_mbps", "9000"}}, 3);
    require(fast.speed_bin_mbps == 9000 && std::abs(fast.timing.tCK_ps - 4000000.0 / 9000) < 1e-9,
            "speed/tCK coupling failed");
    auto one_channel = build_model("hbm4", {{"channels", "1"}}, 3);
    require(one_channel.data_bus_bits == 64, "HBM interface width must follow channels");
    const auto taller = build_model("hbm4", {{"stack_height", "16"}}, 3);
    require(taller.org.sids == 4 && taller.density_gb == 32 &&
            taller.addressable_capacity_bytes() == 2 * build_model("hbm4", {}, 3).addressable_capacity_bytes(),
            "omitted SID must resolve before capacity and density");
    for (const char* standard : {"hbm3", "hbm4"}) {
      for (int schema : {1, 2, 3}) {
        const auto base = build_model(standard, {}, schema);
        for (int height : {4, 8, 12, 16}) {
          const auto text = std::to_string(height);
          const auto automatic = build_model(standard, {{"stack_height", text}, {"sids", "auto"}}, schema);
          const auto omitted = build_model(standard, {{"stack_height", text}}, schema);
          const auto explicit_sid = build_model(standard, {{"stack_height", text},
              {"sids", std::to_string(height / 4)}}, schema);
          require(automatic.org.sids == height / 4 && omitted.org.sids == automatic.org.sids,
                  "HBM auto/omitted SID height mapping failed");
          require(automatic.addressable_capacity_bytes() * 8 == base.addressable_capacity_bytes() * height &&
                      automatic.density_gb == base.density_gb,
                  "SID capacity/density coupling failed");
          require(automatic.timing.nRFC == explicit_sid.timing.nRFC &&
                      automatic.timing.nCL == base.timing.nCL,
                  "SID resolution must reuse existing timing selection unchanged");
          AddressMapper mapper(automatic);
          const Address last = automatic.addressable_capacity_bytes() - automatic.transaction_bytes();
          const auto first_decoded = mapper.decode(0);
          const auto last_decoded = mapper.decode(last);
          require(last_decoded.sid == automatic.org.sids - 1,
                  "last SID must be reachable through the resolved address space");
          MemoryImage image(automatic);
          image.write(0, ByteVector{0x12}, nullptr, &first_decoded);
          image.write(last, ByteVector{0x34}, nullptr, &last_decoded);
          require(image.read(0, 1, nullptr, &first_decoded) == ByteVector{0x12} &&
                      image.read(last, 1, nullptr, &last_decoded) == ByteVector{0x34},
                  "resolved SID storage addresses must not alias");
        }
        const auto fixed = build_model(standard, {{"stack_height", "16"}, {"sids", "2"}}, schema);
        require(fixed.org.sids == 2 && fixed.density_gb == base.density_gb / 2,
                "explicit research SID must be retained");
        auto partial = fixed;
        apply_spec_overrides(partial, {{"ncl", "80"}});
        require(partial.org.sids == 2, "unrelated partial override changed existing research SID");
        apply_spec_overrides(partial, {{"sids", "auto"}});
        require(partial.org.sids == 4 && partial.density_gb == base.density_gb,
                "explicit auto must reselect SID on a configured model");
        for (const char* invalid : {"0", "-1", "1.5", "unknown"})
          rejects([&] { build_model(standard, {{"sids", invalid}}, schema); });
        rejects([&] { build_model(standard, {{"stack_height", "10"}}, schema); });
        rejects([&] { build_model(standard, {{"stack_height", "10"}, {"sids", "auto"}}, schema); });
        require(build_model(standard, {{"stack_height", "10"}, {"sids", "2"}}, schema).org.sids == 2,
                "nonstandard height must allow explicit research geometry");
      }
    }
    for (const char* standard : {"lpddr5", "lpddr6"})
      require(build_model(standard, {{"sids", "auto"}, {"stack_height", "8"}}).org.sids == 1,
              "LPDDR auto SID must stay neutral, not use HBM height mapping");
    auto lp5 = build_model("lpddr5", {{"lpddr_wck_ratio", "2"}}, 3);
    require(std::abs(lp5.timing.tCK_ps - 625.0) < 1e-9, "LPDDR5 ratio clock coupling failed");
    require(fast.timing.nRFC == jedec::ns_to_nck(450.0, fast.timing.tCK_ps),
            "profile ns timing must use the final resolved CK, not an earlier rounded CK");
    for (int height : {4, 8, 12, 16}) {
      const int index = height / 4 - 1;
      const int rfc24[] = {360, 410, 450, 490};
      const int rfc32[] = {400, 450, 490, 530};
      for (int density : {24, 32}) {
        const auto table = build_model("hbm4", {{"density_gb", std::to_string(density)},
            {"stack_height", std::to_string(height)},
            {"sids", std::to_string(height / 4)},
            {"rows", density == 24 ? "12288" : "16384"}}, 2);
        require(table.timing.nRFC == jedec::ns_to_nck(density == 24 ? rfc24[index] : rfc32[index],
                                                     table.timing.tCK_ps),
                "HBM4 Table 108 density/height RFC lookup failed");
      }
    }
    for (const char* family : {"hbm3", "hbm4"}) {
      const auto unsupported = build_model(family, {{"density_gb", "7.5"},
          {"rows", std::string(family) == "hbm3" ? "7680" : "3840"}}, 2);
      for (const auto& entry : unsupported.timing_table.entries)
        if (entry.name == "nRFC" || entry.name == "nRFCpb")
          require(entry.source == TimingValueSource::ResearchDefault,
                  "interpolated research density must not be labeled JEDEC");
    }
    const auto low = build_model("lpddr6", {{"lpddr_dvfs_mode", "low"},
        {"lpddr_low_data_rate_mbps", "4267"}}, 3);
    require(low.data_rate_mbps == 4267 && low.timing.nCL == 46,
            "low DVFS must resolve rate before profile selection");
    // Independent JEDEC Table 381/382 expectations, not values copied from
    // the generated constraint list. Test both sides of each speed boundary.
    for (const auto [rate, same_bg] : {std::pair{4267, 6}, {6400, 6},
         {6401, 8}, {8533, 8}, {8534, 10}, {10667, 10},
         {10668, 12}, {12800, 12}}) {
      const auto burst = build_model("lpddr6", {{"data_rate_mbps", std::to_string(rate)}}, 3);
      require(burst.timing.nBL == 6 && burst.timing.nCCDS == 6 &&
                  burst.timing.nCCDL == same_bg,
              "LPDDR6 BL24 DQ/array timings disagree with Table 381/382");
      DecodedAddress start{};
      auto check_boundary = [&](Command first, Command next, DecodedAddress target,
                                int expected_nck) {
        TimingEngine engine(burst);
        constexpr Cycle issued = 100;
        engine.apply_constraints(burst, start, first, issued);
        const Cycle ready = issued + expected_nck * burst.tick_multiplier;
        require(!engine.constraint_ready(burst, target, next, ready - 1),
                "LPDDR6 burst-related command allowed one tick too early");
        require(engine.constraint_ready(burst, target, next, ready),
                "LPDDR6 burst-related command blocked at exact boundary");
      };
      auto other_bg = start;
      other_bg.bank_group = 1;
      auto same_group = start;
      same_group.bank = 1;
      for (const auto command : {Command::RD, Command::WR}) {
        check_boundary(command, command, other_bg, 6);
        check_boundary(command, command, same_group, same_bg);
      }
      check_boundary(Command::WR, Command::RD, other_bg,
                     burst.timing.nCWL + 6 + burst.timing.nWTRS);
      check_boundary(Command::WR, Command::RD, same_group,
                     burst.timing.nCWL + 6 + burst.timing.nWTRL);
      check_boundary(Command::WR, Command::PREPB, start,
                     burst.timing.nCWL + 6 + burst.timing.nWR);
    }
    const auto disabled = build_model("lpddr6", {{"lpddr_dvfs_mode", "disabled"},
        {"data_rate_mbps", "8533"}}, 3);
    require(disabled.data_rate_mbps == 8533 && disabled.timing.nCL == 54,
            "disabled DVFS must still select timing at the explicitly requested rate");
    rejects([] { build_model("lpddr6", {{"lpddr_wck_ratio", "4"}}, 3); });
    const auto lp6 = build_model("lpddr6");
    require(lp6.lpddr_wck_ratio == 2, "LPDDR6 WCK:CK must be 2:1");
    require(lp6.timing.nRFC == jedec::ns_to_nck(380, lp6.timing.tCK_ps) &&
            lp6.timing.nRFCpb == jedec::ns_to_nck(210, lp6.timing.tCK_ps),
            "16Gb/subchannel must select 32Gb Table 302 refresh row");
    require(lp6.timing.nRFMab == jedec::ns_to_nck(400, lp6.timing.tCK_ps) &&
            lp6.timing.nRFMpb == jedec::ns_to_nck(350, lp6.timing.tCK_ps),
            "LPDDR6 RFM must follow Tables 366/367, not the density-dependent RFC table");
    const auto lp6_refresh_override = build_model("lpddr6", {{"nrfc", "2000"}, {"nrfcpb", "1000"}}, 3);
    require(lp6_refresh_override.timing.nRFMab == jedec::ns_to_nck(400, lp6_refresh_override.timing.tCK_ps) &&
            lp6_refresh_override.timing.nRFMpb == jedec::ns_to_nck(350, lp6_refresh_override.timing.tCK_ps),
            "LPDDR6 RFM must not be re-derived from an explicit RFC override");
    require(build_model("lpddr6", {{"nrfmab", "1500"}}, 3).timing.nRFMab == 1500,
            "LPDDR6 independent RFM research override was lost");
    const auto six_gb_pair = build_model("lpddr6", {{"density_gb", "3"}, {"rows", "12288"}});
    require(six_gb_pair.timing.nRFC == jedec::ns_to_nck(210, six_gb_pair.timing.tCK_ps),
            "3Gb/subchannel must select the defined 6Gb pair row");
    const auto missing_density = build_model("lpddr6", {{"density_gb", "12.5"}, {"rows", "51200"}});
    for (const auto& entry : missing_density.timing_table.entries)
      if (entry.name == "nRFC" || entry.name == "nRFCpb")
        require(entry.source == TimingValueSource::ResearchDefault,
                "absent Table 302 density must not be labeled JEDEC");
    rejects([] { build_model("hbm4", {{"rows", "2147483647"}, {"columns", "2147483647"}}, 3); });
    rejects([] { build_model("hbm4", {{"unsupported_timing", "10"}}, 3); });
    // Explicit zero and auto are different: zero is not silently replaced.
    rejects([] { build_model("hbm4", {{"data_rate_mbps", "0"}}, 3); });
    require(build_model("hbm4", {{"density_gb", "auto"}}, 3).density_gb == 32,
            "explicit auto density failed");
    for (int schema : {1, 2, 3}) {
      rejects([&] { build_model("hbm4", {{"density_gb", "16"}}, schema); });
      require(build_model("hbm4", {{"density_gb", "16"}, {"rows", "8192"}}, schema).density_gb == 16,
              "all schemas must accept consistent geometry/density");
      const auto clock = build_model("hbm4", {{"data_rate_mbps", "9000"}}, schema);
      require(clock.speed_bin_mbps == 9000 &&
              std::abs(clock.timing.tCK_ps - 4000000.0 / 9000) < 1e-9,
              "legacy syntax must use the current clock resolver");
      rejects([&] { build_model("hbm4", {{"nrc", "2"}}, schema); });
      rejects([&] { build_model("hbm4", {{"nras", "80"}, {"nrp", "40"}, {"nrc", "119"}}, schema); });
      require(build_model("hbm4", {{"nras", "80"}, {"nrp", "40"}, {"nrc", "120"}}, schema)
                  .timing.nRC == 120, "nRC equality boundary must be accepted");
    }
    auto library_model = make_spec("hbm4");
    apply_spec_overrides(library_model, {{"data_rate_mbps", "9000"}});
    require(std::abs(library_model.timing.tCK_ps - fast.timing.tCK_ps) < 1e-9,
            "library override path must use current clock coupling");
    rejects([&] { apply_spec_overrides(library_model, {{"nrc", "2"}}); });
    require(library_model.timing.nRC == fast.timing.nRC,
            "failed overrides must not mutate the caller's spec");
    auto invalid_row = make_spec("hbm4");
    invalid_row.timing.nRC = 2;
    rejects([&] { validate_spec(invalid_row); });
    for (const char* standard : {"hbm3", "hbm4", "lpddr5", "lpddr6"}) {
      const auto original = make_spec(standard);
      auto invalid = original;
      invalid.org.columns /= 2;
      rejects([&] { validate_spec(invalid); });
      invalid = original;
      invalid.timing.tCK_ps += 1;
      rejects([&] { validate_spec(invalid); });
      invalid = original;
      invalid.speed_bin_mbps += 1;
      rejects([&] { validate_spec(invalid); });
    }
    for (const char* standard : {"hbm4", "lpddr6"}) {
      const auto full = build_model(standard, {{"channels", "2"}});
      const MemorySystem system(full);
      require(system.controllers().size() == 2, "expected two internal Channel views");
      for (const auto& controller : system.controllers()) {
        const auto& local = controller.spec();
        require(local.org.channels == 1 && local.density_reference_channels == 2 &&
                local.density_gb == full.density_gb && local.timing.nRFC == full.timing.nRFC,
                "local Channel view must preserve full-device density and refresh timing");
        validate_spec(local);
      }
    }
    const auto derived_timing = build_model("hbm4", {{"nras", "80"}, {"nrp", "40"},
        {"trfcab_ns", "500"}}, 3);
    require(derived_timing.timing.nRC == 120 &&
            derived_timing.timing.nRFMab == derived_timing.timing.nRFC,
            "explicit primary timing did not update omitted dependent constraint");
    require(build_model("hbm4", {{"nras", "80"}, {"nrp", "40"}, {"nrc", "125"}}, 3)
                .timing.nRC == 125, "explicit nRC must stay independently configurable");
    rejects([] { build_model("hbm4", {{"nrp", "30"}, {"trp_ns", "15"}}, 3); });
    for (const auto& entry : derived_timing.timing_table.entries)
      if (entry.name == "nRC")
        require(entry.source == TimingValueSource::ResearchDefault,
                "arithmetic must not certify research dependencies");
    const auto named_hbm = build_model("hbm4", {{"mode_profile", "link_crc"},
        {"vendor_profile", "imaginary_vendor"}}, 3);
    require(named_hbm.hbm_link_crc_bits_per_request == 0,
            "audit name must not enable HBM CRC");
    require(!validate_timing_table(named_hbm, true).empty(),
            "vendor label must not certify built-in research timings");
    const auto named_lp = build_model("lpddr6", {{"mode_profile", "linkprot_on_eff_on_ca_parity"}}, 3);
    const auto plain_lp = build_model("lpddr6", {}, 3);
    require(!named_lp.lpddr_ca_parity_enabled && !named_lp.lpddr_link_protection &&
            named_lp.timing.nWR == plain_lp.timing.nWR,
            "audit name must not enable LPDDR features or switch its timing branch");
    std::cout << "model config tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
