import os
import sys
import numpy as np
from bazel_tools.tools.python.runfiles import runfiles
from coralnpu_v2_sim_utils import CoralNPUV2Simulator


def run_test_case(npu_sim, elf_file, input_shape):
    # Symbols to resolve
    symbols = [
        "input_dims",
        "input_data",
        "output_dims",
        "output_data",
        "output_data_ref",
        "params_stride_width",
        "params_stride_height",
        "params_filter_width",
        "params_filter_height",
        "params_padding_width",
        "params_padding_height",
        "params_activation_min",
        "params_activation_max",
        "ref_cycles",
        "opt_cycles",
        "heartbeat",
    ]

    entry_point, symbol_map = npu_sim.get_elf_entry_and_symbol(
        elf_file, symbols
    )

    # Initialize simulator
    npu_sim.load_program(elf_file, entry_point)

    # Parameters for MaxPool 2x2
    filter_height = 2
    filter_width = 2
    stride_height = 2
    stride_width = 2
    activation_min = -128
    activation_max = 127

    # Calculate output shape (VALID padding)
    out_h = (input_shape[1] - filter_height) // stride_height + 1
    out_w = (input_shape[2] - filter_width) // stride_width + 1
    output_shape = [input_shape[0], out_h, out_w, input_shape[3]]

    # VALID padding means no padding needed if dimensions fit perfectly
    pad_h = 0
    pad_w = 0

    print(
        f"Running simulation for shape {input_shape} -> {output_shape}...",
        flush=True
    )

    # Generate random input data
    input_data = np.random.randint(-128, 127, size=input_shape, dtype=np.int8)

    # Helper to write data
    def write_symbol_data(name, data):
        addr = symbol_map.get(name)
        if addr is None:
            raise ValueError(f"Symbol {name} not found")
        # Ensure data is numpy array or bytes
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
    write_symbol_data("output_dims", np.array(output_shape, dtype=np.int32))

    write_symbol_data("params_stride_width", stride_width)
    write_symbol_data("params_stride_height", stride_height)
    write_symbol_data("params_filter_width", filter_width)
    write_symbol_data("params_filter_height", filter_height)
    write_symbol_data("params_padding_width", pad_w)
    write_symbol_data("params_padding_height", pad_h)
    write_symbol_data("params_activation_min", activation_min)
    write_symbol_data("params_activation_max", activation_max)

    # Run Simulation
    total_cycles = 0
    step_size = 500_000
    while True:
        actual_steps = npu_sim.step(step_size)
        total_cycles += actual_steps
        if actual_steps < step_size:
            break
        if total_cycles > 100_000_000:
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
        # sys.exit(1) # Don't exit on first dim failure, but let's see.
        # Actually better to exit to notice errors.
        sys.exit(1)
    else:
        print("  SUCCESS: Outputs match.", flush=True)


def run_max_pool_sim_test():
    npu_sim = CoralNPUV2Simulator(highmem_ld=True)
    r = runfiles.Create()
    elf_file = r.Rlocation(
        "coralnpu_hw/sw/opt/litert-micro/test/max_pool_test.elf"
    )

    if not os.path.exists(elf_file):
        raise FileNotFoundError(f"ELF file not found: {elf_file}")

    # Shapes from HPS (Depth 16, 48)
    # [1, 200, 200, 16] - Large input to verify caching/bandwidth
    # [1, 50, 50, 48]

    test_shapes = [
        [1, 100, 100, 16],
        [1, 50, 50, 48],
    ]

    for shape in test_shapes:
        npu_sim = CoralNPUV2Simulator(highmem_ld=True)
        run_test_case(npu_sim, elf_file, shape)


if __name__ == "__main__":
    run_max_pool_sim_test()
