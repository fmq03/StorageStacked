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

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge
from coralnpu_test_utils.spi_constants import SPI_V2_OP_READ, SPI_V2_OP_WRITE, SPI_V2_BEAT_BYTES


class SPIMaster:

    def __init__(self, clk, csb, mosi, miso, main_clk, log):
        self.clk = clk
        self.csb = csb
        self.mosi = mosi
        self.miso = miso
        self.main_clk = main_clk
        self.log = log
        self.spi_clk_driver = Clock(
            self.clk, 26, "ns"
        )  # Must not divide evenly into sys clock (10ns)
        self.clock_task = None

        # Initialize signal values
        self.clk.value = 0
        self.csb.value = 1
        self.mosi.value = 0

    async def start_clock(self):
        if self.clock_task is None:
            self.clock_task = self.spi_clk_driver.start()

    async def stop_clock(self):
        if self.clock_task is not None:
            self.spi_clk_driver.stop()
            self.clock_task = None
            self.clk.value = 0

    async def _set_cs(self, active):
        self.csb.value = not active

    async def _clock_byte(self, data_out):
        data_in = 0
        for i in range(8):
            self.mosi.value = (data_out >> (7 - i)) & 1
            await FallingEdge(self.clk)
            data_in = (data_in << 1) | int(self.miso.value)
        return data_in

    async def idle_clocking(self, cycles):
        await self.start_clock()
        await ClockCycles(self.clk, cycles)
        await self.stop_clock()

    async def send_header(self, op, addr, length):
        """Send a 7-byte V2 frame header: [op, addr(4B BE), len(2B BE)]."""
        await self._clock_byte(op & 0xFF)
        await self._clock_byte((addr >> 24) & 0xFF)
        await self._clock_byte((addr >> 16) & 0xFF)
        await self._clock_byte((addr >> 8) & 0xFF)
        await self._clock_byte(addr & 0xFF)
        await self._clock_byte((length >> 8) & 0xFF)
        await self._clock_byte(length & 0xFF)

    async def write_line(self, addr, data_128bit):
        """Write a single 128-bit line using V2 frame protocol."""
        await self._set_cs(True)
        await ClockCycles(self.main_clk, 1)
        await self.start_clock()

        # Send header: op=WRITE, len=0 (single beat)
        await self.send_header(SPI_V2_OP_WRITE, addr, 0)

        # Send 16 payload bytes, LSB first
        for i in range(SPI_V2_BEAT_BYTES):
            await self._clock_byte((data_128bit >> (i * 8)) & 0xFF)

        # Keep SPI clock running so AsyncQueue CDC pipeline can drain
        # (both clocks must be active for the handshake protocol).
        await ClockCycles(self.main_clk, 100)
        await self.stop_clock()
        await ClockCycles(self.main_clk, 1)
        await self._set_cs(False)
        await ClockCycles(self.main_clk, 50)

    async def read_line(self, addr):
        """Read a single 128-bit line using V2 frame protocol.

        Returns the 128-bit data as an integer.
        """
        await self._set_cs(True)
        await ClockCycles(self.main_clk, 1)
        await self.start_clock()

        # Send header: op=READ, len=0 (single beat)
        await self.send_header(SPI_V2_OP_READ, addr, 0)

        # Collect MISO bits — V2 response has variable CDC latency.
        # Collect extra bits and search for the response data pattern.
        # We need 128 bits (16 bytes) of response data, but there's latency
        # through the CDC path, so we collect extra and search.
        num_extra_bytes = 64  # generous margin for AsyncQueue CDC latency
        total_bytes = SPI_V2_BEAT_BYTES + num_extra_bytes
        miso_bytes = []
        for _ in range(total_bytes):
            byte_in = await self._clock_byte(0x00)
            miso_bytes.append(byte_in)

        # Keep SPI clock running so AsyncQueue CDC can drain
        await ClockCycles(self.main_clk, 100)
        await self.stop_clock()
        await ClockCycles(self.main_clk, 1)
        await self._set_cs(False)
        await ClockCycles(self.main_clk, 50)

        # Fast path: Byte-level search (enabled by hardware byte-alignment).
        miso_bytes_raw = bytes(miso_bytes)
        sync_idx = miso_bytes_raw.find(b'\xfe')
        if sync_idx != -1 and sync_idx + 1 + 16 <= len(miso_bytes_raw):
            data_bytes = miso_bytes_raw[sync_idx + 1:sync_idx + 17]
            return int.from_bytes(data_bytes, 'little')

        # Fallback: Efficient bit-level search for 0xFE token (11111110) if misalignment occurs.
        # Convert byte array to a single large integer for bitwise operations.
        miso_int = int.from_bytes(miso_bytes_raw, 'big')
        total_bits = len(miso_bytes_raw) * 8

        # Sliding window search: mask 8 bits and compare to 0xFE.
        # We search from MSB to LSB (SPI order).
        for bit_off in range(total_bits - 8 - 128):
            shift = total_bits - 8 - bit_off
            if ((miso_int >> shift) & 0xFF) == 0xFE:
                # Found token. Extract 128 data bits following it.
                data_bits = (miso_int >> (shift - 128)) & ((1 << 128) - 1)

                # Reverse byte order: SPI shifts out MSB-first, but TileLink is little-endian.
                result = 0
                for i in range(16):
                    byte = (data_bits >> (i * 8)) & 0xFF
                    result |= byte << ((15 - i) * 8)
                return result

        return 0

    async def write_multi(self, addr, data_list):
        """Write multiple 128-bit beats using V2 frame protocol."""
        await self._set_cs(True)
        await ClockCycles(self.main_clk, 1)
        await self.start_clock()

        # Send header: len = num_beats - 1
        await self.send_header(SPI_V2_OP_WRITE, addr, len(data_list) - 1)

        # Send payload bytes for each beat, LSB first
        for data_128bit in data_list:
            for i in range(SPI_V2_BEAT_BYTES):
                await self._clock_byte((data_128bit >> (i * 8)) & 0xFF)

        await self.stop_clock()
        await ClockCycles(self.main_clk, 1)
        await self._set_cs(False)
        await ClockCycles(self.main_clk, 50)

    async def read_multi(self, addr, num_beats):
        """Read multiple 128-bit beats using V2 frame protocol.

        Returns a list of 128-bit integers.
        """
        results = []
        # For simplicity, do individual reads (multi-beat read response
        # parsing through CDC is complex in cocotb)
        for i in range(num_beats):
            beat_addr = addr + i * SPI_V2_BEAT_BYTES
            result = await self.read_line(beat_addr)
            results.append(result)
        return results
