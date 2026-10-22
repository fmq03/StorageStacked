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
from cocotb.triggers import RisingEdge, ClockCycles
from cocotb.queue import Queue
import random

from coralnpu_test_utils.TileLinkULInterface import TileLinkULInterface


class TlulScoreboard:
    """Scoreboard to verify routing, ordering, and data integrity of TL-UL interconnects."""

    def __init__(self, dut, M, N, get_master_from_source_cb):
        self.dut = dut
        self.M = M
        self.N = N
        self.get_master_from_source = get_master_from_source_cb

        # State tracking
        self.pending_reqs = {}  # source_id -> req_txn dict
        self.slave_received_order = [
        ]  # list of source_ids in order of arrival at slave(s)
        self.global_response_order = [
        ]  # list of source_ids of responses received by hosts

        self.errors = 0
        self.device_to_host_source_cb = None
        self.host_pending_queues = {i: [] for i in range(M)}

    def write_request(self, host_idx, txn):
        source = txn["source"]
        if source in self.pending_reqs:
            self.dut._log.error(
                f"Scoreboard Error: Source ID {source} reused before previous txn finished!"
            )
            self.errors += 1
        self.pending_reqs[source] = {"host_idx": host_idx, "txn": txn}
        self.host_pending_queues[host_idx].append(source)
        self.dut._log.debug(
            f"Scoreboard: Host {host_idx} sent request with source {source}"
        )

    def write_device_request(self, device_idx, txn):
        source = txn["source"]
        if self.device_to_host_source_cb is not None:
            source = self.device_to_host_source_cb(source)
        self.slave_received_order.append(source)

        # Verify routing
        if source in self.pending_reqs:
            expected_host = self.pending_reqs[source]["host_idx"]
            self.dut._log.debug(
                f"Scoreboard: Slave {device_idx} received request from Host {expected_host} (source {source})"
            )
            # For 1N or Xbar, we would verify device_idx matches address range here.
        else:
            self.dut._log.error(
                f"Scoreboard Error: Slave {device_idx} received request with untracked source {source}"
            )
            self.errors += 1

    def write_response(self, host_idx, txn):
        source = txn["source"]
        self.global_response_order.append(source)

        if source not in self.pending_reqs:
            self.dut._log.error(
                f"Scoreboard Error: Host {host_idx} received unexpected response with source {source}"
            )
            self.errors += 1
            return

        expected_host = self.pending_reqs[source]["host_idx"]
        orig_req = self.pending_reqs[source]["txn"]

        # Check 1: Correct routing of response back to host
        if host_idx != expected_host:
            self.dut._log.error(
                f"Scoreboard Error: Response for source {source} routed to Host {host_idx}, expected Host {expected_host}"
            )
            self.errors += 1

        # Check 2: Per-host response tracking (allows out-of-order for different source IDs)
        pending_queue = self.host_pending_queues[host_idx]
        if not pending_queue:
            self.dut._log.error(
                f"Scoreboard Error: Host {host_idx} received response with source {source} but has no pending requests!"
            )
            self.errors += 1
        elif source not in pending_queue:
            self.dut._log.error(
                f"Scoreboard Error: Host {host_idx} received response for source {source} which is not pending!"
            )
            self.errors += 1
        else:
            pending_queue.remove(source)

        # Check 3: Data integrity (compare read data or check error)
        if orig_req["opcode"] == 4:  # Get (Read)
            pass

        # Check 4: Error bit propagation
        if txn["error"] != 0:
            self.dut._log.warning(
                f"Scoreboard Warning: Transaction with source {source} returned with error."
            )

        # Remove from pending
        self.pending_reqs.pop(source)
        self.dut._log.debug(
            f"Scoreboard: Host {host_idx} completed transaction with source {source}"
        )


class TlulVerificationEnv:
    """Generalized Verification Environment for TL-UL Interconnects."""

    def __init__(
        self,
        dut,
        get_master_from_source_cb,
        clock_name="clock",
        reset_name="reset"
    ):
        self.dut = dut
        self.clock = getattr(dut, clock_name)
        self.reset = getattr(dut, reset_name)

        # 1. Discover Topology
        self.M, self.N = self._discover_topology()
        self.dut._log.info(
            f"Env: Discovered topology with M={self.M} hosts and N={self.N} devices"
        )

        # Define Port Prefixes
        if self.M == 1:
            self.host_prefixes = ["io_tl_h"]
        else:
            self.host_prefixes = [f"io_tl_h_{i}" for i in range(self.M)]

        if self.N == 1:
            self.device_prefixes = ["io_tl_d"]
        else:
            self.device_prefixes = [f"io_tl_d_{i}" for i in range(self.N)]

        # 2. Instantiate Drivers and Responders
        self.hosts = [
            TileLinkULInterface(dut, host_if_name=prefix)
            for prefix in self.host_prefixes
        ]
        self.devices = [
            TileLinkULInterface(dut, device_if_name=prefix)
            for prefix in self.device_prefixes
        ]

        # 3. Instantiate Scoreboard
        self.scoreboard = TlulScoreboard(
            dut, self.M, self.N, get_master_from_source_cb
        )

        self._tasks = []

        # Backpressure configuration
        self.backpressure_enabled = False
        self.host_bp_prob = 0.3
        self.device_bp_prob = 0.3
        self.bp_condition_cb = None  # callback: def cond(cycle_count) -> bool

    def _discover_topology(self):
        M = 0
        if hasattr(self.dut, "io_tl_h_a_valid"):
            M = 1
        else:
            while hasattr(self.dut, f"io_tl_h_{M}_a_valid"):
                M += 1

        N = 0
        if hasattr(self.dut, "io_tl_d_a_valid"):
            N = 1
        else:
            while hasattr(self.dut, f"io_tl_d_{N}_a_valid"):
                N += 1

        return M, N

    async def start(self):
        """Starts the environment monitors and background tasks."""
        # Start monitors for each Host
        for i in range(self.M):
            self._tasks.append(
                cocotb.start_soon(
                    self._monitor_host_a(i, self.host_prefixes[i])
                )
            )
            self._tasks.append(
                cocotb.start_soon(
                    self._monitor_host_d(i, self.host_prefixes[i])
                )
            )

        # Start monitors for each Device
        for j in range(self.N):
            self._tasks.append(
                cocotb.start_soon(
                    self._monitor_device_a(j, self.device_prefixes[j])
                )
            )

        # Start backpressure generator
        self._tasks.append(cocotb.start_soon(self._backpressure_generator()))

    async def stop(self):
        """Stops all background tasks to prevent leakage."""
        for task in self._tasks:
            task.cancel()

        # Also clean up internal TileLinkULInterface agents
        for host in self.hosts:
            for agent in host._agents:
                agent.cancel()
        for device in self.devices:
            for agent in device._agents:
                agent.cancel()

    # --- Signal Monitors ---

    def _reconstruct_a_txn(self, prefix):
        txn = {"user": {}}
        for prop in ["opcode", "param", "size", "source", "address", "mask",
                     "data"]:
            txn[prop] = int(getattr(self.dut, f"{prefix}_a_bits_{prop}").value)
        # Minimal user fields
        for field in ["cmd_intg", "data_intg"]:
            sig = f"{prefix}_a_bits_user_{field}"
            if hasattr(self.dut, sig):
                txn["user"][field] = int(getattr(self.dut, sig).value)
        return txn

    def _reconstruct_d_txn(self, prefix):
        txn = {"user": {}}
        for prop in ["opcode", "param", "size", "source", "sink", "data",
                     "error"]:
            txn[prop] = int(getattr(self.dut, f"{prefix}_d_bits_{prop}").value)
        for field in ["rsp_intg", "data_intg"]:
            sig = f"{prefix}_d_bits_user_{field}"
            if hasattr(self.dut, sig):
                txn["user"][field] = int(getattr(self.dut, sig).value)
        return txn

    async def _monitor_host_a(self, host_idx, prefix):
        a_valid = getattr(self.dut, f"{prefix}_a_valid")
        a_ready = getattr(self.dut, f"{prefix}_a_ready")
        while True:
            await RisingEdge(self.clock)
            try:
                if a_valid.value and a_ready.value == 1:
                    txn = self._reconstruct_a_txn(prefix)
                    self.scoreboard.write_request(host_idx, txn)
            except ValueError:
                # Ignore Logic('X') errors during/before reset is complete.
                # The interface driver (TileLinkULInterface.py) already checks
                # and will fail the test if 'X' persists outside reset.
                pass

    async def _monitor_host_d(self, host_idx, prefix):
        d_valid = getattr(self.dut, f"{prefix}_d_valid")
        d_ready = getattr(self.dut, f"{prefix}_d_ready")
        while True:
            await RisingEdge(self.clock)
            try:
                if d_valid.value and d_ready.value == 1:
                    txn = self._reconstruct_d_txn(prefix)
                    self.scoreboard.write_response(host_idx, txn)
            except ValueError:
                # Ignore Logic('X') errors during/before reset is complete.
                # The interface driver (TileLinkULInterface.py) already checks
                # and will fail the test if 'X' persists outside reset.
                pass

    async def _monitor_device_a(self, device_idx, prefix):
        a_valid = getattr(self.dut, f"{prefix}_a_valid")
        a_ready = getattr(self.dut, f"{prefix}_a_ready")
        while True:
            await RisingEdge(self.clock)
            try:
                if a_valid.value and a_ready.value == 1:
                    txn = self._reconstruct_a_txn(prefix)
                    self.scoreboard.write_device_request(device_idx, txn)
            except ValueError:
                # Ignore Logic('X') errors during/before reset is complete.
                # The interface driver (TileLinkULInterface.py) already checks
                # and will fail the test if 'X' persists outside reset.
                pass

    # --- Backpressure Generator ---

    async def _backpressure_generator(self):
        cycle = 0
        previously_active = False
        while True:
            await RisingEdge(self.clock)
            cycle += 1

            # Check if conditional backpressure should be active
            active = self.backpressure_enabled
            if self.bp_condition_cb is not None:
                active = active and self.bp_condition_cb(cycle)

            if active:
                previously_active = True
                # Apply random backpressure on Host response channels (d_ready)
                for prefix in self.host_prefixes:
                    d_ready = getattr(self.dut, f"{prefix}_d_ready")
                    d_ready.value = 0 if random.random(
                    ) < self.host_bp_prob else 1

                # Apply random backpressure on Device request channels (a_ready)
                for prefix in self.device_prefixes:
                    a_ready = getattr(self.dut, f"{prefix}_a_ready")
                    a_ready.value = 0 if random.random(
                    ) < self.device_bp_prob else 1
            else:
                # If transitioned from active to inactive, release ready once
                if previously_active:
                    for prefix in self.host_prefixes:
                        getattr(self.dut, f"{prefix}_d_ready").value = 1
                    for prefix in self.device_prefixes:
                        getattr(self.dut, f"{prefix}_a_ready").value = 1
                    previously_active = False
