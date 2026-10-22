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
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, FallingEdge, ClockCycles, Combine
import random
import numpy as np

from coralnpu_test_utils.TileLinkULInterface import TileLinkULInterface, create_a_channel_req


class MockSram:

    def __init__(self, dut, word_width=128):
        self.dut = dut
        self.word_width = word_width
        addr_width = len(dut.io_sram_addr)
        self.depth = 1 << addr_width
        self.mem = np.zeros((self.depth, 16), dtype=np.uint8)
        self.pending_resp = None

    async def run(self):
        self.dut.io_sram_rvalid.value = 0
        self.dut.io_sram_rdata.value = 0

        while True:
            # Phase 1: Sample inputs at FallingEdge
            await FallingEdge(self.dut.clock)

            enable = self.dut.io_sram_enable.value == 1
            write = self.dut.io_sram_write.value == 1
            addr = self.dut.io_sram_addr.value.to_unsigned()
            wdata = self.dut.io_sram_wdata.value.to_unsigned() if write else 0
            wmask = self.dut.io_sram_wmask.value.to_unsigned() if write else 0

            if enable:
                if write:
                    wdata_bytes = np.array([(wdata >> (i * 8)) & 0xff
                                            for i in range(16)],
                                           dtype=np.uint8)
                    mask_bytes = np.array([
                        bool((wmask >> i) & 1) for i in range(16)
                    ])
                    self.mem[addr, mask_bytes] = wdata_bytes[mask_bytes]
                    resp_data = 0
                else:
                    bytes_val = self.mem[addr]
                    words64 = bytes_val.view(np.uint64)
                    resp_data = int(words64[0]) | (int(words64[1]) << 64)

                self.pending_resp = {"write": write, "data": resp_data}
            else:
                self.pending_resp = None

            # Phase 2: Drive outputs at RisingEdge
            await RisingEdge(self.dut.clock)
            if self.pending_resp is not None:
                self.dut.io_sram_rvalid.value = 1
                if self.pending_resp["write"]:
                    self.dut.io_sram_rdata.value = 0
                else:
                    self.dut.io_sram_rdata.value = self.pending_resp["data"]
            else:
                self.dut.io_sram_rvalid.value = 0
                self.dut.io_sram_rdata.value = 0


async def setup_dut(dut):
    cocotb.start_soon(Clock(dut.clock, 10, unit="ns").start())
    dut.reset.value = 1
    await ClockCycles(dut.clock, 5)
    dut.reset.value = 0
    await RisingEdge(dut.clock)


def generate_random_transactions(
    num_txns, read_ratio, addr_max, source_width, width=128
):
    txns = []
    expected_responses = []
    model_mem = {}

    for i in range(num_txns):
        is_read = random.random() < read_ratio
        # Align address to word boundary (16 bytes for 128-bit)
        addr = random.randint(0, addr_max // 16) * 16
        source = random.randint(0, (1 << source_width) - 1)

        if is_read:
            txn = create_a_channel_req(
                address=addr, source=source, width=width, is_read=True
            )
            txns.append(txn)

            word_addr = addr // 16
            expected_data = model_mem.get(word_addr, 0)

            expected_resp = {
                "opcode": 1,  # AccessAckData
                "size": txn["size"],
                "source": source,
                "data": expected_data,
                "error": 0
            }
            expected_responses.append(expected_resp)
        else:
            data = random.getrandbits(width)
            mask = 0xffff
            txn = create_a_channel_req(
                address=addr,
                data=data,
                mask=mask,
                source=source,
                width=width,
                is_read=False
            )
            txns.append(txn)

            word_addr = addr // 16
            model_mem[word_addr] = data

            expected_resp = {
                "opcode": 0,  # AccessAck
                "size": txn["size"],
                "source": source,
                "data": 0,
                "error": 0
            }
            expected_responses.append(expected_resp)

    return txns, expected_responses


async def run_tr_test(dut, num_txns, read_ratio, use_backpressure):
    host_if = TileLinkULInterface(
        dut, host_if_name="io_tl", width=128, backpressure=use_backpressure
    )
    await setup_dut(dut)
    sram = MockSram(dut, word_width=128)

    cocotb.start_soon(sram.run())

    addr_width = len(dut.io_sram_addr)
    addr_max = ((1 << addr_width) - 1) * 16
    source_width = len(dut.io_tl_a_bits_source)
    txns, expected_resps = generate_random_transactions(
        num_txns, read_ratio, addr_max, source_width
    )

    async def send_txns():
        for txn in txns:
            await host_if.host_put(txn)
            if random.random() < 0.2:
                await ClockCycles(dut.clock, random.randint(1, 3))

    async def recv_resps():
        for i, expected in enumerate(expected_resps):
            resp = await host_if.host_get_response()

            assert resp["opcode"] == expected[
                "opcode"
            ], f"Txn {i}: Opcode mismatch. Expected {expected['opcode']}, got {resp['opcode']}"
            assert resp["size"] == expected[
                "size"
            ], f"Txn {i}: Size mismatch. Expected {expected['size']}, got {resp['size']}"
            assert resp["source"] == expected[
                "source"
            ], f"Txn {i}: Source mismatch. Expected {expected['source']}, got {resp['source']}"
            assert resp["error"] == expected[
                "error"
            ], f"Txn {i}: Error mismatch. Expected {expected['error']}, got {resp['error']}"
            if expected["opcode"] == 1:
                assert resp["data"] == expected[
                    "data"
                ], f"Txn {i}: Data mismatch.\nExpected: {hex(expected['data'])}\nGot:      {hex(resp['data'])}"

    await Combine(
        cocotb.start_soon(send_txns()), cocotb.start_soon(recv_resps())
    )


@cocotb.test()
async def test_tlul_to_sram_crv_balanced(dut):
    """CRV with balanced read/write and no backpressure."""
    await run_tr_test(
        dut, num_txns=100, read_ratio=0.5, use_backpressure=False
    )


@cocotb.test()
async def test_tlul_to_sram_crv_random_backpressure(dut):
    """CRV with random workload ratio and random backpressure."""
    read_ratio = random.random()  # Random mix of reads and writes (0.0 to 1.0)
    await run_tr_test(
        dut, num_txns=100, read_ratio=read_ratio, use_backpressure=True
    )


@cocotb.test()
async def test_sram_read_write(dut):
    """Write random data to random addresses, then read it back and assert equality."""
    host_if = TileLinkULInterface(dut, host_if_name="io_tl", width=128)
    await setup_dut(dut)
    sram = MockSram(dut, word_width=128)
    cocotb.start_soon(sram.run())

    num_iterations = 50
    addr_width = len(dut.io_sram_addr)
    addr_max = ((1 << addr_width) - 1) * 16
    source_width = len(dut.io_tl_a_bits_source)

    for i in range(num_iterations):
        addr = random.randint(0, addr_max // 16) * 16
        data = random.getrandbits(128)
        source = random.randint(0, (1 << source_width) - 1)

        # Write
        write_txn = create_a_channel_req(
            address=addr,
            data=data,
            mask=0xffff,
            source=source,
            width=128,
            is_read=False
        )
        await host_if.host_put(write_txn)
        write_resp = await host_if.host_get_response()
        assert write_resp["opcode"] == 0
        assert write_resp["source"] == source

        # Read
        read_txn = create_a_channel_req(
            address=addr, source=source, width=128, is_read=True
        )
        await host_if.host_put(read_txn)
        read_resp = await host_if.host_get_response()
        assert read_resp["opcode"] == 1
        assert read_resp["source"] == source
        assert read_resp[
            "data"
        ] == data, f"Mismatch at {hex(addr)}: expected {hex(data)}, got {hex(read_resp['data'])}"


@cocotb.test()
async def test_sram_masking(dut):
    """Perform partial-word writes (using different masks) and verify only masked bytes are updated."""
    host_if = TileLinkULInterface(dut, host_if_name="io_tl", width=128)
    await setup_dut(dut)
    sram = MockSram(dut, word_width=128)
    cocotb.start_soon(sram.run())

    addr_width = len(dut.io_sram_addr)
    addr_max = ((1 << addr_width) - 1) * 16
    num_iterations = 50
    source_width = len(dut.io_tl_a_bits_source)

    for i in range(num_iterations):
        addr = random.randint(0, addr_max // 16) * 16
        initial_data = random.getrandbits(128)
        source = random.randint(0, (1 << source_width) - 1)

        # 1. Write initial data (full mask)
        write_txn = create_a_channel_req(
            address=addr,
            data=initial_data,
            mask=0xffff,
            source=source,
            width=128,
            is_read=False
        )
        await host_if.host_put(write_txn)
        await host_if.host_get_response()

        # 2. Write partial data with random mask
        mask = random.randint(1, 0xfffe)
        partial_data = random.getrandbits(128)
        write_partial_txn = create_a_channel_req(
            address=addr,
            data=partial_data,
            mask=mask,
            source=source,
            width=128,
            is_read=False
        )
        await host_if.host_put(write_partial_txn)
        await host_if.host_get_response()

        # Calculate expected data
        expected_data = 0
        for byte_idx in range(16):
            if (mask >> byte_idx) & 1:
                expected_data |= ((partial_data >> (byte_idx * 8)) & 0xff) << (
                    byte_idx * 8
                )
            else:
                expected_data |= ((initial_data >> (byte_idx * 8)) & 0xff) << (
                    byte_idx * 8
                )

        # 3. Read back and verify
        read_txn = create_a_channel_req(
            address=addr, source=source, width=128, is_read=True
        )
        await host_if.host_put(read_txn)
        read_resp = await host_if.host_get_response()
        assert read_resp[
            "data"
        ] == expected_data, f"Masking mismatch at {hex(addr)} with mask {hex(mask)}:\nExpected: {hex(expected_data)}\nGot:      {hex(read_resp['data'])}"


@cocotb.test()
async def test_sram_pipelining(dut):
    """Send multiple requests back-to-back and verify responses return in order."""
    host_if = TileLinkULInterface(dut, host_if_name="io_tl", width=128)
    await setup_dut(dut)
    sram = MockSram(dut, word_width=128)
    cocotb.start_soon(sram.run())

    pipeline_depth = 4
    source_width = len(dut.io_tl_a_bits_source)

    # 1. Pipelined Writes
    write_txns = []
    write_expected = []
    datas = []
    for i in range(pipeline_depth):
        addr = i * 16
        data = random.getrandbits(128)
        datas.append(data)
        source = i % (1 << source_width)
        txn = create_a_channel_req(
            address=addr,
            data=data,
            mask=0xffff,
            source=source,
            width=128,
            is_read=False
        )
        write_txns.append(txn)
        write_expected.append({"opcode": 0, "source": source})

    for txn in write_txns:
        await host_if.host_put(txn)

    for i, expected in enumerate(write_expected):
        resp = await host_if.host_get_response()
        assert resp["opcode"] == expected["opcode"]
        assert resp["source"] == expected["source"]

    # 2. Pipelined Reads
    read_txns = []
    read_expected = []
    for i in range(pipeline_depth):
        addr = i * 16
        source = (i + 10) % (1 << source_width)
        txn = create_a_channel_req(
            address=addr, source=source, width=128, is_read=True
        )
        read_txns.append(txn)
        read_expected.append({"opcode": 1, "source": source, "data": datas[i]})

    for txn in read_txns:
        await host_if.host_put(txn)

    for i, expected in enumerate(read_expected):
        resp = await host_if.host_get_response()
        assert resp["opcode"] == expected["opcode"]
        assert resp["source"] == expected["source"]
        assert resp["data"] == expected["data"]
