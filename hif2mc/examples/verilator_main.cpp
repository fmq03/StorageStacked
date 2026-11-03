#include "VHifDemoMaster.h"
#include "hbm_sim/config/model.hpp"
#include "hif2mc/adapter.hpp"
#include "hif2mc/memsim_backend.hpp"
#include "hif2mc/verilator_binding.hpp"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <iostream>

int main(int argc, char **argv) {
  try {
    VerilatedContext context;
    context.commandArgs(argc, argv);
    VHifDemoMaster top(&context);
    VerilatedVcdC trace;
    const bool tracing = argc == 2;
    if (tracing) {
      context.traceEverOn(true);
      top.trace(&trace, 5);
      trace.open(argv[1]);
    }
    auto c = hif2mc::Config::reference_64b();
    auto spec = hbm_sim::config::build_model("hbm4");
    hbm_sim::MemorySystemOptions options;
    options.controller.phy.mode = hbm_sim::MemPhyMode::Behavioral;
    options.host_response_queue_capacity = c.max_inflight;
    hif2mc::Adapter adapter(
        c, std::make_unique<hif2mc::MemSimBackend>(spec, options));
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
      top.core_ddrc_core_clk = 0;
      top.core_ddrc_rstn = cycle >= 2;
      top.eval(); // settle the master's combinational outputs before sampling
      auto inputs = hif2mc::sample_hif_inputs(top);
      hif2mc::drive_hif_outputs(top, adapter.evaluate(inputs));
      top.eval();
      inputs = hif2mc::sample_hif_inputs(top);
      // The small master has no combinational valid<-stall dependency. Both
      // parties consume the same pre-edge values exactly once.
      if (tracing)
        trace.dump(context.time());
      context.timeInc(c.hif_period_ps / 2);
      top.core_ddrc_core_clk = 1;
      top.eval();
      adapter.tick(inputs);
      if (tracing)
        trace.dump(context.time());
      context.timeInc(c.hif_period_ps / 2);
      if (top.failed)
        throw std::runtime_error(
            "RTL HIF master reported data/protocol mismatch");
      if (top.done && adapter.idle()) {
        adapter.finish();
        top.final();
        if (tracing)
          trace.close();
        std::cout << "HIF2MC VERILATOR PASS cycles=" << cycle + 1 << '\n';
        return 0;
      }
    }
    throw std::runtime_error("Verilator HIF demo timeout");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
