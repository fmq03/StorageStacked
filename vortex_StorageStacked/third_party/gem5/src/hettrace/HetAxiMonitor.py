"""Transparent memory-side AXI4 HETTrace monitor.

The monitor sits once, after the system interconnect and before the address
decoder for all memory responders.  It does not provide memory timing; it only
projects accepted gem5 packets onto the HETTrace v2 five-channel contract.
"""

from m5.objects.System import System
from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class HetAxiMonitor(SimObject):
    type = "HetAxiMonitor"
    cxx_header = "hettrace/het_axi_monitor.hh"
    cxx_class = "gem5::HetAxiMonitor"

    system = Param.System(Parent.any, "System owning requestor IDs")

    cpu_side_port = ResponsePort("Requests arriving from the system xbar")
    mem_side_port = RequestPort("Requests forwarded to memory responders")

    trace_enable = Param.Bool(
        True, "Enable HETTrace output when HETTRACE_DIR is set"
    )
    trace_host = Param.Bool(True, "Record host memory traffic")
    trace_device = Param.Bool(True, "Record this project's accelerator")
    trace_inst_fetch = Param.Bool(False, "Include instruction fetches")
    axi_data_bytes = Param.Unsigned(32, "AXI256 beat bytes")
    axi_id_bits = Param.Unsigned(8, "Synthesized transaction ID bits")
    unique_packet_ids = Param.Bool(True, "Unique IDs for live packets")
    host_clock_period = Param.Tick(500000, "Host cycle in global fs ticks")
    device_clock_period = Param.Tick(1000000, "Accelerator cycle in global fs ticks")
    device_requestor_patterns = VectorParam.String(["vortex"], "Device requestor names")
