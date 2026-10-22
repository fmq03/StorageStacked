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

import logging
import time
import zlib

# Command IDs (must match firmware)
FLASH_TOOL_CMD_GET_INFO = 1
FLASH_TOOL_CMD_PROGRAM_DATA = 2
FLASH_TOOL_CMD_HELLO = 3

# Status Codes (must match flash_tool_status.h)
FLASH_TOOL_STATUS_OK = 0
FLASH_TOOL_STATUS_DISCOVERY_FAILED = 1
FLASH_TOOL_STATUS_VERIFY_FAILED = 2
FLASH_TOOL_STATUS_CRC_MISMATCH = 3
FLASH_TOOL_STATUS_BOUNDARY_ERROR = 4
FLASH_TOOL_STATUS_BUFFER_TOO_SMALL = 5
FLASH_TOOL_STATUS_INVALID_LEN = 6
FLASH_TOOL_STATUS_NOT_INITIALIZED = 7
FLASH_TOOL_STATUS_ADDRESS_ERROR = 8

# Magic ready value
FLASH_TOOL_READY_MAGIC = 0xFEEDFACE


class DriverWrapper:
    """Wraps an SPI driver to provide a consistent read/write interface."""

    def __init__(self, driver):
        self.driver = driver

    def write(self, addr, data):
        if hasattr(self.driver, "write"):
            return self.driver.write(addr, data)
        elif hasattr(self.driver, "v2_write"):
            return self.driver.v2_write(addr, data)
        # Handle cases like load_data for FTDI driver
        elif hasattr(self.driver, "load_data"):
            return self.driver.load_data(data, addr)
        else:
            raise AttributeError(
                "Driver has no write, v2_write, or load_data method"
            )

    def read(self, addr, num_beats):
        if hasattr(self.driver, "read"):
            return self.driver.read(addr, num_beats)
        elif hasattr(self.driver, "v2_read"):
            return self.driver.v2_read(addr, num_beats)
        # Handle read_data for FTDI driver
        elif hasattr(self.driver, "read_data"):
            return self.driver.read_data(addr, num_beats * 16, verbose=False)
        else:
            raise AttributeError(
                "Driver has no read, v2_read, or read_data method"
            )


class FlashTool:

    def __init__(self, driver, symbols):
        self.driver = DriverWrapper(driver)
        self.ready_addr = symbols["ready"]
        self.cmd_addr = symbols["cmd"]
        self.resp_addr = symbols["resp"]
        self.buffer_addr = symbols["buffer"]
        self.capacity = 0
        self.sector_size = 0
        self.page_size = 0

    def wait_for_ready(self, timeout=60.0):
        logging.info(
            f"HOST: Waiting for ready magic at 0x{self.ready_addr:08x}..."
        )
        start_time = time.time()
        while time.time() - start_time < timeout:
            ready_pkt = bytes(self.driver.read(self.ready_addr, 1))
            word_off = self.ready_addr % 16
            ready_val = (
                int.from_bytes(ready_pkt, "little") >> (word_off * 8)
            ) & 0xFFFFFFFF
            if ready_val == FLASH_TOOL_READY_MAGIC:
                logging.info("HOST: Core is ready.")
                return True
            time.sleep(0.5)
        raise RuntimeError("Timeout waiting for ready magic")

    def send_cmd(self, cmd, addr=0, length=0, crc32=0):
        # 1. Clear response area first to avoid stale data
        zero_pkt = bytearray(16)
        self.driver.write(self.resp_addr, zero_pkt)

        # 2. Write command
        pkt = bytearray(16)
        pkt[0:4] = cmd.to_bytes(4, "little")
        pkt[4:8] = addr.to_bytes(4, "little")
        pkt[8:12] = length.to_bytes(4, "little")
        pkt[12:16] = crc32.to_bytes(4, "little")
        self.driver.write(self.cmd_addr, pkt)

        # 3. Wait for response
        timeout = 60.0
        start_time = time.time()
        while time.time() - start_time < timeout:
            resp_pkt = bytes(self.driver.read(self.resp_addr, 1))
            status = int.from_bytes(resp_pkt[12:16], "little")
            resp_cmd = int.from_bytes(resp_pkt[0:4], "little")

            if resp_cmd == cmd:
                if status == FLASH_TOOL_STATUS_CRC_MISMATCH:
                    raise RuntimeError(
                        f"Command {cmd} failed: CRC MISMATCH on device"
                    )
                if status != FLASH_TOOL_STATUS_OK:
                    raise RuntimeError(
                        f"Command {cmd} failed with status {status}"
                    )
                return resp_pkt
            time.sleep(0.1)
        raise RuntimeError(f"Timeout waiting for command {cmd} response")

    def hello(self):
        logging.info("HOST: Sending HELLO...")
        self.send_cmd(FLASH_TOOL_CMD_HELLO)
        logging.info("HOST: HELLO OK")

    def get_info(self):
        logging.info("HOST: Querying flash info...")
        self.send_cmd(FLASH_TOOL_CMD_GET_INFO)
        info_data = bytes(self.driver.read(self.buffer_addr, 1))
        self.capacity = int.from_bytes(info_data[0:4], "little")
        self.sector_size = int.from_bytes(info_data[4:8], "little")
        self.page_size = int.from_bytes(info_data[8:12], "little")
        logging.info(f"HOST: Flash Capacity: {self.capacity} bytes")
        logging.info(f"HOST: Sector Size: {self.sector_size} bytes")
        logging.info(f"HOST: Page Size: {self.page_size} bytes")

    def program_data(self, addr, data):
        if addr >= 16 * 1024 * 1024:
            raise ValueError(
                f"Address 0x{addr:08x} exceeds 16MB limit for 3-byte addressing"
            )

        if self.sector_size == 0:
            self.get_info()
        if self.sector_size == 0:
            raise RuntimeError("Sector size is unknown; cannot program data")

        total_len = len(data)
        curr_addr = addr
        i = 0

        while i < total_len:
            # Calculate how much we can write in this sector
            offset = curr_addr % self.sector_size
            remaining_in_sector = self.sector_size - offset
            chunk_len = min(total_len - i, remaining_in_sector)

            chunk = data[i:i + chunk_len]
            logging.info(
                f"HOST: Programming chunk at 0x{curr_addr:08x} ({len(chunk)} bytes)..."
            )

            chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF

            padded_chunk = bytearray(chunk)
            if len(padded_chunk) % 16 != 0:
                padded_chunk += b"\x00" * (16 - (len(padded_chunk) % 16))

            self.driver.write(self.buffer_addr, padded_chunk)
            self.send_cmd(
                FLASH_TOOL_CMD_PROGRAM_DATA,
                curr_addr,
                len(chunk),
                crc32=chunk_crc
            )

            curr_addr += chunk_len
            i += chunk_len
