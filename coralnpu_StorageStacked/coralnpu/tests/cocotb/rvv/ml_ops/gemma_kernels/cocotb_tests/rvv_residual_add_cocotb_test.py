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
async def core_mini_rvv_residual_add_test(dut):
    r = runfiles.Create()

    # We need highmem because Gemma 3 270M prefill shape (256x640) takes ~1.9MB across 3 tensors
    fixture = await Fixture.Create(
        dut,
        highmem=True,
        ext_mem_base_addr=0x80000000,
        ext_mem_size=32 * 1024 * 1024
    )

    elf_name = "rvv_residual_add.elf"
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
        elf_path, ["A", "B", "Y", "active_elements", "cycle_count"]
    )

    await fixture.core_mini_axi.reset()

    test_shapes = [
        (1, 640),  # Decode Phase
        (256, 640),  # Prefill Phase
    ]

    rng = np.random.default_rng(seed=42)

    for token_count, hidden_size in test_shapes:
        dut._log.info(f"\nRunning shape: [{token_count}, {hidden_size}]")
        total_elements = token_count * hidden_size

        A_data = rng.uniform(-1.0, 1.0,
                             (token_count, hidden_size)).astype(np.float32)
        B_data = rng.uniform(-0.5, 0.5,
                             (token_count, hidden_size)).astype(np.float32)

        expected_output = np.add(A_data, B_data)

        await fixture.write(
            'active_elements', np.array([total_elements], dtype=np.uint32)
        )
        await fixture.write('A', A_data.flatten())
        await fixture.write('B', B_data.flatten())

        await fixture.write('Y', np.zeros_like(expected_output).flatten())

        sim_cycles = await fixture.run_to_halt(timeout_cycles=20000000)

        npu_cycles = int((await fixture.read('cycle_count',
                                             4)).view(dtype=np.uint32)[0])

        output_size_bytes = total_elements * 4
        actual_output = (await fixture.read('Y', output_size_bytes)).view(
            dtype=np.float32
        ).reshape(token_count, hidden_size)

        np.testing.assert_allclose(
            expected_output, actual_output, rtol=1e-5, atol=1e-5
        )

        log_vector_metrics(
            dut, f"Residual Add Shape: [{token_count}, {hidden_size}]",
            npu_cycles, total_elements
        )
