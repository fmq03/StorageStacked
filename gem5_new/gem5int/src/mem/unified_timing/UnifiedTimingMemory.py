from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class UnifiedTimingMemory(ClockedObject):
    """Sparse, multi-range timing memory with one shared service queue.

    The sparse backing avoids allocating the 4-GiB Vortex BAR eagerly.  All
    listed ranges share the same port, bandwidth regulator and response queue,
    so CPU, NPU and GPU requests contend at one modeled controller boundary.
    """

    type = "UnifiedTimingMemory"
    cxx_header = "mem/unified_timing/unified_timing_memory.hh"
    cxx_class = "gem5::memory::UnifiedTimingMemory"

    port = ResponsePort("Unified CPU/NPU/GPU timing request port")
    ranges = VectorParam.AddrRange([], "Non-overlapping physical ranges")
    latency = Param.Latency("30ns", "Accepted request to response latency")
    bandwidth = Param.MemoryBandwidth(
        "12.8GiB/s", "Combined read/write controller bandwidth"
    )
