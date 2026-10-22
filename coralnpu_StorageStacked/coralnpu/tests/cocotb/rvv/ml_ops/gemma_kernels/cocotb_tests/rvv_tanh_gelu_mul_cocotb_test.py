# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Test suite for RVV Gemma Kernels using Cocotb."""

import os
import cocotb
import numpy as np
from bazel_tools.tools.python.runfiles import runfiles

from coralnpu_test_utils.sim_test_fixture import Fixture
from sw.utils.metrics import log_vector_metrics


@cocotb.test()
async def core_mini_rvv_tanh_gelu_mul_test(dut):
    r = runfiles.Create()

    # Needs highmem due to 6.29MB memory requirement for 256x2048 shape
    fixture = await Fixture.Create(
        dut,
        highmem=True,
        ext_mem_base_addr=0x80000000,
        ext_mem_size=32 * 1024 * 1024
    )

    elf_name = "rvv_tanh_gelu_mul.elf"
    elf_path = r.Rlocation(
        f"coralnpu_hw/tests/cocotb/rvv/ml_ops/gemma_kernels/{elf_name}"
    )

    if not elf_path or not os.path.exists(elf_path):
        dut._log.info(
            f"Skipping test because ELF not found in sandbox: {elf_name}"
        )
        return

    dut._log.info(f"Loading ELF: {elf_path}")
    await fixture.load_elf_and_lookup_symbols(
        elf_path, ["Gate", "Up", "Output", "active_elements", "cycle_count"]
    )

    await fixture.core_mini_axi.reset()

    # Gemma 3 270M Specific FFN Shapes
    # We add proportional chunks (scaled down for RTL simulation speed)
    test_shapes = [
        (1, 48),  # Decode Phase
        (8, 2048),  # Small chunk
        (32, 2048),  # 1/8th proportioned chunk for faster simulation
    ]

    rng = np.random.default_rng(seed=42)

    for token_count, hidden_size in test_shapes:
        dut._log.info(f"\nRunning shape: [{token_count}, {hidden_size}]")
        total_elements = token_count * hidden_size

        Gate_data = rng.uniform(-1.0, 1.0,
                                (token_count, hidden_size)).astype(np.float32)
        Up_data = rng.uniform(-1.0, 1.0,
                              (token_count, hidden_size)).astype(np.float32)

        # Numpy equivalent of the exact Vectorized Rational Approximation used in hardware:
        x = Gate_data
        sqrt_2_over_pi = np.sqrt(2.0 / np.pi)
        z = sqrt_2_over_pi * (x + 0.044715 * np.power(x, 3))

        y = np.clip(z, -3.0, 3.0)
        y2 = y * y
        approx_tanh = (y * (27.0 + y2)) / (27.0 + 9.0 * y2)

        gelu_out = 0.5 * x * (1.0 + approx_tanh)
        expected_output = gelu_out * Up_data

        await fixture.write(
            'active_elements', np.array([total_elements], dtype=np.uint32)
        )
        await fixture.write('Gate', Gate_data.flatten())
        await fixture.write('Up', Up_data.flatten())
        await fixture.write('Output', np.zeros_like(expected_output).flatten())

        sim_cycles = await fixture.run_to_halt(timeout_cycles=30000000)
        npu_cycles = int((await fixture.read('cycle_count',
                                             4)).view(dtype=np.uint32)[0])

        log_vector_metrics(
            dut, f"Tanh-GELU Mul Shape: [{token_count}, {hidden_size}]",
            npu_cycles, total_elements
        )

        actual_output = (await fixture.read('Output', total_elements *
                                            4)).view(dtype=np.float32)

        np.testing.assert_allclose(
            actual_output, expected_output.flatten(), rtol=1e-3, atol=1e-3
        )
