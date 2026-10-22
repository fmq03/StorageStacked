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


def golden_rms_norm(x, w, eps=1e-6):
    # PyTorch/Gemma style RMS Norm: x * rsqrt(mean(x^2) + eps) * (1 + w)
    # x: (seq_len, hidden_size)
    # w: (hidden_size,)
    # reference : https://github.com/google/gemma_pytorch/blob/main/gemma/model.py#L179
    rms = np.sqrt(np.mean(x**2, axis=-1, keepdims=True) + eps)
    return x / rms * (1.0 + w)


@cocotb.test()
async def core_mini_rvv_rms_norm_test(dut):
    r = runfiles.Create()
    # Highmem configuration maps CSRs dynamically via highmem flag
    fixture = await Fixture.Create(dut, highmem=True)

    elf_name = "rvv_rms_norm.elf"
    elf_path = r.Rlocation(
        f"coralnpu_hw/tests/cocotb/rvv/ml_ops/gemma_kernels/{elf_name}"
    )

    if not elf_path or not os.path.exists(elf_path):
        raise FileNotFoundError(f"Could not find ELF at {elf_path}")

    dut._log.info(f"Loading ELF: {elf_path}")
    await fixture.load_elf_and_lookup_symbols(
        elf_path, [
            "rms_input", "rms_weight", "rms_output", "active_seq_len",
            "active_hidden_size", "active_epsilon", "cycle_count"
        ]
    )

    await fixture.core_mini_axi.reset()

    # Define test shapes:
    # 1. (11, 640)  -> Real Gemma 3 270M Prefill
    # 2. (1, 640)   -> Real Gemma 3 270M Decode (Single token)
    # 3. (5, 643)   -> Odd hidden size (testing tail-undisturbed logic for non-vector multiples)
    # 4. (1, 2048)  -> Gemma 2B Decode
    # 5. (2, 2048)  -> Gemma 2B Prefill
    test_shapes = [
        (11, 640),
        (1, 640),
        (5, 643),
        (1, 2048),
        (2, 2048),
    ]

    rng = np.random.default_rng(seed=42)

    for seq_len, hidden_size in test_shapes:
        dut._log.info(f"\nRunning shape: {seq_len}x{hidden_size}")

        input_data = rng.uniform(-1.0, 1.0,
                                 (seq_len, hidden_size)).astype(np.float32)
        weight_data = rng.uniform(-0.5, 0.5,
                                  (hidden_size, )).astype(np.float32)

        expected_output = golden_rms_norm(input_data, weight_data, eps=1e-6)

        await fixture.write(
            'active_seq_len', np.array([seq_len], dtype=np.uint32)
        )
        await fixture.write(
            'active_hidden_size', np.array([hidden_size], dtype=np.uint32)
        )
        await fixture.write(
            'active_epsilon', np.array([1e-6], dtype=np.float32)
        )

        # Write input arrays to the simulated memory
        await fixture.write('rms_input', input_data.flatten())
        await fixture.write('rms_weight', weight_data.flatten())
        # Zero out output array
        await fixture.write(
            'rms_output',
            np.zeros_like(expected_output).flatten()
        )

        sim_cycles = await fixture.run_to_halt(timeout_cycles=10000000)

        npu_cycles = int((await fixture.read('cycle_count',
                                             4)).view(dtype=np.uint32)[0])

        output_size_bytes = seq_len * hidden_size * 4
        actual_output = (await fixture.read('rms_output',
                                            output_size_bytes)).view(
                                                dtype=np.float32
                                            ).reshape(seq_len, hidden_size)

        np.testing.assert_allclose(
            expected_output, actual_output, rtol=1e-6, atol=1e-6
        )

        total_elements = seq_len * hidden_size
        log_vector_metrics(
            dut, f"RMS Norm Shape: {seq_len}x{hidden_size}", npu_cycles,
            total_elements
        )
