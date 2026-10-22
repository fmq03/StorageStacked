# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import threading
import time
import unittest

from bazel_tools.tools.python.runfiles import runfiles
from coralnpu_v2_sim_utils import CoralNPUV2Simulator
import numpy as np


class TestCoralNPUV2SimPybind(unittest.TestCase):

    def setUp(self):
        self.sim = CoralNPUV2Simulator()
        self.r = runfiles.Create()
        self.elf_path = self.r.Rlocation(
            "coralnpu_hw/tests/cocotb/rvv/arithmetics/rvv_add_int8_m1.elf"
        )

    def _load_program(self):
        """Loads the test ELF program and returns entry point & symbol map."""
        entry_point, symbol_map = self.sim.get_elf_entry_and_symbol(
            self.elf_path, ["in_buf_1", "in_buf_2", "out_buf"]
        )
        self.sim.load_program(self.elf_path, entry_point)
        return entry_point, symbol_map

    def _read_pc(self):
        """Helper to read the current PC value."""
        return int(self.sim.read_register("pc"), 16)

    def test_add_kernel(self):
        """Verifies end-to-end execution of a vector add kernel."""
        _, symbol_map = self._load_program()
        # 1. Prepare Input
        input_size = 16
        in1 = np.arange(input_size, dtype=np.uint8)
        in2 = np.arange(input_size, dtype=np.uint8) * 2
        self.sim.write_memory(symbol_map["in_buf_1"], in1)
        self.sim.write_memory(symbol_map["in_buf_2"], in2)

        # 2. Run
        self.sim.run()
        self.sim.wait()

        # 3. Verify Output
        expected_out = in1 + in2
        actual_out = self.sim.read_memory(symbol_map["out_buf"], input_size)
        np.testing.assert_array_equal(
            actual_out, expected_out, "Output should match expected sum"
        )

        # 4. Verify Cycles
        cycle_count = self.sim.get_cycle_count()
        # Cycle count observed around 148
        self.assertTrue(
            140 < cycle_count < 160,
            f"Cycle count {cycle_count} should be ~148"
        )

    def test_check_input_type(self):
        """Verifies inputs are validated."""
        with self.assertRaisesRegex(TypeError, "data must be a numpy array"):
            self.sim.write_memory(0x1000, [1, 2, 3])

    def test_step(self):
        """Verifies single stepping."""
        self._load_program()
        self.assertEqual(self.sim.step(10), 10)
        self.assertGreater(self.sim.get_cycle_count(), 0)

    def test_read_write_register(self):
        """Verifies register access."""
        self._load_program()

        # Write & Read Back
        test_val = 0xDEADBEEF
        self.sim.write_register("t0", test_val)
        self.assertEqual(int(self.sim.read_register("t0"), 16), test_val)

    def test_breakpoint(self):
        """Verifies software breakpoints pause execution."""
        entry_point, _ = self._load_program()

        # 1. Set Breakpoint & Run
        self.sim.set_sw_breakpoint(entry_point)
        self.sim.run()
        self.sim.wait()

        # Should stop at entry point
        self.assertEqual(
            self._read_pc(), entry_point, "Simulator should stop at breakpoint"
        )

        # 2. Clear Breakpoint & Resume
        self.sim.clear_sw_breakpoint(entry_point)
        self.sim.run()
        self.sim.wait()

        # Should have advanced past entry point
        self.assertNotEqual(
            self._read_pc(), entry_point,
            "Simulator should resume after clearing breakpoint"
        )

    def test_halt(self):
        """Verifies asynchronous halt."""
        self._load_program()

        # Run is non-blocking
        self.sim.run()

        # Allow some execution time then Halt
        time.sleep(0.1)
        self.sim.halt()
        self.sim.wait()

        # Verify execution stopped
        cycle_count_before = self.sim.get_cycle_count()
        time.sleep(0.1)
        cycle_count_after = self.sim.get_cycle_count()
        self.assertEqual(
            cycle_count_before, cycle_count_after, "Simulator failed to halt"
        )

    def test_htif_semihosting(self):
        """Verifies HTIF semihosting functionality."""
        htif_elf_path = self.r.Rlocation(
            "coralnpu_hw/tests/verilator_sim/htif_semihosting_test.elf"
        )
        self.assertTrue(htif_elf_path is not None, "HTIF ELF not found")

        # Initialize simulator with semihost_htif enabled
        sim = CoralNPUV2Simulator(semihost_htif=True)
        entry_point, _ = sim.get_elf_entry_and_symbol(htif_elf_path, [])
        sim.load_program(htif_elf_path, entry_point)

        # Run the simulator
        sim.run()
        sim.wait()

        # If it finishes, it means EBREAK was hit (which semihosting uses for exit)
        # or the simulation reached its end.
        self.assertGreater(sim.get_cycle_count(), 0)


if __name__ == "__main__":
    import sys
    import os

    # Bazel passes the test filter in the TESTBRIDGE_TEST_ONLY environment variable.
    # If it's set, we can use it to filter the tests.
    if "TESTBRIDGE_TEST_ONLY" in os.environ:
        test_filter = os.environ["TESTBRIDGE_TEST_ONLY"]
        # unittest.main expects the filter as a positional argument.
        # We replace any '.' with '/' if it's a full path, but usually it's just Class.method
        # which unittest handles if passed as a positional arg.
        argv = [sys.argv[0], test_filter]
        unittest.main(argv=argv)
    else:
        unittest.main()
