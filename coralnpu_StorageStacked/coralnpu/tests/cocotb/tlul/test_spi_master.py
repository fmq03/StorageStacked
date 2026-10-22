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
from cocotb.triggers import ClockCycles, RisingEdge, FallingEdge, with_timeout, Timer
from coralnpu_test_utils.TileLinkULInterface import (
    TileLinkULInterface,
    create_a_channel_req,
)

# --- Constants ---
SPI_MASTER_BASE = 0x40020000
SPI_REG_STATUS = SPI_MASTER_BASE + 0x00
SPI_REG_CONTROL = SPI_MASTER_BASE + 0x04
SPI_REG_TXDATA = SPI_MASTER_BASE + 0x08
SPI_REG_RXDATA = SPI_MASTER_BASE + 0x0C
SPI_REG_CSID = SPI_MASTER_BASE + 0x10
SPI_REG_CSMODE = SPI_MASTER_BASE + 0x14


async def setup_dut(dut):
    """Common setup logic."""
    for signal in dir(dut):
        if signal.startswith("io_external_devices_"):
            if "_d_" in signal or "_a_ready" in signal:
                try:
                    getattr(dut, signal).value = 0
                except AttributeError:
                    pass
        elif signal.startswith("io_external_hosts_"):
            if "_a_" in signal or "_d_ready" in signal:
                try:
                    getattr(dut, signal).value = 0
                except AttributeError:
                    pass
        elif signal.startswith("io_async_ports_"):
            # Initialize async clocks to 0, resets to 1 (active-high reset)
            if "clock" in signal:
                try:
                    getattr(dut, signal).value = 0
                except AttributeError:
                    pass
            elif "reset" in signal:
                try:
                    getattr(dut, signal).value = 1
                except AttributeError:
                    pass

    # Start the main clock
    clock = Clock(dut.io_clk_i, 10, "ns")
    cocotb.start_soon(clock.start())

    # Start the asynchronous test clock (host)
    test_clock = Clock(dut.io_async_ports_hosts_test_clock, 20, "ns")
    cocotb.start_soon(test_clock.start())

    # Start the SPI Master peripheral clock
    # This clock drives the SPI logic in the SpiMaster module
    spim_clock = Clock(
        dut.io_external_ports_spim_clk_i, 100, "ns"
    )  # Slower clock
    cocotb.start_soon(spim_clock.start())

    # Reset the DUT
    dut.io_rst_ni.value = 0
    dut.io_async_ports_hosts_test_reset.value = 1
    await ClockCycles(dut.io_clk_i, 100)
    dut.io_rst_ni.value = 1
    dut.io_async_ports_hosts_test_reset.value = 0
    await ClockCycles(dut.io_clk_i, 20)

    return clock


@cocotb.test()
async def test_spi_master_basic_tx(dut):
    """Tests basic transmission from the SpiMaster."""
    await setup_dut(dut)

    # Instantiate a TL-UL host to drive transactions
    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()

    # Log initial states
    dut._log.info(f"Initial CSB: {dut.io_external_ports_spim_csb.value}")

    # 1. Enable SPI Master
    # Div = 2 (approx), CPOL=0, CPHA=0, Enable=1
    # Control register: Div(15:8), CPHA(2), CPOL(1), Enable(0)
    # 0x0201 -> Div=2, Enable=1
    ctrl_val = (2 << 8) | 1
    dut._log.info(f"Enabling SPI Master with CONTROL=0x{ctrl_val:X}")
    write_txn = create_a_channel_req(
        address=SPI_REG_CONTROL, data=ctrl_val, mask=0xF, width=host_if.width
    )
    await host_if.host_put(write_txn)
    resp = await host_if.host_get_response()
    dut._log.info(f"Response: {resp}")
    assert resp["error"] == 0

    # 2. Assert CSID 0 and Auto Mode
    write_txn = create_a_channel_req(
        address=SPI_REG_CSID, data=0, mask=0xF, width=host_if.width
    )
    await host_if.host_put(write_txn)
    await host_if.host_get_response()

    write_txn = create_a_channel_req(
        address=SPI_REG_CSMODE, data=0, mask=0xF, width=host_if.width
    )  # Auto
    await host_if.host_put(write_txn)
    await host_if.host_get_response()

    # 3. Write Data to TX FIFO
    test_byte = 0xA5  # 10100101
    dut._log.info(f"Writing 0x{test_byte:X} to TXDATA")
    write_txn = create_a_channel_req(
        address=SPI_REG_TXDATA, data=test_byte, mask=0xF, width=host_if.width
    )
    await host_if.host_put(write_txn)
    resp = await host_if.host_get_response()
    assert resp["error"] == 0

    # 4. Monitor SPI Signals
    # Wait for CSB to go Low
    dut._log.info(f"Current CSB: {dut.io_external_ports_spim_csb.value}")
    if dut.io_external_ports_spim_csb.value == 1:
        dut._log.info("Waiting for CSB low...")
        timeout_ns = 5000
        try:
            await with_timeout(
                FallingEdge(dut.io_external_ports_spim_csb), timeout_ns, "ns"
            )
            dut._log.info("CSB went low!")
        except Exception as e:
            dut._log.error(
                f"CSB did not go low within {timeout_ns}ns. Current CSB: {dut.io_external_ports_spim_csb.value}"
            )
            raise e
    else:
        dut._log.info("CSB is already low!")

    # Verify MOSI data
    # CPOL=0, CPHA=0: Data valid on rising edge, changed on falling edge (or setup before first rising)
    # Sample on Rising Edge of SCLK
    received_val = 0
    for i in range(8):
        await RisingEdge(dut.io_external_ports_spim_sclk)  # SCLK Rising
        bit = int(dut.io_external_ports_spim_mosi.value)  # MOSI
        received_val = (received_val << 1) | bit
        dut._log.info(f"Bit {7 - i}: {bit}")

    dut._log.info(f"Received Value: 0x{received_val:X}")
    assert received_val == test_byte, (
        f"Expected 0x{test_byte:X}, got 0x{received_val:X}"
    )

    # Wait for CSB to go High
    await RisingEdge(dut.io_external_ports_spim_csb)
    dut._log.info("CSB went high. Transaction complete.")


@cocotb.test()
async def test_spi_master_half_duplex_rx(dut):
    """Tests half-duplex read mode (HDRX) without manual TXDATA writes."""
    await setup_dut(dut)

    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()

    # 1. Set CSID and CSMODE (Auto)
    await host_if.host_put(
        create_a_channel_req(
            address=SPI_REG_CSID, data=0, mask=0xF, width=host_if.width
        )
    )
    await host_if.host_get_response()
    await host_if.host_put(
        create_a_channel_req(
            address=SPI_REG_CSMODE, data=0, mask=0xF, width=host_if.width
        )
    )
    await host_if.host_get_response()

    # 2. Drive MISO high BEFORE enabling SPI
    dut.io_external_ports_spim_miso.value = 1

    # 3. Enable HDRX (bit 3) + ENABLE (bit 0)
    # 0x0209 -> Div=2, HDRX=1, Enable=1
    ctrl_val = (2 << 8) | (1 << 3) | 1
    dut._log.info(f"Enabling SPI Master with HDRX, CONTROL=0x{ctrl_val:X}")
    await host_if.host_put(
        create_a_channel_req(
            address=SPI_REG_CONTROL,
            data=ctrl_val,
            mask=0xF,
            width=host_if.width
        )
    )
    await host_if.host_get_response()

    # 4. SPI should start automatically. Monitor CSB and SCLK.
    dut._log.info("Waiting for CSB low (automatic start)...")
    await FallingEdge(dut.io_external_ports_spim_csb)
    dut._log.info("CSB went low automatically!")

    # Wait for one byte to finish (CSB goes high in Auto mode after one byte)
    await RisingEdge(dut.io_external_ports_spim_csb)
    dut._log.info("First byte transaction complete.")

    # 5. Check RXDATA
    # STATUS: bit 1 is RX Empty.
    read_status = create_a_channel_req(
        address=SPI_REG_STATUS, is_read=True, width=host_if.width
    )
    await host_if.host_put(read_status)
    resp = await host_if.host_get_response()
    status = int(resp["data"])
    dut._log.info(f"STATUS: 0x{status:X}")
    assert (status & 2) == 0, "RX FIFO should not be empty"

    read_rxdata = create_a_channel_req(
        address=SPI_REG_RXDATA, is_read=True, width=host_if.width
    )
    await host_if.host_put(read_rxdata)
    resp = await host_if.host_get_response()
    rxdata = int(resp["data"])
    dut._log.info(f"RXDATA: 0x{rxdata:X}")
    assert rxdata == 0xFF, f"Expected 0xFF from constant-1 MISO, got 0x{rxdata:X}"


@cocotb.test()
async def test_spi_master_half_duplex_tx(dut):
    """Tests half-duplex write mode (HDTX) where RX data is ignored."""
    await setup_dut(dut)

    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()

    # 1. Enable HDTX (bit 4) + ENABLE (bit 0)
    # 0x0211 -> Div=2, HDTX=1, Enable=1
    ctrl_val = (2 << 8) | (1 << 4) | 1
    dut._log.info(f"Enabling SPI Master with HDTX, CONTROL=0x{ctrl_val:X}")
    await host_if.host_put(
        create_a_channel_req(
            address=SPI_REG_CONTROL,
            data=ctrl_val,
            mask=0xF,
            width=host_if.width
        )
    )
    await host_if.host_get_response()

    # 2. Write 8 bytes to TXDATA.
    # RX FIFO size is 4. If RX data wasn't being ignored, this would eventually
    # stall the master and we'd see 'busy' staying high.
    for i in range(8):
        await host_if.host_put(
            create_a_channel_req(
                address=SPI_REG_TXDATA, data=i, mask=0xF, width=host_if.width
            )
        )
        await host_if.host_get_response()

        # Poll busy bit (bit 0) until it goes low
        while True:
            await host_if.host_put(
                create_a_channel_req(
                    address=SPI_REG_STATUS, is_read=True, width=host_if.width
                )
            )
            resp = await host_if.host_get_response()
            status = int(resp["data"])
            if (status & 1) == 0:
                break
            await Timer(10, units="ns")

    # 3. Verify RX FIFO is STILL empty (bit 1 of STATUS is 1)
    await host_if.host_put(
        create_a_channel_req(
            address=SPI_REG_STATUS, is_read=True, width=host_if.width
        )
    )
    resp = await host_if.host_get_response()
    status = int(resp["data"])
    dut._log.info(f"Final STATUS: 0x{status:X}")
    assert (status & 2) != 0, "RX FIFO should be empty in HDTX mode"
