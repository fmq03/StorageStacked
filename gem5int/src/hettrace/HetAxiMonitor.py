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
    trace_host = Param.Bool(True, "Create the host source trace")
    trace_vortex = Param.Bool(False, "Create the Vortex source trace")
    trace_coralnpu = Param.Bool(False, "Create the CoralNPU source trace")
    trace_inst_fetch = Param.Bool(False, "Include host instruction fetches")
    axi_data_bytes = Param.Unsigned(
        16, "Width in bytes used for packet-to-AXI4 projection"
    )
    axi_id_bits = Param.Unsigned(8, "AXI ID width used for synthesized IDs")
    unique_packet_ids = Param.Bool(
        False, "Allocate a synthetic ID per live packet when responses may reorder"
    )

    # Requestor names are allocated by gem5 from SimObject paths.  Keeping the
    # match strings configurable makes the classification auditable without
    # baking a particular top-level object name into C++.
    vortex_requestor_patterns = VectorParam.String(
        ["vortex"], "Substrings identifying Vortex requestors"
    )
    coralnpu_requestor_patterns = VectorParam.String(
        ["coralnpu"], "Substrings identifying CoralNPU requestors"
    )
