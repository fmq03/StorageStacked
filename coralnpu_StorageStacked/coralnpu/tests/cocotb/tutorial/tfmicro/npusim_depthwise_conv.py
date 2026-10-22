from bazel_tools.tools.python.runfiles import runfiles
from coralnpu_v2_sim_utils import CoralNPUV2Simulator
import numpy as np


def tolerate(target: int, variance=0.2) -> int:
    return int(target * (1 + variance))


class MpactDepthwiseConvTest:

    def __init__(self, in_d, dm=1, stride=1, out_h=4, out_w=4):
        self.dm = dm
        self.stride = stride
        out_d = in_d * dm
        in_h = out_h * stride
        in_w = out_w * stride
        self.in_shape = np.array([1, in_h, in_w, in_d], dtype=np.uint32)
        self.f_shape = np.array([1, 3, 3, out_d], dtype=np.uint32)
        self.bias_shape = np.array([out_d], dtype=np.uint32)
        self.out_shape = np.array([1, out_h, out_w, out_d], dtype=np.uint32)
        self.out_size = int(np.prod(self.out_shape))

        r = runfiles.Create()
        self.elf_file = r.Rlocation(
            'coralnpu_hw/tests/cocotb/tutorial/tfmicro/depthwise_conv_test.elf'
        )

    def load_and_populate_input(self):

        self.npu_sim = CoralNPUV2Simulator(highmem_ld=True)
        self.entry_point, self.symbol_map = self.npu_sim.get_elf_entry_and_symbol(
            self.elf_file, [
                'impl',
                'run_ref',
                'run_optimized',
                'dm',
                'stride',
                'filter_shape',
                'filter_data',
                'bias_shape',
                'bias_data',
                'input_shape',
                'input_data',
                'output_shape',
                'output_data',
            ]
        )
        self.npu_sim.load_program(self.elf_file)
        rng = np.random.default_rng()
        self.filter_data = rng.integers(-128, 128, self.f_shape, dtype=np.int8)
        filter_data_flat = self.filter_data.flatten()
        bias_data = rng.integers(
            -100000, 100000, self.out_shape[3], dtype=np.int32
        )
        input_data = rng.integers(
            -128, 128, self.in_shape, dtype=np.int8
        ).flatten()
        self.npu_sim.write_word(
            self.symbol_map['stride'], np.uint32(self.stride)
        )
        self.npu_sim.write_word(self.symbol_map['dm'], np.uint32(self.dm))
        self.npu_sim.write_memory(
            self.symbol_map['filter_shape'], self.f_shape
        )
        self.npu_sim.write_memory(
            self.symbol_map['filter_data'], filter_data_flat
        )
        self.npu_sim.write_memory(
            self.symbol_map['bias_shape'], self.bias_shape
        )
        self.npu_sim.write_memory(self.symbol_map['bias_data'], bias_data)
        self.npu_sim.write_memory(
            self.symbol_map['input_shape'], self.in_shape
        )
        self.npu_sim.write_memory(self.symbol_map['input_data'], input_data)
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

    def test(self, ref_target, opt_target):
        opt_cycles, ref_outputs = self.run(fun_ptr="run_optimized")
        ref_cycles, opt_outputs = self.run(fun_ptr="run_ref")
        print(f"opt_cycles {opt_cycles}")
        print(f"ref_cycles {ref_cycles}")
        assert (opt_outputs == ref_outputs).all()
        assert opt_cycles < tolerate(opt_target)
        assert ref_cycles < tolerate(ref_target)


def run_tests():

    print("Running functional tests...")
    print("test_dwconv8to8stride1")
    t = MpactDepthwiseConvTest(in_d=32)
    t.load_and_populate_input()
    t.test(ref_target=171_880, opt_target=7_907)

    print("test_dwconv32to32stride2")
    t = MpactDepthwiseConvTest(in_d=32, stride=2)
    t.load_and_populate_input()
    t.test(ref_target=182_500, opt_target=7_752)

    print("test_dwconv64to64stride1")
    t = MpactDepthwiseConvTest(in_d=64)
    t.load_and_populate_input()
    t.test(ref_target=337_804, opt_target=10_800)

    print("test_dwconv64to64stride2")
    t = MpactDepthwiseConvTest(in_d=64, stride=2)
    t.load_and_populate_input()
    t.test(ref_target=359_251, opt_target=10_675)

    print("test_dwconv16to32stride2")
    t = MpactDepthwiseConvTest(in_d=16, dm=2, stride=2)
    t.load_and_populate_input()
    t.test(ref_target=177_961, opt_target=9_586)


if __name__ == "__main__":
    run_tests()
