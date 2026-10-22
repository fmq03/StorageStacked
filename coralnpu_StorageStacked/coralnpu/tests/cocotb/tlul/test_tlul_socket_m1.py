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
from cocotb.triggers import RisingEdge, ClockCycles, Combine, with_timeout
import random

from coralnpu_test_utils.TileLinkULInterface import create_a_channel_req
from coralnpu_test_utils.TlulVerificationEnv import TlulVerificationEnv


async def setup_dut(dut):
    """Common setup for all tests."""
    clock = Clock(dut.clock, 10, unit="ns")
    cocotb.start_soon(clock.start())
    dut.reset.value = 1
    await ClockCycles(dut.clock, 5)
    dut.reset.value = 0
    await RisingEdge(dut.clock)


@cocotb.test()
async def test_arbitration_and_ordering(dut):
    """Verify fixed-priority arbitration and strict in-order response routing using TlulVerificationEnv."""
    await setup_dut(dut)

    def get_master_from_source(source_id):
        return source_id // 16

    env = TlulVerificationEnv(dut, get_master_from_source)
    M = env.M
    StIdW = (M - 1).bit_length()
    env.scoreboard.device_to_host_source_cb = lambda src: src >> StIdW
    await env.start()
    num_txns_per_master = 5
    responder_tasks = []

    try:
        # 1. Device Responder (Auto-responds on all devices)
        async def device_responder(dev_idx):
            device = env.devices[dev_idx]
            while True:
                req = await device.device_get_request()
                await device.device_respond(
                    opcode=0, param=0, size=req["size"], source=req["source"]
                )

        responder_tasks = [
            cocotb.start_soon(device_responder(j)) for j in range(env.N)
        ]

        # 2. Host Traffic Generators
        async def send_master_traffic(master_idx):
            host = env.hosts[master_idx]
            for j in range(num_txns_per_master):
                source = (master_idx * 16) + j
                req = create_a_channel_req(
                    address=0x1000 + master_idx * 0x100 + j * 4,
                    data=0x11223344 + source,
                    mask=0xF,
                    source=source
                )
                await host.host_put(req)
                if random.random() < 0.3:
                    await ClockCycles(dut.clock, random.randint(1, 3))

        traffic_tasks = [
            cocotb.start_soon(send_master_traffic(i)) for i in range(M)
        ]
        await Combine(*traffic_tasks)

        # 3. Wait for all responses to finish
        total_txns = M * num_txns_per_master
        cycles = 0
        while len(env.scoreboard.global_response_order
                  ) < total_txns and cycles < 200:
            await ClockCycles(dut.clock, 1)
            cycles += 1

        assert len(
            env.scoreboard.global_response_order
        ) == total_txns, "Test timed out waiting for responses"

        # --- VERIFICATION ---
        # Check 1: Fixed-priority arbitration (Master 0 wins first request)
        first_req_master = get_master_from_source(
            env.scoreboard.slave_received_order[0]
        )
        assert first_req_master == 0, f"Priority failure: Master {first_req_master} won over Master 0 at startup"

        # Verify no scoreboard errors
        assert env.scoreboard.errors == 0, f"Scoreboard detected {env.scoreboard.errors} errors during execution"

    finally:
        for r_task in responder_tasks:
            r_task.cancel()
        await env.stop()


@cocotb.test()
async def test_directed_backpressure(dut):
    """Verify backpressure propagation and Head-of-Line blocking on responses."""
    await setup_dut(dut)

    def get_master_from_source(source_id):
        return source_id // 16

    env = TlulVerificationEnv(dut, get_master_from_source)
    M = env.M
    StIdW = (M - 1).bit_length()
    env.scoreboard.device_to_host_source_cb = lambda src: src >> StIdW
    await env.start()

    # Wait for initialization to settle
    await ClockCycles(dut.clock, 1)

    assert M > 1, "This test requires at least 2 masters"

    m0 = env.hosts[0]
    m1 = env.hosts[1]
    device = env.devices[0]

    try:
        # --- PART A: Slave Backpressure propagation to A-Channel ---
        # We disable device ready (set ready = 0)
        dut.io_tl_d_a_ready.value = 0

        # Master 0 sends request
        req0 = create_a_channel_req(address=0x1000, data=0x111, source=0)
        await m0.host_put(req0)

        # Wait and verify blocked
        await ClockCycles(dut.clock, 3)
        assert dut.io_tl_h_0_a_valid.value == 1, "Master did not drive valid"
        assert dut.io_tl_h_0_a_ready.value == 0, "Master ready was not backpressured"

        # Make slave ready
        dut.io_tl_d_a_ready.value = 1
        await ClockCycles(dut.clock, 2)
        assert dut.io_tl_h_0_a_valid.value == 0, "Master request was not accepted"

        # Clean up response
        req_seen = await device.device_get_request()
        await device.device_respond(
            opcode=0,
            param=0,
            size=req_seen["size"],
            source=req_seen["source"]
        )
        await m0.host_get_response()

        # --- PART B: Host Backpressure causing Head-of-Line blocking on D-Channel ---
        req_m0 = create_a_channel_req(address=0x2000, data=0x222, source=0)
        req_m1 = create_a_channel_req(address=0x3000, data=0x333, source=16)

        await m0.host_put(req_m0)
        await m1.host_put(req_m1)

        req_seen_0 = await device.device_get_request()
        req_seen_1 = await device.device_get_request()

        assert (int(req_seen_0["source"]) >> StIdW) == 0
        assert (int(req_seen_1["source"]) >> StIdW) == 16

        # Master 0 backpressures response path
        dut.io_tl_h_0_d_ready.value = 0
        dut.io_tl_h_1_d_ready.value = 1  # Master 1 is ready

        # Send response 0 (for M0)
        async def respond_0():
            await device.device_respond(
                opcode=0,
                param=0,
                size=req_seen_0["size"],
                source=req_seen_0["source"]
            )

        cocotb.start_soon(respond_0())

        await ClockCycles(dut.clock, 5)

        # Send response 1 (for M1)
        async def respond_1():
            await device.device_respond(
                opcode=0,
                param=0,
                size=req_seen_1["size"],
                source=req_seen_1["source"]
            )

        respond_1_task = cocotb.start_soon(respond_1())

        await ClockCycles(dut.clock, 5)

        # Verify Master 1 has NOT received its response yet (because Master 0 is blocking the FIFO head)
        assert m1.host_d_fifo.qsize(
        ) == 0, "Master 1 received response out-of-order"

        # Release backpressure on Master 0
        dut.io_tl_h_0_d_ready.value = 1

        # Now both responses should complete
        await with_timeout(respond_1_task, 10, "ns")

        await m0.host_get_response()
        await m1.host_get_response()

        # Verify no scoreboard errors
        assert env.scoreboard.errors == 0, f"Scoreboard detected errors: {env.scoreboard.errors}"

    finally:
        await env.stop()


@cocotb.test()
async def test_random_backpressure(dut):
    """Verify correct operation with randomized conditional backpressure."""
    await setup_dut(dut)

    def get_master_from_source(source_id):
        return source_id // 16

    env = TlulVerificationEnv(dut, get_master_from_source)
    M = env.M
    StIdW = (M - 1).bit_length()
    env.scoreboard.device_to_host_source_cb = lambda src: src >> StIdW
    await env.start()

    # Configure backpressure:
    env.backpressure_enabled = True
    env.host_bp_prob = 0.4
    env.device_bp_prob = 0.4

    # Conditional backpressure callback:
    # Enable backpressure only during cycles 20 to 120 to verify recovery
    def bp_condition(cycle_count):
        return 20 <= cycle_count <= 120

    env.bp_condition_cb = bp_condition

    M = env.M
    num_txns_per_master = 10
    responder_tasks = []

    try:
        # Responder
        async def device_responder(dev_idx):
            device = env.devices[dev_idx]
            while True:
                req = await device.device_get_request()
                await ClockCycles(dut.clock, random.randint(0, 4))
                await device.device_respond(
                    opcode=0, param=0, size=req["size"], source=req["source"]
                )

        responder_tasks = [
            cocotb.start_soon(device_responder(j)) for j in range(env.N)
        ]

        # Host traffic
        async def send_master_traffic(master_idx):
            host = env.hosts[master_idx]
            for j in range(num_txns_per_master):
                source = (master_idx * 16) + j
                req = create_a_channel_req(
                    address=0x1000 + master_idx * 0x100 + j * 4,
                    data=0x11223344 + source,
                    mask=0xF,
                    source=source
                )
                await host.host_put(req)
                await ClockCycles(dut.clock, random.randint(1, 5))

        traffic_tasks = [
            cocotb.start_soon(send_master_traffic(i)) for i in range(M)
        ]
        await Combine(*traffic_tasks)

        # Wait for completion
        total_txns = M * num_txns_per_master
        cycles = 0
        while len(env.scoreboard.global_response_order
                  ) < total_txns and cycles < 500:
            await ClockCycles(dut.clock, 1)
            cycles += 1

        assert len(
            env.scoreboard.global_response_order
        ) == total_txns, f"Test timed out. Completed {len(env.scoreboard.global_response_order)}/{total_txns} txns"

        # Verification
        assert env.scoreboard.errors == 0, f"Scoreboard detected {env.scoreboard.errors} errors"

    finally:
        for r_task in responder_tasks:
            r_task.cancel()
        await env.stop()
