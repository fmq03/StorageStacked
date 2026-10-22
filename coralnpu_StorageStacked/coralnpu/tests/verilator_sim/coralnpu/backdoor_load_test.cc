
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "tests/verilator_sim/coralnpu/core_mini_axi_tb.h"
#include "tests/verilator_sim/util.h"

/* clang-format off */
#include <systemc>
#include "traffic-generators/traffic-desc.h"
namespace internal {
#include "tests/test-modules/utils.h"
}
using namespace internal;
/* clang-format on */

ABSL_FLAG(bool, trace, false, "Enable tracing");

int sc_main(int argc, char** argv) {
  std::vector<char*> v_argv;
  for (int i = 0; i < argc; ++i) {
    if (std::string(argv[i]) != "--trace") {
      v_argv.push_back(argv[i]);
    }
  }
  int v_argc = v_argv.size();

  absl::ParseCommandLine(argc, argv);
  Verilated::commandArgs(v_argc, v_argv.data());

  CoreMiniAxi_tb tb("CoreMiniAxi_tb", 1000, /* random= */ false,
                    /*debug_axi=*/true, /*instr_trace=*/false,
                    /*backdoor_load=*/true,
                    /*wfi_cb=*/std::nullopt, std::nullopt);
  sc_start(SC_ZERO_TIME);

  uint8_t itcm_data[32];
  uint8_t dtcm_data[32];
  for (int i = 0; i < 32; i++) {
    itcm_data[i] = 0xA0 + i;
    dtcm_data[i] = 0xB0 + i;
  }

  tb.BackdoorLoad(0, itcm_data, 32);
  tb.BackdoorLoad(0x10000, dtcm_data, 32);

  if (absl::GetFlag(FLAGS_trace)) {
    tb.trace(tb.core());
  }

  std::vector<DataTransfer> read_transfers;
  read_transfers.push_back(utils::Read(0, 32));
  read_transfers.push_back(utils::Expect(itcm_data, 32));
  read_transfers.push_back(utils::Read(0x10000, 32));
  read_transfers.push_back(utils::Expect(dtcm_data, 32));

  tb.EnqueueTransactionAsync(read_transfers);

  tb.start();

  return 0;
}
