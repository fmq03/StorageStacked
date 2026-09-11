"""Bundled gem5 -> nonblocking TLM -> AXI signals -> independent AXI RAM."""
import argparse
import os
import m5
from m5.objects import (
    System, SrcClockDomain, VoltageDomain, SimpleMemory, AddrRange, SystemXBar,
    Root, SystemC_Kernel, Gem5ToTlmBridge64, AxiDemo, AxiPacketTester,
    X86TimingSimpleCPU, SEWorkload, Process,
)

parser = argparse.ArgumentParser()
parser.add_argument("--mode", choices=["tester", "cpu"], default="tester")
parser.add_argument("--binary")
parser.add_argument("--het-trace", action="store_true",
                    help="Observe target packets using gem5_new HetAxiMonitor")
parser.add_argument("--backend", choices=["ram", "aou"], default="ram")
parser.add_argument("--memory-backend", choices=["simple", "memsim"], default="simple")
parser.add_argument("--memsim-channels", type=int, default=2)
parser.add_argument("--memsim-scale", type=int, default=1)
parser.add_argument("--memsim-queue", type=int, default=4)
parser.add_argument("--memsim-slots", type=int, default=8)
parser.add_argument("--memsim-response-hold", type=int, default=0)
parser.add_argument("--planes", type=int, choices=[1,2,3,4], default=1)
parser.add_argument("--replay", action="store_true")
parser.add_argument("--latency", type=int, default=3)
parser.add_argument("--slots", type=int, default=4)
parser.add_argument("--period", default="2ns")
parser.add_argument("--no-stalls", action="store_true")
parser.add_argument("--response-hold", default="7ns")
parser.add_argument("--max-ticks", type=int, default=10**13)
args = parser.parse_args()
if args.memory_backend == "memsim" and args.backend != "aou":
    parser.error("memsim requires --backend aou")
if min(args.memsim_channels, args.memsim_scale, args.memsim_queue, args.memsim_slots) < 1 or args.memsim_response_hold < 0:
    parser.error("invalid memsim capacities/clock scale")
if args.mode == "cpu" and not args.binary:
    parser.error("--binary required for CPU mode")

# Set before constructing any SystemC time objects or fixing gem5 frequency.
m5.ticks.setGlobalFrequency(10**15)
out = os.path.abspath(m5.options.outdir)
os.makedirs(out, exist_ok=True)
base = 0x90000000
system = System()
system.clk_domain = SrcClockDomain(clock="2GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
host_range = AddrRange(0, size="512MiB")
target_range = AddrRange(base, size="16KiB")
system.mem_ranges = [host_range, target_range]
system.host_mem = SimpleMemory(range=host_range, latency="10ns")
system.axi = AxiDemo(
    base=base, size=8192, period=args.period, outstanding=args.slots,
    backend=args.backend, planes=args.planes, replay=args.replay,
    memory_backend=args.memory_backend, memsim_channels=args.memsim_channels,
    memsim_scale=args.memsim_scale, memsim_queue=args.memsim_queue,
    memsim_slots=args.memsim_slots, memsim_response_hold=args.memsim_response_hold,
    latency=args.latency, stalls=not args.no_stalls, trace_dir=out,
)
system.bridge = Gem5ToTlmBridge64(addr_ranges=[target_range])
system.bridge.tlm = system.axi.tlm
target_port = system.bridge.gem5
if args.het_trace:
    from m5.objects import HetAxiMonitor
    trace_path = os.path.join(out, "hettrace")
    os.makedirs(trace_path, exist_ok=True)
    os.environ["HETTRACE_DIR"] = trace_path
    os.environ["HETTRACE_FILTER"] = "all"
    os.environ["HETTRACE_FORMAT"] = "binary"
    system.het_monitor = HetAxiMonitor(unique_packet_ids=True)
    system.het_monitor.mem_side_port = system.bridge.gem5
    target_port = system.het_monitor.cpu_side_port

if args.mode == "tester":
    system.tester = AxiPacketTester(base=base, trace_dir=out,
                                    response_hold=args.response_hold)
    system.tester.port = target_port
else:
    system.membus = SystemXBar()
    system.host_mem.port = system.membus.mem_side_ports
    system.membus.mem_side_ports = target_port
    system.cpu = X86TimingSimpleCPU()
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
    system.cpu.createInterruptController()
    system.cpu.interrupts[0].pio = system.membus.mem_side_ports
    system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
    system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports
    system.workload = SEWorkload.init_compatible(args.binary)
    process = Process(cmd=[os.path.abspath(args.binary)])
    system.cpu.workload = process
    system.cpu.createThreads()

root = Root(full_system=False, systemc_kernel=SystemC_Kernel(system=system))
m5.instantiate()
if args.mode == "cpu":
    process.map(base, base, 8192, cacheable=False)
event = m5.simulate(args.max_ticks)  # Default 10 ms simulated, finite watchdog
system.axi.finish()
print("EXIT:", event.getCause(), "code", event.getCode(), "tick", m5.curTick())
if args.mode == "tester":
    if event.getCause() != "AXI packet/data/retry tests passed":
        raise RuntimeError("tester did not complete")
else:
    if "exiting with last active thread context" not in event.getCause() or event.getCode() != 0:
        raise RuntimeError("CPU workload did not pass")
