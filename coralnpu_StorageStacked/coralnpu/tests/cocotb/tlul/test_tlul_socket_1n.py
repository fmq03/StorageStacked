# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
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
async def test_steering_and_locking(dut):
    """Verify routing steering and ordering lock behavior (blocking requests to new device)."""
    await setup_dut(dut)

    def get_master_from_source(source_id):
        return 0

    env = TlulVerificationEnv(dut, get_master_from_source)

    expected_devices = {}  # source -> expected_device_idx

    # Dynamically hook into the scoreboard to verify steering
    orig_write_device_request = env.scoreboard.write_device_request

    def check_routing(device_idx, txn):
        source = txn["source"]
        assert source in expected_devices, f"Scoreboard Error: Untracked source {source} at device {device_idx}"
        expected_dev = expected_devices.pop(source)
        assert device_idx == expected_dev, f"Routing Error: Request with source {source} went to Device {device_idx}, expected Device {expected_dev}"
        orig_write_device_request(device_idx, txn)

    env.scoreboard.write_device_request = check_routing

    await env.start()
    await ClockCycles(dut.clock, 1)

    assert env.N > 1, "This test requires at least 2 device ports"
    host = env.hosts[0]
    device0 = env.devices[0]
    device1 = env.devices[1]

    # Helper to wait for host request handshake to complete
    async def wait_host_handshake():
        while True:
            await RisingEdge(dut.clock)
            if dut.io_tl_h_a_valid.value == 1 and dut.io_tl_h_a_ready.value == 1:
                return

    try:
        # 1. Send Request 0 to Device 0
        dut.io_dev_select_i.value = 0
        req0 = create_a_channel_req(address=0x1000, data=0x111, source=0)
        expected_devices[0] = 0
        await host.host_put(req0)
        await wait_host_handshake()

        # 2. Send Request 1 to Device 0 (outstanding count on Device 0 becomes 2)
        dut.io_dev_select_i.value = 0
        req1 = create_a_channel_req(address=0x1004, data=0x222, source=1)
        expected_devices[1] = 0
        await host.host_put(req1)
        await wait_host_handshake()

        # 3. Send Request 2 to Device 1 (Should complete handshake immediately without blocking)
        dut.io_dev_select_i.value = 1
        req2 = create_a_channel_req(address=0x2000, data=0x333, source=2)
        expected_devices[2] = 1

        await host.host_put(req2)
        await wait_host_handshake()  # Handshake completes immediately!

        # 4. Verify all requests are outstanding at their respective devices
        dev0_req0 = await device0.device_get_request()
        dev0_req1 = await device0.device_get_request()
        dev1_req2 = await device1.device_get_request()

        assert dev0_req0["source"] == 0
        assert dev0_req1["source"] == 1
        assert dev1_req2["source"] == 2

        # 5. Respond to Request 2 on Device 1 first
        await device1.device_respond(
            opcode=0,
            param=0,
            size=dev1_req2["size"],
            source=dev1_req2["source"]
        )
        resp2 = await host.host_get_response()
        assert resp2[
            "source"
        ] == 2, f"Expected response with source 2 (Device 1), got {resp2['source']}"

        # 6. Respond to Request 0 and 1 on Device 0
        await device0.device_respond(
            opcode=0,
            param=0,
            size=dev0_req0["size"],
            source=dev0_req0["source"]
        )
        await device0.device_respond(
            opcode=0,
            param=0,
            size=dev0_req1["size"],
            source=dev0_req1["source"]
        )

        resp0 = await host.host_get_response()
        resp1 = await host.host_get_response()
        assert resp0["source"] == 0
        assert resp1["source"] == 1

        assert env.scoreboard.errors == 0, f"Scoreboard detected {env.scoreboard.errors} errors"

    finally:
        await env.stop()


@cocotb.test()
async def test_error_response(dut):
    """Verify error response for out-of-bounds dev_select."""
    await setup_dut(dut)

    def get_master_from_source(source_id):
        return 0

    env = TlulVerificationEnv(dut, get_master_from_source)
    await env.start()
    await ClockCycles(dut.clock, 1)

    host = env.hosts[0]
    N = env.N

    # Set selection out of bounds (index N) to trigger internal ErrorResponder
    dut.io_dev_select_i.value = N
    req = create_a_channel_req(address=0xBAD, data=0xBAD, mask=0xF, source=42)

    try:
        await host.host_put(req)
        response = await host.host_get_response()

        assert response["error"] == 1, "Expected error bit to be set"
        assert response["source"] == 42
        assert env.scoreboard.errors == 0, f"Scoreboard detected {env.scoreboard.errors} errors"
    finally:
        await env.stop()


@cocotb.test()
async def test_random_backpressure(dut):
    """Verify correct routing and lock recovery under randomized backpressure."""
    await setup_dut(dut)

    def get_master_from_source(source_id):
        return 0

    env = TlulVerificationEnv(dut, get_master_from_source)

    expected_devices = {}

    # Hook into scoreboard
    orig_write_device_request = env.scoreboard.write_device_request

    def check_routing(device_idx, txn):
        source = txn["source"]
        assert source in expected_devices, f"Scoreboard Error: Untracked source {source} at device {device_idx}"
        expected_dev = expected_devices.pop(source)
        assert device_idx == expected_dev, f"Routing Error: Request with source {source} went to Device {device_idx}, expected Device {expected_dev}"
        orig_write_device_request(device_idx, txn)

    env.scoreboard.write_device_request = check_routing

    await env.start()
    await ClockCycles(dut.clock, 1)

    # Configure backpressure
    env.backpressure_enabled = True
    env.host_bp_prob = 0.3
    env.device_bp_prob = 0.3

    def bp_condition(cycle_count):
        return 20 <= cycle_count <= 150

    env.bp_condition_cb = bp_condition

    N = env.N
    num_txns = 15
    host = env.hosts[0]

    # Device Responders with random delay
    async def device_responder(dev_idx):
        device = env.devices[dev_idx]
        while True:
            req = await device.device_get_request()
            await ClockCycles(dut.clock, random.randint(0, 4))
            await device.device_respond(
                opcode=0, param=0, size=req["size"], source=req["source"]
            )

    responder_tasks = [
        cocotb.start_soon(device_responder(j)) for j in range(N)
    ]

    try:
        for i in range(num_txns):
            dev_select = random.randint(0, N - 1)
            dut.io_dev_select_i.value = dev_select

            source = i
            expected_devices[source] = dev_select

            req = create_a_channel_req(
                address=0x1000 + i * 4,
                data=0x11223344 + i,
                mask=0xF,
                source=source
            )

            await host.host_put(req)

            # Wait for handshake to propagate to the device
            cycles = 0
            while source in expected_devices and cycles < 100:
                await ClockCycles(dut.clock, 1)
                cycles += 1

            assert source not in expected_devices, f"Transaction {source} timed out under backpressure"

            # Consume response
            resp = await host.host_get_response()
            assert resp["source"] == source

        # Wait for all remaining responses to propagate
        await ClockCycles(dut.clock, 20)
        assert env.scoreboard.errors == 0, f"Scoreboard detected {env.scoreboard.errors} errors"

    finally:
        for r_task in responder_tasks:
            r_task.cancel()
        await env.stop()
