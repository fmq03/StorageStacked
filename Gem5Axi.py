from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmTargetSocket
from m5.SimObject import SimObject, PyBindMethod
from m5.params import *
from m5.proxy import Parent


class AxiDemo(SystemC_ScModule):
    type = 'AxiDemo'
    cxx_class = 'storage_axi::Demo'
    cxx_header = 'gem5_axi/axi_demo.hh'
    cxx_exports = [PyBindMethod('finish')]
    tlm = TlmTargetSocket(64, 'Nonblocking transaction input')
    backend = Param.String('ram', 'ram or aou full UCIe path')
    planes = Param.Unsigned(1, 'AoU resource planes (1..4)')
    replay = Param.Bool(False, 'Inject reproducible 2% physical flit errors')
    memory_backend = Param.String('simple', 'simple or memsim after UCIe')
    memsim_channels = Param.Unsigned(2, 'HBM4 channel count for integration')
    memsim_scale = Param.Unsigned(1, 'Multiply native HBM clock period')
    memsim_queue = Param.Unsigned(4, 'Native ingress/controller/response capacity')
    memsim_slots = Param.Unsigned(8, 'Bounded in-flight AXI bursts')
    memsim_response_hold = Param.Unsigned(0, 'Stress-test response hold in memory ticks')
    period = Param.Latency('2ns', 'AXI clock period')
    base = Param.Addr(0x90000000, 'RAM base')
    size = Param.UInt64(8192, 'Target memory window size in bytes')
    outstanding = Param.Unsigned(4, 'Bounded TLM transaction slots')
    latency = Param.Unsigned(3, 'RAM response latency in AXI cycles')
    stalls = Param.Bool(True, 'Deterministic stalls on all five channels')
    trace_dir = Param.String('', 'Directory for CSV/VCD traces')


class AxiPacketTester(SimObject):
    type = 'AxiPacketTester'
    cxx_class = 'gem5::AxiPacketTester'
    cxx_header = 'gem5_axi/packet_tester.hh'
    system = Param.System(Parent.any, 'Parent system')
    port = RequestPort('Timing/functional request source')
    base = Param.Addr(0x90000000, 'Target base')
    trace_dir = Param.String('', 'Result directory')
    response_hold = Param.Latency('7ns', 'Hold each response before retry')
