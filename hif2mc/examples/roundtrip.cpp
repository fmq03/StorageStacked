#include "hbm_sim/config/model.hpp"
#include "hif2mc/memsim_backend.hpp"
#include "hif_master.hpp"
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  try {
    const auto config = hif2mc::Config::reference_64b();
    auto spec = hbm_sim::config::build_model("hbm4");
    hbm_sim::MemorySystemOptions options;
    options.controller.phy.mode = hbm_sim::MemPhyMode::Behavioral;
    options.controller.phy.reset_cycles = 4;
    options.controller.phy.initialization_cycles = 4;
    options.controller.phy.training_cycles = 8;
    options.host_response_queue_capacity = config.max_inflight;
    auto backend = std::make_unique<hif2mc::MemSimBackend>(spec, options);
    auto *observed = backend.get();
    hif2mc::Adapter adapter(config, std::move(backend));
    hif2mc::example::Master master(adapter, config);
    const auto original = hif2mc::example::pattern(64, 17);
    const auto newer = hif2mc::example::pattern(64, 83);
    hif2mc::Bytes mask(64);
    for (std::size_t i = 0; i < 64; ++i)
      mask[i] = i % 3 == 0;
    const std::uint64_t token = (std::uint64_t(1) << 52) | 0x123;
    master.write(0x1000, 22, original, hif2mc::Bytes(64, 0xff));
    master.read(0x1000, token, true); // queued before write data is collected
    master.run();
    if (master.reads[token] != original)
      throw std::runtime_error("initial write/read mismatch");
    master.write(0x1000, 21, newer, mask, true);
    master.read(0x1000, token + 1);
    master.run();
    auto expected = original;
    for (std::size_t i = 0; i < 64; ++i)
      if (mask[i])
        expected[i] = newer[i];
    if (master.reads[token + 1] != expected)
      throw std::runtime_error("RMW/read mismatch");
    adapter.finish();
    const auto &stats = adapter.stats();
    const auto &mem = observed->memory_system().stats();
    if (!mem.phy_read_completions || !mem.phy_write_completions)
      throw std::runtime_error("behavioral PHY was not exercised");
    if (argc == 2) {
      std::ofstream csv(argv[1]);
      if (!csv)
        throw std::runtime_error("cannot open CSV output");
      csv << "cycle,cmd_valid,cmd_stall,cmd_type,cmd_addr,cmd_token,cmd_"
             "pointer,ptr_valid,ptr,ptr_addr_err,wvalid,wstall,wend,rvalid,"
             "rend,rtoken,lpr_credit,hpr_credit,wr_credit\n";
      for (const auto &s : master.samples)
        csv << s.cycle << ',' << s.in.hif_cmd_valid << ','
            << s.out.hif_cmd_stall << ',' << unsigned(s.in.hif_cmd_type) << ','
            << s.in.hif_cmd_addr << ',' << s.in.hif_cmd_token << ','
            << s.in.hif_cmd_wdata_ptr << ',' << s.out.hif_wdata_ptr_valid << ','
            << s.out.hif_wdata_ptr << ',' << s.out.hif_wdata_ptr_addr_err << ','
            << s.in.hif_wdata_valid << ',' << s.out.hif_wdata_stall << ','
            << s.in.hif_wdata_end << ',' << s.out.hif_rdata_valid << ','
            << s.out.hif_rdata_end << ',' << s.out.hif_rdata_token << ','
            << unsigned(s.out.hif_lpr_credit) << ','
            << unsigned(s.out.hif_hpr_credit) << ',' << s.out.hif_wr_credit
            << '\n';
      if (!csv)
        throw std::runtime_error("CSV write failed");
    }
    std::cout << "HIF2MC ROUNDTRIP PASS\n"
              << "HIF cycles=" << adapter.cycle()
              << " backend_ticks=" << stats.backend_ticks
              << " reads=" << stats.completed_reads
              << " writes=" << stats.completed_writes
              << " rmws=" << stats.completed_rmws
              << " children=" << stats.submitted_children << '\n'
              << "PHY read_completions=" << mem.phy_read_completions
              << " write_completions=" << mem.phy_write_completions << '\n';
  } catch (const std::exception &e) {
    std::cerr << "hif2mc: " << e.what() << '\n';
    return 1;
  }
}
