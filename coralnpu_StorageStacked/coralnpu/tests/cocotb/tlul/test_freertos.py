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
from cocotb.triggers import ClockCycles
from cocotb.clock import Clock
from bazel_tools.tools.python.runfiles import runfiles
import os
import sys

# Add the directory containing test_subsystem to sys.path
sys.path.append(os.path.dirname(__file__))
from test_subsystem import (
    setup_dut,
    load_elf,
    create_a_channel_req,
    TileLinkULInterface,
)


@cocotb.test()
async def test_freertos(dut):
    """Loads and executes a FreeRTOS application.
    Verifies context switching and timer interrupts."""
    clock = await setup_dut(dut)

    host_if = TileLinkULInterface(
        dut,
        host_if_name="io_external_hosts_test_host_32",
        clock_name="io_async_ports_hosts_test_clock",
        reset_name="io_async_ports_hosts_test_reset",
        width=32,
    )
    await host_if.init()

    # UART1 responder for task output
    uart1_if = TileLinkULInterface(
        dut,
        device_if_name="io_external_devices_uart1",
        clock_name="io_clk_i",
        reset_name="io_rst_ni",
        width=32,
    )
    await uart1_if.init()

    captured_output = []

    async def uart1_responder():
        while True:
            req = await uart1_if.device_get_request()
            if int(req["opcode"]) in [0, 1]:
                char = int(req["data"]) & 0xFF
                if char != 0:
                    c = chr(char)
                    sys.stdout.write(c)
                    sys.stdout.flush()
                    captured_output.append(c)
                await uart1_if.device_respond(
                    opcode=0, param=0, size=req["size"], source=req["source"]
                )
            elif int(req["opcode"]) == 4:
                await uart1_if.device_respond(
                    opcode=1,
                    param=0,
                    size=req["size"],
                    source=req["source"],
                    data=0
                )

    cocotb.start_soon(uart1_responder())

    r = runfiles.Create()
    elf_path = r.Rlocation(
        "coralnpu_hw/tests/cocotb/freertos_app/freertos_app.elf"
    )
    assert elf_path, "Could not find freertos_app.elf"

    with open(elf_path, "rb") as f:
        # Highmem uses different offsets, but load_elf handles the absolute addresses in the ELF.
        entry_point = await load_elf(dut, f, host_if)

    dut._log.info(f"FreeRTOS app loaded. Entry point: 0x{entry_point:08x}")

    # Control CSRs for Highmem are at 0x00200000 (standard is 0x00030000)
    coralnpu_reset_csr_addr = 0x200000
    coralnpu_pc_csr_addr = 0x200004

    # Program start PC
    write_txn = create_a_channel_req(
        address=coralnpu_pc_csr_addr,
        data=entry_point,
        mask=0xF,
        width=host_if.width
    )
    await host_if.host_put(write_txn)
    await host_if.host_get_response()

    # Release reset
    write_txn = create_a_channel_req(
        address=coralnpu_reset_csr_addr, data=1, mask=0xF, width=host_if.width
    )
    await host_if.host_put(write_txn)
    await host_if.host_get_response()

    await ClockCycles(dut.io_clk_i, 1)

    write_txn = create_a_channel_req(
        address=coralnpu_reset_csr_addr, data=0, mask=0xF, width=host_if.width
    )
    await host_if.host_put(write_txn)
    await host_if.host_get_response()

    dut._log.info("FreeRTOS running. Waiting for output...")

    # Poll for output in small increments to exit early.
    # We expect to see 'S' (started), then interleaved '1' and '2'.
    # 50k cycles is the upper limit (50 ticks).
    max_cycles = 50_000
    poll_increment = 1000
    cycles_elapsed = 0

    while cycles_elapsed < max_cycles:
        await ClockCycles(dut.io_clk_i, poll_increment)
        cycles_elapsed += poll_increment

        # Check if we have seen both tasks after 'S'
        if "S" in captured_output:
            s_index = captured_output.index("S")
            after_s = captured_output[s_index + 1:]
            if "1" in after_s and "2" in after_s:
                dut._log.info(
                    f"Detected task interleaving after {cycles_elapsed} cycles."
                )
                break

    dut._log.info(f"Captured output: {''.join(captured_output)}")

    assert "S" in captured_output, "FreeRTOS did not start!"
    assert "1" in captured_output, "Task 1 did not run!"
    assert "2" in captured_output, "Task 2 did not run!"

    s_index = captured_output.index("S")
    after_s = captured_output[s_index + 1:]
    assert "1" in after_s and "2" in after_s, "Context switching failed!"

    dut._log.info("FreeRTOS test passed.")
