"""Host-controlled Vortex/CoralNPU with all target memory behind AXI/UCIe."""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
from types import SimpleNamespace
import m5
from m5.objects import (AddrRange,AxiDemo,Root,SEWorkload,Process,System,SystemXBar,
                       SimpleMemory,SrcClockDomain,VoltageDomain,SystemC_Kernel,
                       Gem5ToTlmBridge64,HetAxiMonitor)
root_path=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('het_frontends',root_path/'gem5_new/gem5int/configs/het/het_system.py')
front=importlib.util.module_from_spec(spec);spec.loader.exec_module(front)
p=argparse.ArgumentParser()
p.add_argument('--cmd',required=True)
p.add_argument('--options',default='')
p.add_argument('--vortex-library',default='')
p.add_argument('--vortex-host-rt-dir',default='')
p.add_argument('--npu-library',default='')
p.add_argument('--npu-kernel',default='')
p.add_argument('--num-cpus',type=int,default=4)
p.add_argument('--memsim-scale',type=int,default=1)
p.add_argument('--max-ticks',type=int,default=20_000_000_000_000)
p.add_argument('--replay',action='store_true')
a=p.parse_args()
args=SimpleNamespace(**vars(a),env=[],vortex_fast_forward=False,vortex_kernel='',
    vortex_bar_skew=0,npu_auto_start=False,npu_no_share=False)
for filename in (a.cmd,a.vortex_library,a.npu_library,a.npu_kernel):
    if filename and not Path(filename).is_file():raise RuntimeError('Missing '+filename)
if a.vortex_library and a.num_cpus<2:raise RuntimeError('Vortex runtime needs at least two CPU contexts')
out=Path(m5.options.outdir).resolve();out.mkdir(parents=True,exist_ok=True)
m5.ticks.setGlobalFrequency('1fs')
(out/'hettrace').mkdir(exist_ok=True)
os.environ['HETTRACE_DIR']=str(out/'hettrace')
os.environ['HETTRACE_FILTER']='all'
os.environ['HETTRACE_FORMAT']='binary'
system=System()
system.clk_domain=SrcClockDomain(clock=front.HOST_CLOCK,voltage_domain=VoltageDomain())
system.mem_mode='timing'
system.membus=SystemXBar()
system.system_port=system.membus.cpu_side_ports
host_range=AddrRange(front.HOST_HEAP[0],size=front.HOST_HEAP[1])
system.mem_ranges=[host_range]
system.host_mem=SimpleMemory(range=host_range,latency='10ns',conf_table_reported=True)
system.host_mem.port=system.membus.mem_side_ports
ranges=[AddrRange(front.SHARED_BUFFER[0],size=front.SHARED_BUFFER[1]),
        AddrRange(front.NPU_WORK[0],size=front.NPU_WORK[1])]
if a.vortex_library:ranges.append(AddrRange(front.VORTEX_BAR[0],size=front.VORTEX_BAR[1]))
size=0x170000000 if a.vortex_library else 0x30000000
system.axi=AxiDemo(backend='aou',memory_backend='memsim',base=0x90000000,size=size,
    memsim_channels=8 if a.vortex_library else 2,memsim_scale=a.memsim_scale,
    memsim_queue=4,memsim_slots=8,outstanding=16,planes=2,stalls=True,replay=a.replay,
    trace_dir=str(out))
system.bridge=Gem5ToTlmBridge64(addr_ranges=ranges)
system.bridge.tlm=system.axi.tlm
system.het_monitor=HetAxiMonitor(unique_packet_ids=True,trace_host=True,
    trace_vortex=bool(a.vortex_library),trace_coralnpu=bool(a.npu_library))
system.het_monitor.mem_side_port=system.bridge.gem5
system.het_monitor.cpu_side_port=system.membus.mem_side_ports
system.workload=SEWorkload.init_compatible(a.cmd)
env=front.host_env(args)
if a.vortex_host_rt_dir:
    env=[s for s in env if not s.startswith('LD_LIBRARY_PATH=')]
    env.append('LD_LIBRARY_PATH='+a.vortex_host_rt_dir+':'+os.environ['SS_PREFIX']+'/lib')
process=Process(pid=100,cmd=[os.path.abspath(a.cmd)]+shlex.split(a.options),executable=os.path.abspath(a.cmd),env=env)
front.build_cpus(system,args,process)
if a.npu_library:front.build_npu(system,args)
if a.vortex_library:front.build_vortex(system,args)
kernel=SystemC_Kernel(system=system)
root=Root(full_system=False,systemc_kernel=kernel)
m5.instantiate()
front.map_device_windows(process,args)
event=m5.simulate(a.max_ticks)
system.axi.finish()
print('EXIT:',event.getCause(),'code',event.getCode(),'tick',m5.curTick())
if event.getCode()!=0 or 'exiting with last active thread context' not in event.getCause():
    raise RuntimeError('XPU workload did not finish successfully')
