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
from cocotb.triggers import ClockCycles
from coralnpu_test_utils.TileLinkULInterface import (
    TileLinkULInterface,
    create_a_channel_req,
)

# --- Constants ---
GPIO_BASE = 0x40030000
GPIO_DATA_IN = GPIO_BASE + 0x00
GPIO_DATA_OUT = GPIO_BASE + 0x04
GPIO_OUT_EN = GPIO_BASE + 0x08


async def setup_dut(dut):
    """Common setup logic."""
    clock = Clock(dut.io_clk_i, 10, "ns")
    cocotb.start_soon(clock.start())

    # Start host clock
    test_clock = Clock(dut.io_async_ports_hosts_test_clock, 20, "ns")
    cocotb.start_soon(test_clock.start())

    dut.io_rst_ni.value = 0
    dut.io_async_ports_hosts_test_reset.value = 1
    dut.io_external_ports_gpio_i.value = 0
    await ClockCycles(dut.io_clk_i, 5)
    dut.io_rst_ni.value = 1
    dut.io_async_ports_hosts_test_reset.value = 0
    await ClockCycles(dut.io_clk_i, 20)

    return clock


@cocotb.test()
async def test_gpio_integration(dut):
    """Tests GPIO integration in the subsystem."""
    await setup_dut(dut)

    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()

    # 1. Test Output
    test_val = 0xDEADBEEF
    expected_val = 0xEF  # 8-bit LSB
    dut._log.info(f"Writing 0x{test_val:X} to GPIO DATA_OUT")
    write_txn = create_a_channel_req(
        address=GPIO_DATA_OUT,
        data=test_val,
        mask=0xF,
        width=host_if.width,
        source=0x10
    )
    await host_if.host_put(write_txn)
    resp = await host_if.host_get_response()
    assert resp["error"] == 0

    # Check external port
    # Note: cocotb reads UInt ports as integer values
    gpio_o = int(dut.io_external_ports_gpio_o.value)
    dut._log.info(f"GPIO Output: 0x{gpio_o:X}")
    assert gpio_o == expected_val

    # 2. Test Input
    input_val = 0xCAFEBABE
    expected_input = 0xBE  # 8-bit LSB
    dut._log.info(f"Driving 0x{input_val:X} to GPIO Input")
    dut.io_external_ports_gpio_i.value = expected_input
    await ClockCycles(dut.io_clk_i, 5)

    dut._log.info(f"Host IF width: {host_if.width}")
    # Read from 0x40030000
    read_txn = create_a_channel_req(
        address=GPIO_DATA_IN,
        width=host_if.width,
        is_read=True,
        size=0,
        source=0x10
    )
    dut._log.info(f"Read TXN: {read_txn}")
    await host_if.host_put(read_txn)
    resp = await host_if.host_get_response()
    dut._log.info(f"Response: {resp}")
    assert resp["error"] == 0
    read_data = int(resp["data"])
    assert read_data == expected_input
    dut._log.info(f"Read back input: 0x{read_data:X}")
