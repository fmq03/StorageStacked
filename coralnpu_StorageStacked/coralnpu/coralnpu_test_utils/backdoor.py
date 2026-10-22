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

import ctypes
import logging

# --- ctypes setup for SRAM backdoor loading ---
# We use CDLL(None) to access symbols already loaded in the process (the simulator).
try:
    lib = ctypes.CDLL(None)
    sram_backdoor_load = lib.sram_backdoor_load_c
    # Explicitly set argument types for correctness
    sram_backdoor_load.argtypes = [
        ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t
    ]
    sram_backdoor_load.restype = ctypes.c_bool
except (AttributeError, Exception):
    # This might happen if the test is run on a simulator that doesn't have the DPI linked,
    # or if the simulator wasn't built with -rdynamic.
    sram_backdoor_load = None


def backdoor_load(addr, data):
    """Loads data into the simulator's SRAM via the C++ backdoor.

    Args:
        addr: Global byte address.
        data: bytes or bytearray of data to load.

    Raises:
        RuntimeError: if the backdoor load function is not found or fails.
    """
    if sram_backdoor_load is None:
        raise RuntimeError(
            "sram_backdoor_load_c symbol not found in the simulator process. "
            "Ensure the DPI library is linked and Verilator is run with -rdynamic."
        )

    data_bytes = bytes(data)
    # Using cast to void_p to ensure it's handled as a pointer
    if not sram_backdoor_load(
            ctypes.c_uint64(addr),
            ctypes.cast(ctypes.c_char_p(data_bytes), ctypes.c_void_p),
            ctypes.c_size_t(len(data_bytes)),
    ):
        raise RuntimeError(f"Backdoor load failed for address 0x{addr:x}")
