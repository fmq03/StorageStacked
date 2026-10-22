# Copyright 2026 Google LLC
import os
import sys
import numpy as np
from bazel_tools.tools.python.runfiles import runfiles
from coralnpu_v2_sim_utils import CoralNPUV2Simulator


def run_test_case(
    npu_sim,
    elf_file,
    input_shape,
    filter_shape,
    stride,
    padding_type="VALID"
):
    # Symbols to resolve
    symbols = [
        "input_dims",
        "input_data",
        "filter_dims",
        "filter_data",
        "bias_dims",
        "bias_data",
        "output_dims",
        "output_data",
        "output_data_ref",
        "params_stride_width",
        "params_stride_height",
        "params_padding_width",
        "params_padding_height",
        "params_input_offset",
        "params_output_offset",
        "params_activation_min",
        "params_activation_max",
        "per_channel_multiplier",
        "per_channel_shift",
        "ref_cycles",
        "opt_cycles",
        "heartbeat",
    ]

    entry_point, symbol_map = npu_sim.get_elf_entry_and_symbol(
        elf_file, symbols
    )

    # Initialize simulator
    npu_sim.load_program(elf_file, entry_point)

    # Parameters
    filter_height = filter_shape[1]
    filter_width = filter_shape[2]
    input_depth = input_shape[3]
    output_depth = filter_shape[0]
    stride_height, stride_width = stride

    input_offset = 128
    output_offset = -128
    activation_min = -128
    activation_max = 127

    if padding_type == "SAME":
        out_h = (input_shape[1] + stride_height - 1) // stride_height
        out_w = (input_shape[2] + stride_width - 1) // stride_width
        pad_h = (
            max(
                0, (out_h - 1) * stride_height + filter_height - input_shape[1]
            ) // 2
        )
        pad_w = max(
            0, (out_w - 1) * stride_width + filter_width - input_shape[2]
        ) // 2
    else:  # VALID
        out_h = (input_shape[1] - filter_height) // stride_height + 1
        out_w = (input_shape[2] - filter_width) // stride_width + 1
        pad_h = 0
        pad_w = 0

    output_shape = [input_shape[0], out_h, out_w, output_depth]

    print(
        f"Testing: Input {input_shape}, Filter {filter_shape}, Stride {stride}, Padding {padding_type} -> Output {output_shape}",
        flush=True,
    )

    # Generate random data
    input_data = np.random.randint(-128, 127, size=input_shape, dtype=np.int8)
    filter_data = np.random.randint(
        -128, 127, size=filter_shape, dtype=np.int8
    )
    bias_data = np.random.randint(
        -1000, 1000, size=(output_depth, ), dtype=np.int32
    )
    per_channel_multiplier = np.random.randint(
        1073741824, 2147483647, size=(output_depth, ), dtype=np.int32
    )
    per_channel_shift = np.random.randint(
        -10, -1, size=(output_depth, ), dtype=np.int32
    )

    # Helper to write data
    def write_symbol_data(name, data):
        addr = symbol_map.get(name)
        if addr is None:
            raise ValueError(f"Symbol {name} not found")
        if isinstance(data, np.ndarray):
            pass
        elif isinstance(data, (bytes, bytearray)):
            data = np.frombuffer(data, dtype=np.uint8)
        elif isinstance(data, (int, float)):
            data = int(data).to_bytes(4, byteorder="little", signed=True)
            data = np.frombuffer(data, dtype=np.uint8)
        else:
            data = np.array(data, dtype=np.uint8)
        npu_sim.write_memory(addr, data)

    def read_symbol_val(name, size=4):
        addr = symbol_map.get(name)
        val = npu_sim.read_memory(addr, size)
        return int.from_bytes(val, "little")

    write_symbol_data("input_dims", np.array(input_shape, dtype=np.int32))
    write_symbol_data("input_data", input_data)
    write_symbol_data("filter_dims", np.array(filter_shape, dtype=np.int32))
    write_symbol_data("filter_data", filter_data)
    write_symbol_data("bias_dims", np.array([output_depth], dtype=np.int32))
    write_symbol_data("bias_data", bias_data)
    write_symbol_data("output_dims", np.array(output_shape, dtype=np.int32))

    write_symbol_data("params_stride_width", stride_width)
    write_symbol_data("params_stride_height", stride_height)
    write_symbol_data("params_padding_width", pad_w)
    write_symbol_data("params_padding_height", pad_h)
    write_symbol_data("params_input_offset", input_offset)
    write_symbol_data("params_output_offset", output_offset)
    write_symbol_data("params_activation_min", activation_min)
    write_symbol_data("params_activation_max", activation_max)
    write_symbol_data("per_channel_multiplier", per_channel_multiplier)
    write_symbol_data("per_channel_shift", per_channel_shift)

    # Run Simulation
    total_cycles = 0
    step_size = 1_000_000
    while True:
        actual_steps = npu_sim.step(step_size)
        total_cycles += actual_steps
        if actual_steps < step_size:
            break
        if total_cycles > 500_000_000:  # Conv is slower than MaxPool
            print("Timeout reached.")
            break

    # Read Results
    ref_cycles = read_symbol_val("ref_cycles", 8)
    print(f"  Ref Cycles: {ref_cycles}")

    ref_out = npu_sim.read_memory(
        symbol_map.get("output_data_ref"), np.prod(output_shape)
    )
    ref_out = np.frombuffer(ref_out, dtype=np.int8)

    opt_cycles = read_symbol_val("opt_cycles", 8)
    print(f"  Opt Cycles: {opt_cycles}")

    if opt_cycles > 0:
        print(f"  Speedup: {ref_cycles / opt_cycles:.2f}x")

    opt_out = npu_sim.read_memory(
        symbol_map.get("output_data"), np.prod(output_shape)
    )
    opt_out = np.frombuffer(opt_out, dtype=np.int8)

    # Verify
    mismatches = np.sum(opt_out != ref_out)
    if mismatches > 0:
        print(f"  FAILED: {mismatches} mismatches found!", flush=True)
        # Find first mismatch for debugging
        idx = np.where(opt_out != ref_out)[0][0]
        print(
            f"  First mismatch at index {idx}: Opt {opt_out[idx]}, Ref {ref_out[idx]}"
        )
        sys.exit(1)
    else:
        print("  SUCCESS: Outputs match.", flush=True)


def run_conv_sim_test():
    npu_sim = CoralNPUV2Simulator(highmem_ld=True)
    r = runfiles.Create()
    elf_file = r.Rlocation(
        "coralnpu_hw/sw/opt/litert-micro/test/conv_test.elf"
    )

    if not os.path.exists(elf_file):
        raise FileNotFoundError(f"ELF file not found: {elf_file}")

    test_cases = [
        # 1. Conv_4_4_16_StrideN (ic <= 16)
        {
            "input": [1, 10, 10, 16],
            "filter": [16, 4, 4, 16],
            "stride": (1, 1)
        },
        {
            "input": [1, 10, 10, 16],
            "filter": [16, 4, 4, 16],
            "stride": (2, 2)
        },
        {
            "input": [1, 10, 10, 16],
            "filter": [48, 4, 4, 16],
            "stride": (1, 1)
        },
        # 2. Conv_4_4_48_Stride1 (ic <= 48, stride 1)
        {
            "input": [1, 10, 10, 48],
            "filter": [16, 4, 4, 48],
            "stride": (1, 1)
        },
        # 3. Conv_48_4_4_48 (ic=48, oc=48)
        {
            "input": [1, 10, 10, 48],
            "filter": [48, 4, 4, 48],
            "stride": (1, 1)
        },
        {
            "input": [1, 10, 10, 48],
            "filter": [48, 4, 4, 48],
            "stride": (2, 2)
        },
        # 4. Conv2D_4x4 (Generic 4x4)
        {
            "input": [1, 8, 8, 32],
            "filter": [32, 4, 4, 32],
            "stride": (1, 1)
        },
        {
            "input": [1, 8, 8, 16],
            "filter": [48, 4, 4, 16],
            "stride": (1, 1)
        },
        # 5. Fallback (fh=3, fw=3)
        {
            "input": [1, 8, 8, 16],
            "filter": [16, 3, 3, 16],
            "stride": (1, 1)
        },
    ]

    for tc in test_cases:
        npu_sim = CoralNPUV2Simulator(highmem_ld=True)
        run_test_case(
            npu_sim, elf_file, tc["input"], tc["filter"], tc["stride"]
        )


if __name__ == "__main__":
    run_conv_sim_test()
