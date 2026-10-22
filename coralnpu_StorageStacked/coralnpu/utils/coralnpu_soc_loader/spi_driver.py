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

import os
import socket
import struct


class SPIDriver:
    """A driver that communicates with a DPI-based SPI V2 server in simulation."""

    class CommandType:
        WRITE_REG = 0
        POLL_REG = 1
        IDLE_CLOCKING = 2
        PACKED_WRITE = 3
        BULK_READ = 4
        READ_SPI_DOMAIN_REG = 5
        WRITE_REG_16B = 6
        READ_SPI_DOMAIN_REG_16B = 7
        V2_WRITE = 8
        V2_READ = 9

    # Format: < (little-endian), B (u8 cmd), I (u32 addr), Q (u64 data), I (u32 count)
    COMMAND_FORMAT = "<BIQI"
    # Format: < (little-endian), Q (u64 data), B (u8 success)
    RESPONSE_FORMAT = "<QB"

    def __init__(self, port: int = 5555):
        port_str = os.environ.get("SPI_DPI_PORT")
        self.port = int(port_str) if port_str else port
        print(f"SPI_DRIVER: Connecting to localhost:{self.port}...")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(("localhost", self.port))
        print("SPI_DRIVER: Connected.")

    def close(self):
        if self.sock:
            print("SPI_DRIVER: Closing socket.")
            self.sock.close()
            self.sock = None

    def _send_command(self, cmd_type, addr=0, data=0, count=0, payload=b''):
        cmd_header = struct.pack(
            self.COMMAND_FORMAT, cmd_type, addr, data, count
        )
        self.sock.sendall(cmd_header)
        if payload:
            self.sock.sendall(payload)

        response_data = self.sock.recv(struct.calcsize(self.RESPONSE_FORMAT))
        if not response_data:
            raise ConnectionAbortedError("Socket connection broken.")

        unpacked = struct.unpack(self.RESPONSE_FORMAT, response_data)
        if not unpacked[1]:  # success flag
            raise RuntimeError(f"SPI command {cmd_type} failed in simulation.")
        return unpacked[0]  # data

    def idle_clocking(self, cycles):
        """Sends a command to toggle the SPI clock for a number of cycles."""
        self._send_command(self.CommandType.IDLE_CLOCKING, count=cycles)

    def v2_write(self, target_addr, data_bytes):
        """Writes data using the SPI V2 frame protocol."""
        if not data_bytes:
            return
        if len(data_bytes) % 16 != 0:
            raise ValueError("Data length must be a multiple of 16 bytes")
        num_beats = len(data_bytes) // 16
        # In V2 header, len field is num_beats - 1
        self._send_command(
            self.CommandType.V2_WRITE,
            addr=target_addr,
            count=num_beats - 1,
            payload=data_bytes
        )

    def v2_read(self, target_addr, num_beats):
        """Reads data using the SPI V2 frame protocol."""
        if num_beats == 0:
            return []
        self._send_command(
            self.CommandType.V2_READ, addr=target_addr, count=num_beats - 1
        )
        # Receive the raw data payload following the response header
        num_bytes = num_beats * 16
        read_payload = b''
        while len(read_payload) < num_bytes:
            chunk = self.sock.recv(num_bytes - len(read_payload))
            if not chunk:
                raise ConnectionAbortedError(
                    "Socket connection broken during data read."
                )
            read_payload += chunk
        return list(read_payload)
