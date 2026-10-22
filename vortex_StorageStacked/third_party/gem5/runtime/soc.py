"""gem5 multicore host, MMU/cache hierarchy and Vortex runtime platform."""
import os
from pathlib import Path
import m5
from m5.objects import (AddrRange, Cache, L2XBar, Process, SEWorkload, SimpleMemory,
    RedirectPath, SrcClockDomain, System, SystemXBar, TimingSimpleCPU, VoltageDomain, VortexGPGPU)

def cache_options(host, level):
    latency=host[level+'_latency_cycles']
    return dict(assoc=host[level+'_ways'],tag_latency=latency,
                data_latency=latency,response_latency=latency)

class L1(Cache):
    assoc = 8
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20


class L2(Cache):
    assoc = 16
    tag_latency = 10
    data_latency = 10
    response_latency = 10
    mshrs = 32
    tgts_per_mshr = 12
    write_buffers = 16


def connect_host(system, config, host_region):
    """Run the official benchmark and runtime, and return the device output bytes."""
    host = config['architecture']['host']
    isa=host['isa']
    libraries=config['runtime_dir']
    if isa=='aarch64':
        from m5.core import setInterpDir
        sysroot=config['host_sysroot']
        setInterpDir(sysroot)
        libraries+=':'+sysroot+'/lib'
        system.redirect_paths=[RedirectPath(app_path=path,host_paths=[sysroot+'/lib'])
                               for path in ('/lib/aarch64-linux-gnu','/usr/lib/aarch64-linux-gnu')]
    else:libraries+=':'+os.environ['SS_PREFIX']+'/lib'
    base, size = host_region
    host_range = AddrRange(base, size=size)
    system.mem_ranges = [host_range]
    # Only host code/stack/heap use local memory; the GPU BAR has its own path.
    system.host_mem = SimpleMemory(range=host_range, latency='10ns', conf_table_reported=True)
    system.host_mem.port = system.membus.mem_side_ports
    system.workload = SEWorkload.init_compatible(config['host_binary'])
    command = [config['host_binary'], config['kernel']]
    process = Process(
        pid=100, executable=config['host_binary'],
        cmd=command,
        output='host_stdout.log', errout='host_stderr.log',
        env=['VORTEX_DRIVER=gem5-'+isa,'LD_LIBRARY_PATH='+libraries],
    )
    system.cpu = [TimingSimpleCPU(cpu_id=i) for i in range(host['num_cpus'])]
    system.multi_thread = True
    system.l2bus = L2XBar()
    for cpu in system.cpu:
        cpu.mmu.itb.size = host['tlb_entries']
        cpu.mmu.dtb.size = host['tlb_entries']
        cpu.icache = L1(size=host['l1i'], **cache_options(host,'l1i'), is_read_only=True, writeback_clean=True)
        cpu.dcache = L1(size=host['l1d'], **cache_options(host,'l1d'))
        cpu.icache.cpu_side = cpu.icache_port
        cpu.dcache.cpu_side = cpu.dcache_port
        cpu.icache.mem_side = system.l2bus.cpu_side_ports
        cpu.dcache.mem_side = system.l2bus.cpu_side_ports
        cpu.createInterruptController()
        if isa=='x86_64':
            cpu.interrupts[0].pio = system.membus.mem_side_ports
            cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
            cpu.interrupts[0].int_responder = system.membus.mem_side_ports
        else:cpu.mmu.connectWalkerPorts(system.l2bus.cpu_side_ports,system.l2bus.cpu_side_ports)
        cpu.workload = process
        cpu.createThreads()
    system.l2cache = L2(size=host['l2'], **cache_options(host,'l2'))
    system.l2cache.cpu_side = system.l2bus.mem_side_ports
    system.l2cache.mem_side = system.membus.cpu_side_ports
    return process


def connect_gpu(system, config, cp, bar):
    """gem5 ticks SimX; core and CP DMA complete only after timing responses."""
    system.vortex = VortexGPGPU(
        library=config['library'], kernel='',
        pio_addr=cp[0], pio_size=cp[1], pin_addr=bar[0], pin_size=bar[1],
        timing_memory=True, trace_enable=False, trace_addr_offset=bar[0],
        clk_domain=SrcClockDomain(
            clock=f"{config['architecture']['device_clock_mhz']}MHz",
            voltage_domain=system.clk_domain.voltage_domain,
        ),
    )
    system.vortex.pio = system.membus.mem_side_ports
    system.vortex.dma = system.membus.cpu_side_ports



def create(config, output):
    m5.ticks.setGlobalFrequency('1fs')
    (output/'hettrace').mkdir(exist_ok=True)
    os.environ.update(HETTRACE_DIR=str(output/'hettrace'), HETTRACE_FILTER='all',
                      HETTRACE_FORMAT='binary', SOC_MMU_TRACE=str(output/'mmu_translations.csv'))
    config = dict(config)
    for key in ('host_binary','kernel','library','runtime_dir'):
        config[key] = str((output/config[key]).resolve())
    if 'host_sysroot' in config:config['host_sysroot']=str((output/config['host_sysroot']).resolve())
    regions={r['name']:(int(r['base'],16),int(r['size'],16)) for r in config['address_map']['regions']}
    system=System()
    system.clk_domain=SrcClockDomain(clock=f"{config['architecture']['host']['clock_mhz']}MHz",voltage_domain=VoltageDomain())
    system.mem_mode='timing'
    system.membus=SystemXBar()
    system.system_port=system.membus.cpu_side_ports
    process=connect_host(system,config,regions['host_heap'])
    connect_gpu(system,config,regions['vortex_cp'],regions['vortex_bar'])
    return system,process,regions
