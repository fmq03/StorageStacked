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

import cocotb
from coralnpu_test_utils.sim_test_fixture import Fixture
from bazel_tools.tools.python.runfiles import runfiles
import random
import struct

# Rounding modes
RNE = 0
RTZ = 1
RDN = 2
RUP = 3
RMM = 4

# fflags
F_NV = 1 << 4
F_DZ = 1 << 3
F_OF = 1 << 2
F_UF = 1 << 1
F_NX = 1 << 0


def fp32_to_bits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def bits_to_fp32(i):
    return struct.unpack('<f', struct.pack('<I', i & 0xFFFFFFFF))[0]


def bf16_to_fp32_bits_with_flags(bf16_bits):
    # BF16 to FP32 is always exact.
    # NV set only on signaling NaN.
    flags = 0
    # BF16 NaN: exp=0xFF, mant != 0
    # Signaling NaN if top bit of mantissa (bit 6) is 0.
    if (bf16_bits & 0x7f80) == 0x7f80 and (bf16_bits & 0x7f) != 0:
        if not (bf16_bits & 0x40):  # sNaN
            flags |= F_NV

    fp32_bits = (bf16_bits << 16) & 0xFFFFFFFF
    return fp32_bits, flags


def fp32_to_bf16_bits_with_flags(fp32_bits, rm=RNE):
    sign = (fp32_bits >> 31) & 1
    exp = (fp32_bits >> 23) & 0xFF
    mant = fp32_bits & 0x7FFFFF
    flags = 0

    if exp == 0xFF:  # Inf or NaN
        if mant != 0:  # NaN
            if not (mant & 0x400000):  # sNaN
                flags |= F_NV
            # Canonicalize NaN for BF16
            res_mant = (mant >> 16) | 0x40  # make it qNaN
            return (sign << 15) | (0xFF << 7) | res_mant, flags
        return (sign << 15) | (0xFF << 7), flags

    # Rounding bits
    round_bit = (fp32_bits >> 15) & 1
    sticky_bit = 1 if (fp32_bits & 0x7FFF) != 0 else 0

    if round_bit or sticky_bit:
        flags |= F_NX

    # Value is in range if exp is normal or subnormal
    # BF16 and FP32 share the same exponent range, but BF16 has less mantissa.
    # Max BF16 value: (2 - 2^-7) * 2^127
    # Bit pattern: exp=0xFE, mant=0x7F -> 0x7F7F

    upper = (fp32_bits >> 16)
    lsb = upper & 1

    increment = 0
    if rm == RNE:
        if round_bit and (lsb or sticky_bit):
            increment = 1
    elif rm == RTZ:
        increment = 0
    elif rm == RDN:
        if sign and (round_bit or sticky_bit):
            increment = 1
    elif rm == RUP:
        if not sign and (round_bit or sticky_bit):
            increment = 1
    elif rm == RMM:
        if round_bit:
            increment = 1

    res_raw = upper + increment
    res = res_raw & 0xFFFF

    # Check Overflow: if exponent becomes 0xFF due to rounding
    if (res_raw & 0x7F80) == 0x7f80 and exp != 0xFF:
        flags |= F_OF | F_NX

    # Check Underflow: if result is tiny and inexact
    # Tiny means magnitude < min normal BF16 (2^-126)
    is_tiny = (exp == 0) or (res_raw & 0x7f80) == 0
    if is_tiny and (flags & F_NX):
        flags |= F_UF

    return res, flags


@cocotb.test()
async def zfbfmin_test(dut):
    """Test that runs Zfbfmin conversion instructions with various inputs."""
    fixture = await Fixture.Create(dut)
    r = runfiles.Create()
    elf_file = 'zfbfmin_test.elf'

    await fixture.load_elf_and_lookup_symbols(
        r.Rlocation('coralnpu_hw/tests/cocotb/' + elf_file), [
            'fcvt_s_bf16_cases', 'fcvt_bf16_s_cases', 'num_fcvt_s_bf16_cases',
            'num_fcvt_bf16_s_cases'
        ]
    )

    symbols = fixture.symbols

    # Test cases for fcvt.s.bf16 (BF16 to FP32)
    s_bf16_inputs = [
        0x3fc0,  # 1.5f
        0x0000,  # 0.0
        0x8000,  # -0.0
        0x7f80,  # +Inf
        0xff80,  # -Inf
        0x7fc0,  # qNaN
        0x7fbf,  # sNaN
        0x0001,  # min subnormal
    ]
    for _ in range(10):
        s_bf16_inputs.append(random.getrandbits(16))

    for i, val in enumerate(s_bf16_inputs):
        await fixture.core_mini_axi.write_word(
            symbols['fcvt_s_bf16_cases'] + i * 16, 0xFFFF0000 | val
        )
        await fixture.core_mini_axi.write_word(
            symbols['fcvt_s_bf16_cases'] + i * 16 + 4, RNE
        )

    await fixture.core_mini_axi.write_word(
        symbols['num_fcvt_s_bf16_cases'], len(s_bf16_inputs)
    )

    # Test cases for fcvt.bf16.s (FP32 to BF16)
    bf16_s_inputs = [
        (1.5, RNE),
        (2.75, RNE),
        (0.0, RNE),
        (-0.0, RNE),
        (float('inf'), RNE),
        (float('-inf'), RNE),
        (bits_to_fp32(0x7f800001), RNE),  # sNaN
        (bits_to_fp32(0x7f800000 + (1 << 22)), RNE),  # qNaN
        # Max FP32 (overflows to BF16 Inf)
        (bits_to_fp32(0x7f7fffff), RNE),
        # Rounding tests
        (1.00390625, RNE),  # 1.0 + 2^-8
        (1.00390625, RTZ),
        # Tiny FP32 (underflow)
        (bits_to_fp32(0x00000001), RNE),  # min FP32 subnormal
    ]

    for _ in range(10):
        f = random.uniform(-100, 100)
        bf16_s_inputs.append((f, random.choice([RNE, RTZ, RDN, RUP, RMM])))

    for i, (val, rm) in enumerate(bf16_s_inputs):
        bits = fp32_to_bits(val)
        await fixture.core_mini_axi.write_word(
            symbols['fcvt_bf16_s_cases'] + i * 16, bits
        )
        await fixture.core_mini_axi.write_word(
            symbols['fcvt_bf16_s_cases'] + i * 16 + 4, rm
        )

    await fixture.core_mini_axi.write_word(
        symbols['num_fcvt_bf16_s_cases'], len(bf16_s_inputs)
    )

    # Run the core
    cycles = await fixture.run_to_halt(timeout_cycles=1000000)
    dut._log.info(f"Cycle count: {cycles}")

    # Verify fcvt.s.bf16
    dut._log.info("Verifying fcvt.s.bf16 results...")
    for i, val in enumerate(s_bf16_inputs):
        addr = symbols['fcvt_s_bf16_cases'] + i * 16 + 8
        actual_bytes = await fixture.core_mini_axi.read_word(addr)
        actual_bits = int.from_bytes(actual_bytes.tobytes(), 'little')

        flags_bytes = await fixture.core_mini_axi.read_word(addr + 4)
        actual_flags = int.from_bytes(flags_bytes.tobytes(), 'little')

        expected_bits, expected_flags = bf16_to_fp32_bits_with_flags(val)

        is_expected_nan = (expected_bits & 0x7f800000
                           ) == 0x7f800000 and (expected_bits & 0x7fffff) != 0
        is_actual_nan = (actual_bits & 0x7f800000
                         ) == 0x7f800000 and (actual_bits & 0x7fffff) != 0

        if is_expected_nan:
            assert is_actual_nan, f"Case {i}: Input {hex(val)}, Expected NaN, got {hex(actual_bits)}"
        else:
            assert actual_bits == expected_bits, f"Case {i}: Input {hex(val)}, Expected {hex(expected_bits)}, got {hex(actual_bits)}"

        assert actual_flags == expected_flags, f"Case {i}: Input {hex(val)}, Expected flags {hex(expected_flags)}, got {hex(actual_flags)}"

    # Verify fcvt.bf16.s
    dut._log.info("Verifying fcvt.bf16.s results...")
    for i, (val, rm) in enumerate(bf16_s_inputs):
        addr = symbols['fcvt_bf16_s_cases'] + i * 16 + 8
        actual_bytes = await fixture.core_mini_axi.read_word(addr)
        actual_bits = int.from_bytes(actual_bytes.tobytes(), 'little')

        flags_bytes = await fixture.core_mini_axi.read_word(addr + 4)
        actual_flags = int.from_bytes(flags_bytes.tobytes(), 'little')

        actual_bf16 = actual_bits & 0xFFFF
        expected_bf16, expected_flags = fp32_to_bf16_bits_with_flags(
            fp32_to_bits(val), rm
        )

        is_expected_nan = (expected_bf16
                           & 0x7f80) == 0x7f80 and (expected_bf16
                                                    & 0x7f) != 0
        is_actual_nan = (actual_bf16
                         & 0x7f80) == 0x7f80 and (actual_bf16
                                                  & 0x7f) != 0

        if is_expected_nan:
            assert is_actual_nan, f"Case {i}: Input {val} (RM {rm}), Expected NaN, got {hex(actual_bf16)}"
        else:
            assert actual_bf16 == expected_bf16, f"Case {i}: Input {val} (RM {rm}), Expected {hex(expected_bf16)}, got {hex(actual_bf16)}"
            assert (
                actual_bits >> 16
            ) == 0xFFFF, f"Case {i}: Result not NaN-boxed: {hex(actual_bits)}"

        # Verify flags
        assert actual_flags == expected_flags, f"Case {i}: Input {val} (RM {rm}), Expected flags {hex(expected_flags)}, got {hex(actual_flags)}"

    dut._log.info("All Zfbfmin tests passed!")
