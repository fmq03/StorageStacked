#!/usr/bin/env python3
# Copyright 2025 Google LLC
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

import argparse
import os
import random
import sys
import time
import traceback
import numpy as np

try:
    from coralnpu_test_utils.ftdi_spi_master import FtdiSpiMaster
except ImportError:
    FtdiSpiMaster = None


class SimSpiMaster:
    """A simulation-compatible SPI master using the TCP DPI interface."""

    def __init__(self, port=5555):
        # Try to import SPIDriver from the utils directory
        driver_path = os.path.abspath(
            os.path.join(
                os.path.dirname(__file__), "../utils/coralnpu_soc_loader"
            )
        )
        if driver_path not in sys.path:
            sys.path.append(driver_path)

        try:
            from spi_driver import SPIDriver
        except ImportError:
            # Fallback for bazel run where imports might be different
            try:
                from utils.coralnpu_soc_loader.spi_driver import SPIDriver
            except ImportError:
                raise ImportError(
                    f"Could not import SPIDriver. Checked {driver_path} and utils.coralnpu_soc_loader.spi_driver"
                )

        print(f"Attempting to connect to simulator on port {port}...")
        for i in range(30):
            try:
                self.driver = SPIDriver(port)
                print(f"Connected to simulator on port {port}")
                break
            except (ConnectionRefusedError, OSError) as e:
                if i % 5 == 0:
                    print(f"Waiting for simulator... ({e})")
                time.sleep(1)
        else:
            raise ConnectionRefusedError(
                f"Could not connect to simulator on port {port} after 30 seconds."
            )

    def close(self):
        if hasattr(self, "driver") and self.driver:
            self.driver.close()

    def idle_clocking(self, cycles):
        self.driver.idle_clocking(cycles)

    def read_line(self, address):
        """Reads a single 128-bit line from memory via SPI V2."""
        read_data_bytes = self.driver.v2_read(address, 1)
        return int.from_bytes(bytes(read_data_bytes), "little")

    def write_lines(self, start_addr, num_beats, data_as_bytes):
        """Writes a contiguous block of data via SPI V2."""
        self.driver.v2_write(start_addr, data_as_bytes)

    def write_line(self, address, data):
        data_as_bytes = data.to_bytes(16, "little")
        self.write_lines(address, 1, data_as_bytes)

    def load_data(self, data, address):
        """Loads data handling unaligned start/end."""
        size = len(data)
        start_address = address
        end_address = start_address + size
        data_ptr = 0

        # 1. Handle unaligned start
        start_offset = start_address % 16
        if start_offset != 0:
            line_addr = start_address - start_offset
            bytes_to_write = min(16 - start_offset, size)
            data_chunk = data[data_ptr:data_ptr + bytes_to_write]

            old_line_int = self.read_line(line_addr)
            old_line_bytes = old_line_int.to_bytes(16, "little")

            new_line_bytes = bytearray(old_line_bytes)
            new_line_bytes[start_offset:start_offset +
                           bytes_to_write] = data_chunk
            new_line_int = int.from_bytes(new_line_bytes, "little")

            self.write_line(line_addr, new_line_int)
            data_ptr += bytes_to_write
            current_addr = start_address + bytes_to_write
        else:
            current_addr = start_address

        # 2. Middle aligned blocks
        while (end_address - current_addr) >= 16:
            data_chunk = data[data_ptr:data_ptr + 16]
            self.write_line(current_addr, int.from_bytes(data_chunk, "little"))
            data_ptr += 16
            current_addr += 16

        # 3. Handle unaligned end
        bytes_remaining = end_address - current_addr
        if bytes_remaining > 0:
            line_addr = current_addr
            data_chunk = data[data_ptr:data_ptr + bytes_remaining]

            old_line_int = self.read_line(line_addr)
            old_line_bytes = old_line_int.to_bytes(16, "little")

            new_line_bytes = bytearray(old_line_bytes)
            new_line_bytes[0:bytes_remaining] = data_chunk
            new_line_int = int.from_bytes(new_line_bytes, "little")

            self.write_line(line_addr, new_line_int)

    def read_data(self, address, size, verbose=False):
        if size == 0:
            return bytearray()

        data = bytearray()
        bytes_remaining = size
        current_addr = address

        # 1. Unaligned start
        start_offset = current_addr % 16
        if start_offset != 0:
            line_addr = current_addr - start_offset
            bytes_to_read = min(16 - start_offset, bytes_remaining)
            line_data = self.read_line(line_addr)
            line_bytes = line_data.to_bytes(16, "little")
            data.extend(line_bytes[start_offset:start_offset + bytes_to_read])
            bytes_remaining -= bytes_to_read
            current_addr += bytes_to_read

        # 2. Aligned chunks
        while bytes_remaining > 0:
            bytes_to_read = min(16, bytes_remaining)
            line_data = self.read_line(current_addr)
            line_bytes = line_data.to_bytes(16, "little")
            data.extend(line_bytes[:bytes_to_read])
            bytes_remaining -= bytes_to_read
            current_addr += bytes_to_read

        return data[:size]


class SramTestRunner:
    """Runs a SRAM test on the CoralNPU hardware."""

    SRAM_ADDR = 0x20000000

    def __init__(
        self,
        usb_serial,
        ftdi_port=1,
        csr_base_addr=0x30000,
        continue_on_error=False,
        simulation=False,
        sim_port=5555,
        max_size=256,
    ):
        self.MAX_SIZE = max_size
        if simulation:
            print(f"Initializing Simulation SPI Master on port {sim_port}")
            self.spi_master = SimSpiMaster(sim_port)
        else:
            if FtdiSpiMaster is None:
                raise ImportError(
                    "FtdiSpiMaster could not be imported. Is pyftdi installed?"
                )
            self.spi_master = FtdiSpiMaster(
                usb_serial, ftdi_port, csr_base_addr
            )

        self.continue_on_error = continue_on_error
        self.results = []

    def _generate_pattern_data(self, size, pattern_type):
        """Generates data based on the requested pattern type."""
        if pattern_type == "zeros":
            return np.zeros(size, dtype=np.uint8)
        elif pattern_type == "ones":
            return np.full(size, 0xFF, dtype=np.uint8)
        elif pattern_type == "0x55":
            return np.full(size, 0x55, dtype=np.uint8)
        elif pattern_type == "0xAA":
            return np.full(size, 0xAA, dtype=np.uint8)
        elif pattern_type == "incrementing":
            return np.arange(size, dtype=np.uint8)
        elif pattern_type == "random":
            return np.random.randint(0, 256, size=size, dtype=np.uint8)
        else:
            raise ValueError(f"Unknown pattern type: {pattern_type}")

    def run_test(self):
        """Executes the full SRAM test flow."""
        try:
            self.spi_master.idle_clocking(20)
            self.results = []

            patterns = [
                "zeros", "ones", "0x55", "0xAA", "incrementing", "random"
            ]

            # Power of two sizes from 1 up to MAX_SIZE
            sizes = []
            curr_size = 1
            while curr_size <= self.MAX_SIZE:
                sizes.append(curr_size)
                curr_size *= 2

            print(f"Starting SRAM Test Suite.")
            print(f"Target Address: 0x{self.SRAM_ADDR:x}")
            print(f"Max Size: {self.MAX_SIZE} bytes")

            total_tests = 0
            passed_tests = 0

            # If user specified a single test/size, override
            if getattr(self, "single_pattern", None):
                patterns = [self.single_pattern]
            if getattr(self, "single_size", None):
                sizes = [self.single_size]

            for size in sizes:
                for pattern in patterns:
                    total_tests += 1
                    success, details = self._run_single_test(size, pattern)
                    self.results.append({
                        "size": size,
                        "pattern": pattern,
                        "success": success,
                        "details": details,
                    })
                    if success:
                        passed_tests += 1
                    elif not self.continue_on_error:
                        self._print_summary()
                        raise RuntimeError(f"SRAM test failed: {details}")

            self._print_summary()
            if passed_tests < total_tests:
                sys.exit(1)

        finally:
            self.spi_master.close()

    def _print_summary(self):
        print("\nTest Summary:")
        print(f"{'Size':<10} | {'Pattern':<10} | {'Result':<10} | {'Details'}")
        print("-" * 80)
        for res in self.results:
            result_str = "PASS" if res["success"] else "FAIL"
            print(
                f"{res['size']:<10} | {res['pattern']:<10} | {result_str:<10} | {res['details']}"
            )
        print("-" * 80)
        total = len(self.results)
        passed = sum(1 for r in self.results if r["success"])
        print(f"Total Tests: {total}")
        print(f"Passed:      {passed}")
        print(f"Failed:      {total - passed}")

    def _get_mismatch_ranges(self, mismatch_indices):
        if len(mismatch_indices) == 0:
            return []
        ranges = []
        start = mismatch_indices[0]
        prev = mismatch_indices[0]
        for i in range(1, len(mismatch_indices)):
            if mismatch_indices[i] == prev + 1:
                prev = mismatch_indices[i]
            else:
                ranges.append((start, prev))
                start = mismatch_indices[i]
                prev = mismatch_indices[i]
        ranges.append((start, prev))
        return ranges

    def _format_ranges(self, ranges):
        return ", ".join([
            f"0x{s:x}" if s == e else f"0x{s:x}-0x{e:x}" for s, e in ranges
        ])

    def _run_single_test(self, size, pattern_type):
        """Runs a single test case with a specific size and pattern."""
        print(f"Running test: size={size}, pattern={pattern_type}")
        golden_data = self._generate_pattern_data(size, pattern_type)

        # 1. Load
        print(f"(Loading {size} bytes to 0x{self.SRAM_ADDR:x}...)")
        self.spi_master.load_data(golden_data.tobytes(), self.SRAM_ADDR)

        # 2. Read back
        print(f"(Reading back {size} bytes from 0x{self.SRAM_ADDR:x}...)")
        result_data = self.spi_master.read_data(self.SRAM_ADDR, size)
        result_array = np.frombuffer(result_data, dtype=np.uint8)

        # 3. Verify
        print("(Verifying...)")
        if not np.array_equal(golden_data, result_array):
            mismatch_indices = np.where(golden_data != result_array)[0]
            count = len(mismatch_indices)
            ranges = self._get_mismatch_ranges(mismatch_indices)
            print(f"FAILED: {count} errors found.")
            for idx in mismatch_indices[:16]:
                print(
                    f"  Addr 0x{self.SRAM_ADDR + idx:x}: Expected 0x{golden_data[idx]:02x}, Got 0x{result_array[idx]:02x}"
                )

            return False, f"{count} errs. Ranges: {self._format_ranges(ranges)}"

        print("PASSED")
        return True, "OK"


def main():
    sys.stdout.reconfigure(line_buffering=True)
    print("SRAM Test Script Started.", flush=True)

    parser = argparse.ArgumentParser(description="Run SRAM test on CoralNPU.")
    parser.add_argument(
        "--usb-serial", help="USB serial number of the FTDI device."
    )
    parser.add_argument(
        "--ftdi-port",
        type=int,
        default=1,
        help="Port number of the FTDI device."
    )
    parser.add_argument(
        "--csr-base-addr",
        type=lambda x: int(x, 0),
        default=0x30000,
        help="Base address for CSR registers (default: 0x30000).",
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Continue testing even if a test fails.",
    )
    parser.add_argument(
        "--simulation",
        action="store_true",
        help="Run in simulation mode (connect to TCP port).",
    )
    parser.add_argument(
        "--sim-port",
        type=int,
        help="TCP port for simulation mode (defaults to SPI_DPI_PORT or 5555)."
    )
    parser.add_argument(
        "--max-size",
        type=int,
        default=256,
        help="Maximum test size in bytes."
    )
    parser.add_argument(
        "--single-test",
        help="Run only a specific test pattern (e.g., 'incrementing')."
    )
    parser.add_argument(
        "--single-size", type=int, help="Run only a specific test size."
    )
    args = parser.parse_args()

    if args.simulation:
        if args.sim_port is None:
            args.sim_port = int(os.environ.get("SPI_DPI_PORT", 5555))
    elif not args.usb_serial:
        parser.error("--usb-serial is required unless --simulation is used.")

    try:
        runner = SramTestRunner(
            args.usb_serial,
            args.ftdi_port,
            args.csr_base_addr,
            continue_on_error=args.continue_on_error,
            simulation=args.simulation,
            sim_port=args.sim_port,
            max_size=args.max_size,
        )

        if args.single_size:
            runner.single_size = args.single_size
        if args.single_test:
            runner.single_pattern = args.single_test

        runner.run_test()
    except (ValueError, RuntimeError, FileNotFoundError) as e:
        print(f"Error: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
