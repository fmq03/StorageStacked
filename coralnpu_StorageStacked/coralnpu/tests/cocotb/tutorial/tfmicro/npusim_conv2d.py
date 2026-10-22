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

from bazel_tools.tools.python.runfiles import runfiles
from coralnpu_v2_sim_utils import CoralNPUV2Simulator
import numpy as np


class MpactConv2DTest:

    def __init__(self, in_d, out_d, stride=1, out_h=4, out_w=4):
        self.stride = stride
        in_h = out_h * stride
        in_w = out_w * stride
        self.in_shape = np.array([1, in_h, in_w, in_d], dtype=np.uint32)
        self.f_shape = np.array([out_d, 4, 4, in_d], dtype=np.uint32)
        self.bias_shape = np.array([out_d], dtype=np.uint32)
        self.out_shape = np.array([1, out_h, out_w, out_d], dtype=np.uint32)
        self.out_size = int(np.prod(self.out_shape))

        r = runfiles.Create()
        self.elf_file = r.Rlocation(
            'coralnpu_hw/tests/cocotb/tutorial/tfmicro/conv2d_test.elf'
        )

    def load_and_populate_input(self):
        self.npu_sim = CoralNPUV2Simulator(highmem_ld=True)
        self.entry_point, self.symbol_map = self.npu_sim.get_elf_entry_and_symbol(
            self.elf_file, [
                'impl',
                'run_ref',
                'run_opt',
                'stride',
                'filter_shape',
                'filter_data',
                'bias_shape',
                'bias_data',
                'input_shape',
                'input_data',
                'output_shape',
                'output_data',
                'params',
            ]
        )
        self.npu_sim.load_program(self.elf_file)
        rng = np.random.default_rng()
        filter_data = rng.integers(
            -128, 128, self.f_shape, dtype=np.int8
        ).flatten()
        bias_data = rng.integers(
            -100000, 100000, self.out_shape[3], dtype=np.int32
        )
        input_data = rng.integers(
            -128, 128, self.in_shape, dtype=np.int8
        ).flatten()

        self.npu_sim.write_word(
            self.symbol_map['stride'], np.uint32(self.stride)
        )
        self.npu_sim.write_memory(
            self.symbol_map['filter_shape'], self.f_shape
        )
        self.npu_sim.write_memory(self.symbol_map['filter_data'], filter_data)

        self.npu_sim.write_memory(
            self.symbol_map['bias_shape'], self.bias_shape
        )
        self.npu_sim.write_memory(self.symbol_map['bias_data'], bias_data)
        self.npu_sim.write_memory(
            self.symbol_map['input_shape'], self.in_shape
        )
        self.npu_sim.write_memory(self.symbol_map['input_data'], input_data)

        # Verify input_data integrity
        read_back_input = self.npu_sim.read_memory(
            self.symbol_map['input_data'], len(input_data)
        ).view(np.int8)
        if not (read_back_input == input_data).all():
            print("Input data mismatch during load!")
            raise AssertionError("Input data corrupted during write_memory")
        self.npu_sim.write_memory(
            self.symbol_map['output_shape'], self.out_shape
        )

    def run(self, fun_ptr):
        self.npu_sim.write_register('pc', self.entry_point)
        self.npu_sim.write_ptr(
            self.symbol_map['impl'], self.symbol_map[fun_ptr]
        )
        self.npu_sim.write_memory(
            self.symbol_map['output_data'],
            np.zeros([self.out_size], dtype=np.int8)
        )
        self.npu_sim.run()
        self.npu_sim.wait()
        cycles = self.npu_sim.get_cycle_count()
        outputs = self.npu_sim.read_memory(
            self.symbol_map['output_data'], self.out_size
        ).view(np.int8)
        return cycles, outputs

    def test(self):
        opt_cycles, opt_outputs = self.run(fun_ptr="run_opt")
        ref_cycles, ref_outputs = self.run(fun_ptr="run_ref")
        print(f"ref_cycles {ref_cycles} opt_cycles {opt_cycles}")
        assert (opt_outputs == ref_outputs).all()


def run_tests():

    print("test_conv2d_16x1")
    t = MpactConv2DTest(in_d=16, out_d=1, stride=1, out_h=4, out_w=4)
    t.load_and_populate_input()
    t.test()

    print("test_conv2d_16x16")
    t = MpactConv2DTest(in_d=16, out_d=16, stride=1, out_h=4, out_w=4)
    t.load_and_populate_input()
    t.test()

    print("test_conv2d_16x16_s2_h8w8")
    t = MpactConv2DTest(in_d=16, out_d=16, stride=2, out_h=8, out_w=8)
    t.load_and_populate_input()
    t.test()

    print("test_conv2d_48x5")
    # Using 500k as target based on earlier cocotb attempt failing at 0
    t = MpactConv2DTest(in_d=48, out_d=5, stride=1, out_h=8, out_w=8)
    t.load_and_populate_input()
    t.test()

    print("test_conv2d_21x16")
    # Using 500k as target based on earlier cocotb attempt failing at 0
    t = MpactConv2DTest(in_d=21, out_d=16, stride=1, out_h=2, out_w=2)
    t.load_and_populate_input()
    t.test()


if __name__ == "__main__":
    run_tests()
