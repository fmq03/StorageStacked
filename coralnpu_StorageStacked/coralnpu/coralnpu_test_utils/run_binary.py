#!/usr/bin/env python3
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

import argparse
import os
import sys
import time
import logging
import subprocess

# To support 'import coralnpu_hw.coralnpu_test_utils' without Bazel:
_script_dir = os.path.dirname(os.path.abspath(__file__))
_project_root = os.path.dirname(_script_dir)
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

try:
    import coralnpu_hw
except ImportError:
    import types

    _coralnpu_hw = types.ModuleType("coralnpu_hw")
    _coralnpu_hw.__path__ = [_project_root]
    sys.modules["coralnpu_hw"] = _coralnpu_hw

from elftools.elf.elffile import ELFFile
from coralnpu_hw.coralnpu_test_utils.ftdi_spi_master import FtdiSpiMaster

logger = logging.getLogger(__name__)


class BinaryRunner:
    """Loads and runs a binary on the CoralNPU hardware without input/output handling."""

    def __init__(
        self,
        elf_path,
        usb_serial,
        ftdi_port=1,
        csr_base_addr=0x30000,
        verify=False,
        exit_after_start=False,
        reset=False,
    ):
        """
        Initializes the BinaryRunner.

        Args:
            elf_path: Path to the ELF file.
            usb_serial: USB serial number of the FTDI device.
            ftdi_port: Port number of the FTDI device.
            csr_base_addr: Base address for CSR registers.
            verify: Whether to verify the load by reading back memory.
            exit_after_start: Whether to exit immediately after starting the core.
        """
        self.elf_path = elf_path
        self.spi_master = FtdiSpiMaster(usb_serial, ftdi_port, csr_base_addr)
        self.entry_point = None
        self.verify = verify
        self.exit_after_start = exit_after_start
        self.reset = reset
        self.status_msg_addr = None
        self.status_msg_size = 0
        self._parse_elf()

    def _parse_elf(self):
        """Parses the ELF file to find the entry point."""
        logger.info(f"Parsing ELF file: {self.elf_path}")
        with open(self.elf_path, "rb") as f:
            elf = ELFFile(f)
            self.entry_point = elf.header["e_entry"]

            # Find inference_status_message symbol
            symtab = elf.get_section_by_name(".symtab")
            if symtab:
                syms = symtab.get_symbol_by_name("inference_status_message")
                if syms:
                    self.status_msg_addr = syms[0].entry["st_value"]
                    self.status_msg_size = syms[0].entry["st_size"]
                    logger.info(
                        f"  Found 'inference_status_message' at 0x{self.status_msg_addr:x} (size {self.status_msg_size})"
                    )

        if self.entry_point is None:
            raise ValueError("Could not find entry point in ELF file.")
        logger.info(f"  Found entry point at 0x{self.entry_point:x}")

    def run_binary(self):
        """Executes the binary load and run flow."""
        is_responsive = False
        if not self.reset:
            try:
                logger.info("Checking if FPGA bus is responsive...")
                # ITCM (0x0) is always mapped and safe to read.
                self.spi_master.read_word(0x0)
                is_responsive = True
                logger.info("FPGA bus is responsive.")
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                logger.warning(
                    "FPGA bus is unresponsive. Attempting automatic recovery reset..."
                )

        if self.reset or not is_responsive:
            logger.info("Performing hardware reset (toggle PROG_B)...")
            self.spi_master.device_reset()
            # Wait a bit for DDR calibration to complete
            time.sleep(0.1)

        if self.exit_after_start:
            logger.info(f"Loading ELF file: {self.elf_path}")
            self.spi_master.load_elf(
                self.elf_path, start_core=True, verify=self.verify
            )
            logger.info("Exiting after start as requested.")
            return

        # 1. Load, Start and Poll for halt in a single call to avoid subprocess overhead.
        logger.info(
            f"Loading {self.elf_path}, starting core and polling for halt..."
        )
        timeout = 60.0
        try:
            self.spi_master.load_elf(
                self.elf_path,
                start_core=True,
                verify=self.verify,
                poll_halt=timeout,
                status_addr=self.status_msg_addr,
                status_size=self.status_msg_size,
            )
            logger.info(
                "Binary execution COMPLETED: Core halted successfully."
            )
        except Exception as e:
            logger.error(f"Binary execution FAILED: {e}")
            sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Load and run a binary on CoralNPU."
    )
    parser.add_argument("elf_file", help="Path to the ELF file to run.")
    parser.add_argument(
        "--usb-serial",
        required=True,
        help="USB serial number of the FTDI device."
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
        help="Base address for CSR registers (can be hex, default: 0x30000).",
    )
    parser.add_argument(
        "--highmem",
        action="store_true",
        help="Use high memory (0x200000) for CSR base address.",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Verify the ELF load by reading back memory.",
    )
    parser.add_argument(
        "--exit-after-start",
        action="store_true",
        help="Exit immediately after starting the core.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose logging.",
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help=
        "Perform hardware reset (toggle PROG_B) before loading (will wipe DDR).",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    )

    csr_base_addr = args.csr_base_addr
    if args.highmem:
        csr_base_addr = 0x200000

    try:
        runner = BinaryRunner(
            args.elf_file,
            args.usb_serial,
            args.ftdi_port,
            csr_base_addr,
            verify=args.verify,
            exit_after_start=args.exit_after_start,
            reset=args.reset,
        )
        runner.run_binary()
    except (ValueError, RuntimeError, FileNotFoundError) as e:
        logger.error(f"Error: {e}")
        sys.exit(1)
    except Exception as e:
        logger.error(f"An unexpected error occurred: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
