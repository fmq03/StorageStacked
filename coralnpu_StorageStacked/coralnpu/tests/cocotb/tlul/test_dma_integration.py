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
from cocotb.triggers import ClockCycles, with_timeout, Timer
from coralnpu_test_utils.TileLinkULInterface import (
    TileLinkULInterface,
    create_a_channel_req,
)

# --- DMA Register Map ---
DMA_BASE = 0x40050000
DMA_CTRL = DMA_BASE + 0x00
DMA_STATUS = DMA_BASE + 0x04
DMA_DESC_ADDR = DMA_BASE + 0x08
DMA_CUR_DESC = DMA_BASE + 0x0C
DMA_XFER_REMAIN = DMA_BASE + 0x10

# SRAM base
SRAM_BASE = 0x20000000


# --- Helpers ---
async def setup_dut(dut, host_if):
    """Common setup: clocks and reset."""
    clock = Clock(dut.io_clk_i, 10, "ns")
    cocotb.start_soon(clock.start())

    test_clock = Clock(dut.io_async_ports_hosts_test_clock, 20, "ns")
    cocotb.start_soon(test_clock.start())

    dut.io_rst_ni.value = 0
    dut.io_async_ports_hosts_test_reset.value = 1
    dut.io_external_ports_gpio_i.value = 0
    await ClockCycles(dut.io_clk_i, 5)
    dut.io_rst_ni.value = 1
    dut.io_async_ports_hosts_test_reset.value = 0
    await ClockCycles(dut.io_clk_i, 100)

    # Sanity check: DMA should not be busy after reset
    status = await tl_read(host_if, DMA_STATUS)
    dut._log.info(f"DMA Status after reset: 0x{status:08X}")

    return clock


async def tl_write(host_if, addr, data, source=0x10):
    """Write a 32-bit value via TileLink."""
    txn = create_a_channel_req(
        address=addr, data=data, mask=0xF, width=host_if.width, source=source
    )
    await host_if.host_put(txn)
    resp = await host_if.host_get_response()
    assert resp["error"] == 0, f"TL write error at 0x{addr:08X}"


async def tl_read(host_if, addr, source=0x10):
    """Read a 32-bit value via TileLink."""
    txn = create_a_channel_req(
        address=addr, width=host_if.width, is_read=True, size=2, source=source
    )
    await host_if.host_put(txn)
    resp = await host_if.host_get_response()
    assert resp["error"] == 0, f"TL read error at 0x{addr:08X}"
    return int(resp["data"])


def pack_descriptor_word0_3(
    src_addr,
    dst_addr,
    xfer_len,
    xfer_width=2,
    src_fixed=False,
    dst_fixed=False,
    poll_en=False,
    next_desc=0,
):
    """Pack the first 16 bytes of a descriptor into 4 x 32-bit words."""
    flags = ((xfer_len & 0xFFFFFF)
             | ((xfer_width & 0x7) << 24)
             | ((1 if src_fixed else 0) << 27)
             | ((1 if dst_fixed else 0) << 28)
             | ((1 if poll_en else 0) << 29))
    return [src_addr, dst_addr, flags, next_desc]


async def write_descriptor(
    host_if,
    desc_addr,
    src_addr,
    dst_addr,
    xfer_len,
    xfer_width=2,
    src_fixed=False,
    dst_fixed=False,
    poll_en=False,
    next_desc=0,
    poll_addr=0,
    poll_mask=0,
    poll_value=0,
):
    """Write a 32-byte descriptor to memory via TileLink."""
    words = pack_descriptor_word0_3(
        src_addr,
        dst_addr,
        xfer_len,
        xfer_width,
        src_fixed,
        dst_fixed,
        poll_en,
        next_desc,
    )
    # Words 4-7: poll_addr, poll_mask, poll_value, reserved
    words += [poll_addr, poll_mask, poll_value, 0]

    for i, word in enumerate(words):
        await tl_write(host_if, desc_addr + i * 4, word)


async def poll_dma_done(host_if, timeout_cycles=5000):
    """Poll DMA STATUS register until done or error."""
    for _ in range(timeout_cycles):
        status = await tl_read(host_if, DMA_STATUS)
        if status & 0x2:  # done
            return status
        await ClockCycles(host_if.clock, 1)
    raise TimeoutError("DMA did not complete within timeout")


# --- Tests ---


@cocotb.test()
async def test_dma_csr_access(dut):
    """Test basic DMA CSR read/write."""
    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()
    await setup_dut(dut, host_if)

    # Write and read back DESC_ADDR
    await tl_write(host_if, DMA_DESC_ADDR, 0xDEAD0000)
    val = await tl_read(host_if, DMA_DESC_ADDR)
    assert val == 0xDEAD0000, f"DESC_ADDR mismatch: 0x{val:08X}"

    # STATUS should read 0 initially
    status = await tl_read(host_if, DMA_STATUS)
    assert status == 0, f"STATUS should be 0, got 0x{status:08X}"

    # CTRL: write enable, read back
    await tl_write(host_if, DMA_CTRL, 0x1)  # enable
    ctrl = await tl_read(host_if, DMA_CTRL)
    assert ctrl & 1 == 1, f"CTRL enable not set: 0x{ctrl:08X}"


@cocotb.test()
async def test_dma_mem_to_mem(dut):
    """Test DMA mem-to-mem transfer via SRAM."""
    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()
    await setup_dut(dut, host_if)

    src_addr = SRAM_BASE + 0x0000
    dst_addr = SRAM_BASE + 0x1000
    desc_addr = SRAM_BASE + 0x2000
    xfer_len = 16  # 16 bytes = 4 words

    # Write source data to SRAM
    test_pattern = [0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0xA5A5A5A5]
    for i, word in enumerate(test_pattern):
        await tl_write(host_if, src_addr + i * 4, word)

    # Write descriptor to SRAM
    await write_descriptor(
        host_if, desc_addr, src_addr, dst_addr, xfer_len, xfer_width=2
    )

    # Program DMA
    await tl_write(host_if, DMA_DESC_ADDR, desc_addr)
    await tl_write(host_if, DMA_CTRL, 0x3)  # enable + start

    # Wait for completion
    status = await poll_dma_done(host_if, timeout_cycles=10000)
    assert status & 0x2, f"DMA not done: 0x{status:08X}"
    assert not (status & 0x4), f"DMA error: 0x{status:08X}"

    # Verify destination
    for i, expected in enumerate(test_pattern):
        actual = await tl_read(host_if, dst_addr + i * 4)
        assert actual == expected, (
            f"Word {i}: expected 0x{expected:08X}, got 0x{actual:08X}"
        )

    dut._log.info("DMA mem-to-mem transfer PASSED")


@cocotb.test()
async def test_dma_descriptor_chain(dut):
    """Test DMA with two chained descriptors."""
    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()
    await setup_dut(dut, host_if)

    src0 = SRAM_BASE + 0x0000
    dst0 = SRAM_BASE + 0x1000
    src1 = SRAM_BASE + 0x0100
    dst1 = SRAM_BASE + 0x1100
    desc0 = SRAM_BASE + 0x2000
    desc1 = SRAM_BASE + 0x2020  # 32-byte aligned

    # Write source data
    for i in range(2):
        await tl_write(host_if, src0 + i * 4, 0xAA000000 + i)
    for i in range(2):
        await tl_write(host_if, src1 + i * 4, 0xBB000000 + i)

    # Descriptor 0: 8 bytes src0->dst0, chains to desc1
    await write_descriptor(
        host_if, desc0, src0, dst0, 8, xfer_width=2, next_desc=desc1
    )
    # Descriptor 1: 8 bytes src1->dst1, end of chain
    await write_descriptor(host_if, desc1, src1, dst1, 8, xfer_width=2)

    # Start DMA
    await tl_write(host_if, DMA_DESC_ADDR, desc0)
    await tl_write(host_if, DMA_CTRL, 0x3)

    status = await poll_dma_done(host_if, timeout_cycles=20000)
    assert status & 0x2, "DMA not done"
    assert not (status & 0x4), f"DMA error: 0x{status:08X}"

    # Verify both transfers
    for i in range(2):
        val = await tl_read(host_if, dst0 + i * 4)
        assert val == 0xAA000000 + i, f"Chain 0 word {i}: 0x{val:08X}"
    for i in range(2):
        val = await tl_read(host_if, dst1 + i * 4)
        assert val == 0xBB000000 + i, f"Chain 1 word {i}: 0x{val:08X}"

    dut._log.info("DMA descriptor chaining PASSED")
