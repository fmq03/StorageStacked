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

import unittest
import zlib
import threading
import time

from fpga.sw.flash_tool_lib import (
    FlashTool,
    FLASH_TOOL_READY_MAGIC,
    FLASH_TOOL_CMD_HELLO,
    FLASH_TOOL_CMD_GET_INFO,
    FLASH_TOOL_CMD_PROGRAM_DATA,
    FLASH_TOOL_STATUS_OK,
    FLASH_TOOL_STATUS_CRC_MISMATCH,
    FLASH_TOOL_STATUS_BOUNDARY_ERROR,
    FLASH_TOOL_STATUS_ADDRESS_ERROR,
)


class MockSPIDriver:

    def __init__(self):
        self.memory = {}  # addr -> bytearray(16)

    def write(self, addr, data):
        # Data can be any length multiple of 16
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            self.memory[addr + i] = bytearray(chunk)

    def read(self, addr, num_beats):
        res = bytearray()
        for i in range(num_beats):
            res.extend(self.memory.get(addr + i * 16, b"\x00" * 16))
        return res


class MockFirmware:

    def __init__(self, driver, symbols):
        self.driver = driver
        self.symbols = symbols
        self.running = False
        self.capacity = 64 * 1024 * 1024
        self.sector_size = 256 * 1024
        self.page_size = 512

    def start(self):
        self.running = True
        # Set ready magic
        ready_val = FLASH_TOOL_READY_MAGIC.to_bytes(4, "little")
        # READY is at self.symbols['ready'], which might not be 16-byte aligned in the middle of a line
        # but for mock we assume it's aligned for simplicity or we handle it
        line_addr = (self.symbols["ready"] // 16) * 16
        offset = self.symbols["ready"] % 16
        line = self.driver.memory.get(line_addr, bytearray(16))
        line[offset:offset + 4] = ready_val
        self.driver.memory[line_addr] = line

        self.thread = threading.Thread(target=self._loop)
        self.thread.daemon = True
        self.thread.start()

    def stop(self):
        self.running = False
        self.thread.join()

    def _loop(self):
        while self.running:
            cmd_line = self.driver.memory.get(
                self.symbols["cmd"], bytearray(16)
            )
            cmd_id = int.from_bytes(cmd_line[0:4], "little")

            if cmd_id != 0:
                addr = int.from_bytes(cmd_line[4:8], "little")
                length = int.from_bytes(cmd_line[8:12], "little")
                host_crc = int.from_bytes(cmd_line[12:16], "little")

                resp = bytearray(16)
                resp[0:4] = cmd_id.to_bytes(4, "little")
                resp[4:8] = addr.to_bytes(4, "little")
                resp[8:12] = length.to_bytes(4, "little")

                status = FLASH_TOOL_STATUS_OK

                if cmd_id == FLASH_TOOL_CMD_HELLO:
                    pass
                elif cmd_id == FLASH_TOOL_CMD_GET_INFO:
                    info_pkt = bytearray(16)
                    info_pkt[0:4] = self.capacity.to_bytes(4, "little")
                    info_pkt[4:8] = self.sector_size.to_bytes(4, "little")
                    info_pkt[8:12] = self.page_size.to_bytes(4, "little")
                    self.driver.write(self.symbols["buffer"], info_pkt)
                elif cmd_id == FLASH_TOOL_CMD_PROGRAM_DATA:
                    # Address check (3-byte limit)
                    if addr >= 16 * 1024 * 1024:
                        status = FLASH_TOOL_STATUS_ADDRESS_ERROR
                    else:
                        # Boundary check
                        offset = addr % self.sector_size
                        if offset + length > self.sector_size:
                            status = FLASH_TOOL_STATUS_BOUNDARY_ERROR
                        else:
                            # Verify CRC
                            data = self.driver.read(
                                self.symbols["buffer"], (length + 15) // 16
                            )
                            actual_data = data[:length]
                            dev_crc = zlib.crc32(actual_data) & 0xFFFFFFFF
                            if dev_crc != host_crc:
                                status = FLASH_TOOL_STATUS_CRC_MISMATCH

                resp[12:16] = status.to_bytes(4, "little")
                # Write response
                self.driver.write(self.symbols["resp"], resp)
                # Clear command
                self.driver.write(self.symbols["cmd"], b"\x00" * 16)

            time.sleep(0.01)


class TestFlashTool(unittest.TestCase):

    def setUp(self):
        self.driver = MockSPIDriver()
        self.symbols = {
            "ready": 0x1000,
            "cmd": 0x1010,
            "resp": 0x1020,
            "buffer": 0x2000,
        }
        self.fw = MockFirmware(self.driver, self.symbols)
        self.fw.start()
        self.tool = FlashTool(self.driver, self.symbols)

    def tearDown(self):
        self.fw.stop()

    def test_wait_for_ready(self):
        self.tool.wait_for_ready()

    def test_hello(self):
        self.tool.wait_for_ready()
        self.tool.hello()

    def test_get_info(self):
        self.tool.wait_for_ready()
        self.tool.get_info()
        self.assertEqual(self.tool.capacity, self.fw.capacity)
        self.assertEqual(self.tool.sector_size, self.fw.sector_size)
        self.assertEqual(self.tool.page_size, self.fw.page_size)

    def test_program_data_success(self):
        self.tool.wait_for_ready()
        self.tool.get_info()  # Sets sector size
        data = b"Hello, World!" * 100
        self.tool.program_data(0x0, data)

    def test_program_data_crc_error(self):
        self.tool.wait_for_ready()
        self.tool.get_info()

        # Manually corrupt the send_cmd to send a bad CRC
        original_send_cmd = self.tool.send_cmd

        def bad_crc_send_cmd(cmd, addr=0, length=0, crc32=0):
            return original_send_cmd(cmd, addr, length, crc32 ^ 0xFFFFFFFF)

        self.tool.send_cmd = bad_crc_send_cmd

        data = b"Bad CRC data"
        with self.assertRaisesRegex(RuntimeError, "CRC MISMATCH"):
            self.tool.program_data(0x0, data)

    def test_program_data_unaligned(self):
        self.tool.wait_for_ready()
        self.tool.get_info()

        # Program 140 bytes at an unaligned address (sector_size - 50)
        # This should result in two chunks: 50 bytes and 90 bytes.
        addr = self.tool.sector_size - 50
        data = b"Unaligned data" * 10  # 140 bytes
        self.tool.program_data(addr, data)

    def test_boundary_error(self):
        self.tool.wait_for_ready()
        self.tool.get_info()

        # Force a boundary error by bypassing the lib's alignment logic
        # and sending a command directly.
        with self.assertRaisesRegex(RuntimeError, "failed with status 4"):
            self.tool.send_cmd(
                FLASH_TOOL_CMD_PROGRAM_DATA,
                addr=self.tool.sector_size - 10,
                length=20,
                crc32=0,
            )

    def test_address_error_lib(self):
        self.tool.wait_for_ready()
        # Lib should catch it before sending
        with self.assertRaisesRegex(ValueError, "exceeds 16MB limit"):
            self.tool.program_data(16 * 1024 * 1024, b"too far")

    def test_address_error_fw(self):
        self.tool.wait_for_ready()
        self.tool.get_info()
        # Bypass lib check to test firmware check
        with self.assertRaisesRegex(RuntimeError, "failed with status 8"):
            self.tool.send_cmd(
                FLASH_TOOL_CMD_PROGRAM_DATA,
                addr=16 * 1024 * 1024,
                length=16,
                crc32=0,
            )


if __name__ == "__main__":
    unittest.main()
