#!/usr/bin/env python3
"""Independent transaction, source, byte, compute and waveform acceptance."""
import csv
import json
import math
from pathlib import Path
import re
import sys
from verify_compute import verify as verify_compute
from verify_host import verify as verify_host
from report import write_report
from paths import dump
from hettrace import addrmap
from hettrace.reader import CHAN_AR, CHAN_AW, CHAN_B, CHAN_R, CHAN_W, read_records, read_header
from hettrace.validate import validate_dir, format_report

def read(p):
    return json.loads(p.read_text())

def verify_soc(root,c,actual):
    a=c['architecture'];h=a['host']
    def size(text):return int(text[:-3])*(1024 if text.endswith('KiB') else 1024**2)
    for cpu in actual['cpu']:
        assert cpu['type']=='BaseTimingSimpleCPU' and cpu['mmu']['type']=={'x86_64':'X86MMU','aarch64':'ArmMMU'}[h.get('isa','x86_64')]
        assert cpu['icache']['size']==size(h['l1i']) and cpu['dcache']['size']==size(h['l1d'])
        assert cpu['mmu']['itb']['size']==h['tlb_entries'] and cpu['mmu']['dtb']['size']==h['tlb_entries']
    assert actual['l2cache']['size']==size(h['l2'])
    for level,cache_name in (('l1i','icache'),('l1d','dcache'),('l2','l2cache')):
        caches=[actual['l2cache']] if level=='l2' else [cpu[cache_name] for cpu in actual['cpu']]
        for cache in caches:
            assert cache['assoc']==h[level+'_ways'],'CPU cache associativity differs: '+level
            for field in ('tag_latency','data_latency','response_latency'):
                assert cache[field]==h[level+'_latency_cycles'],'CPU cache latency differs: '+level

    assert read(root/'ucie_config.json')==a['ucie'],'Compiled UCIe differs from JSON'
    with (root/'mmu_translations.csv').open() as stream:translations=list(csv.DictReader(stream))
    translated=[r for r in translations if r['virtual_address']!=r['physical_address']]
    assert translated,'No observed non-identity virtual-to-physical translation'
    stats={}
    for line in (root/'stats.txt').read_text().splitlines():
        fields=line.split()
        if len(fields)>=2 and any(s in fields[0] for s in ('.icache.','.dcache.','.l2cache.','.mmu.')):
            try:
                value=float(fields[1])
                if math.isfinite(value):stats[fields[0]]=value
            except ValueError:pass
    for cache in ('icache','dcache','l2cache'):
        assert any('.'+cache+'.' in key and 'accesses' in key.lower() and val>0 for key,val in stats.items()),'No cache activity: '+cache
    result={'passed':True,'mode':'gem5 syscall emulation with '+h.get('isa','x86_64')+' MMU and emulated process page tables',
        'host':h,'gpu':a['gpu'],'translation_samples':len(translations),
        'non_identity_samples':len(translated),'example':translated[0],'cache_tlb_statistics':stats}
    dump(root/'soc_summary.json',result);return result

def verify(root):
    c = read(root / 'resolved.json')
    dev = c['device']
    a = c['architecture']
    b = c['benchmark']
    completion = read(root / 'completion.json')
    assert completion['passed'] and completion['code'] == 0
    memory_config = read(root / 'memsim_config.json')
    assert memory_config['standard'] == a['memory']['standard']
    assert memory_config['channels'] == a['memory']['channels']
    assert memory_config['timing_scale'] == a['memory']['scale']
    assert memory_config['queue_depth'] == a['memory']['queue']
    actual_system = read(root / 'config.json')['systemc_kernel']['system']
    assert c['time_unit'] == '1 fs' and c['address_map']['ticks_per_second'] == 10 ** 15
    assert actual_system['mem_mode'] == 'timing'
    assert len(actual_system['cpu']) == a['host']['num_cpus']
    assert actual_system['clk_domain']['clock'] == [10 ** 9 // a['host']['clock_mhz']]
    gpu = actual_system['vortex']
    assert gpu['timing_memory'] is True and gpu['kernel'] == ''
    assert (root/gpu['library']).resolve() == (root/c['library']).resolve()
    assert gpu['clk_domain']['clock'] == [10 ** 9 // a['device_clock_mhz']]
    regions = {r['name']: (int(r['base'], 16), int(r['size'], 16))
               for r in c['address_map']['regions']}
    base, size = regions['vortex_bar']
    assert (gpu['pin_addr'], gpu['pin_size']) == (base, size)
    assert (gpu['pio_addr'], gpu['pio_size']) == regions['vortex_cp']
    assert actual_system['bridge']['addr_ranges'] == [f'{base}:{base + size}']
    host_base, host_size = regions['host_heap']
    assert actual_system['host_mem']['range'] == f'{host_base}:{host_base + host_size}'
    actual_axi = actual_system['axi']
    assert (actual_axi['base'], actual_axi['size']) == (base, size)
    assert actual_axi['period'] == a['axi']['period_ns'] * 1000000
    assert actual_axi['planes'] == a['axi']['planes']
    assert actual_axi['outstanding'] == a['axi']['outstanding']
    assert actual_axi['replay'] == a['axi']['replay']
    assert actual_axi['stalls'] == a['axi']['stalls']
    assert actual_axi['memsim_slots'] == a['memory']['slots']
    assert actual_axi['memsim_response_hold'] == a['memory']['response_hold']
    checks = {k: read(root / (k + '.json')) for k in ('check_summary', 'aou_check_summary', 'memsim_check', 'memsim_core')}
    assert all((v['passed'] for v in checks.values()))
    wave = read(root / 'wave_audit/summary.json')
    assert len(wave) == 1 and all((v['passed'] for v in wave.values()))
    expected = {dev, 'host'}
    for name in expected:
        mhz = a['device_clock_mhz'] if name == dev else a['host']['clock_mhz']
        header = read_header(str(root / 'hettrace' / (name + '.hettrace')))
        assert header.ticks_per_second == 10 ** 15
        assert header.clock_period_ticks == 10 ** 9 // mhz
        sid, level, _, _ = addrmap.SOURCES[name]
        addrmap.SOURCES[name] = (sid, level, mhz, addrmap.TICKS_PER_SECOND // (mhz * 10 ** 6))
    issues, summaries = validate_dir(str(root / 'hettrace'), require_heterogeneous=dev == 'vortex', ticks_per_second=10 ** 15)
    (root / 'hettrace/validation.txt').write_text(format_report(issues, summaries) + '\n')
    assert not [i for i in issues if i.level == 'ERROR']
    assert {s.name for s in summaries} == expected and all((s.transactions > 0 for s in summaries))
    with (root / 'transactions.csv').open() as f:
        tx = list(csv.DictReader(f))
    core_rows=[r for r in tx if int(r['stream'])>0]
    last_write={};last_read={};ordered_dependencies=0
    for r in sorted(core_rows,key=lambda r:int(r['begin_tick'])):
        block=int(r['address'])//64;begin=int(r['begin_tick']);end=int(r['end_resp_tick'])
        dependency=last_write.get(block,0)
        if r['command']=='W':dependency=max(dependency,last_read.get(block,0))
        if dependency:
            assert begin>dependency,'A conflicting GPU read/write overtook external completion'
            ordered_dependencies+=1
        target=last_write if r['command']=='W' else last_read
        target[block]=max(target.get(block,0),end)
    records = {s: list(read_records(str(root / 'hettrace' / (s + '.hettrace')))) for s in expected}
    # Both the monitor and AXI bridge split packets into legal bursts; e.g.
    # a 24-byte CP transfer becomes separate 16-byte and 8-byte bursts.
    assert sum(s.transactions for s in summaries) == sum(int(t['segments']) for t in tx), 'Trace burst count differs from AXI packet segments'
    for chan, cmd in ((CHAN_R, 'R'), (CHAN_W, 'W')):
        assert sum((r.size for rs in records.values() for r in rs if r.chan == chan)) == sum((int(t['bytes']) for t in tx if t['command'] == cmd))
    sources = {}
    for source, rs in records.items():
        starts = {r.txn: r.tick for r in rs if r.chan in (CHAN_AW, CHAN_AR)}
        ends = {}
        for r in rs:
            if r.chan in (CHAN_R, CHAN_B):
                ends[r.txn] = max(r.tick, ends.get(r.txn, 0))
        assert starts.keys() == ends.keys()
        latencies = [ends[k] - starts[k] for k in starts]
        assert min(latencies) > 0 and max((r.tick for r in rs)) <= completion['tick_fs']
        sources[source] = {'transactions': len(starts), 'bytes': sum((r.size for r in rs if r.chan in (CHAN_R, CHAN_W))), 'roundtrip_mean_ns': sum(latencies) / len(latencies) / 1000000.0}
    log = (root / 'run.log').read_text()
    devices = {}
    assert 'DEVICE_STATUS 600d0000' in log
    compute=verify_compute(root,c)
    soc=verify_soc(root,c,actual_system)
    build = re.search(r'vortex-gem5 \(XLEN=(\d+), threads=(\d+), warps=(\d+), cores=(\d+), clusters=(\d+)\)', log)
    assert build, 'No SimX library build configuration'
    xlen, threads, warps, cores, clusters = map(int, build.groups())
    assert xlen == 32 and clusters == 1
    assert {'cores': cores, 'warps': warps, 'threads': threads} == a['simx'], 'Loaded SimX differs from requested GPU configuration'
    cache = re.search(r'cache\(ICACHE=(\d+), DCACHE=(\d+), L2_ENABLED=(\d+), L2_SIZE=(\d+)\)',log)
    assert cache, 'No compiled GPU cache parameters'
    assert list(map(int,cache.groups())) == [a['gpu'][key] for key in ('l1i_bytes','l1d_bytes','l2_enabled','l2_bytes')]
    geometry=re.search(r'cache_geometry\(I_WAYS=(\d+), D_WAYS=(\d+), L2_WAYS=(\d+), I_CYCLES=(\d+), D_CYCLES=(\d+), L2_CYCLES=(\d+)\)',log)
    assert geometry,'No compiled GPU cache geometry'
    fields=[level+'_ways' for level in ('l1i','l1d','l2')]+[level+'_latency_cycles' for level in ('l1i','l1d','l2')]
    assert list(map(int,geometry.groups()))==[a['gpu'][key] for key in fields],'Loaded GPU cache geometry differs from configuration'
    m = re.search('VortexGPGPU timing summary: core_read=(\\d+) core_write=(\\d+) cp_read=(\\d+) cp_write=(\\d+) completed=(\\d+).* cp_cycles=(\\d+) vortex_cycles=(\\d+)', log)
    assert m, 'No SimX completion statistics'
    devices = dict(zip(('core_read', 'core_write', 'cp_read', 'cp_write', 'completed', 'cp_cycles', 'cycles'), map(int, m.groups())))
    assert all((devices[k] > 0 for k in ('core_read', 'core_write', 'cp_read', 'cp_write')))
    assert devices['completed'] == sum((devices[k] for k in ('core_read', 'core_write', 'cp_read', 'cp_write')))
    assert len(core_rows)==devices['core_read']+devices['core_write'],'Core source tags differ from executed requests'
    result = {'passed': True, 'device': dev, 'benchmark': b, 'axi_data_bits': read(root / 'protocol_summary.json')['axi_data_bits'], 'completion': completion, 'sources': sources, 'devices': devices, 'checks': checks, 'wave': wave, 'scope': 'accelerator -> native AXI256 -> AXI2Flit -> UCIe -> online mem_sim -> returned data'}
    assert result['axi_data_bits'] == 256
    result.update(compute);result['soc']=soc
    result['host_execution']=verify_host(root,c)
    result['gpu_memory_order']={'passed':True,'checked_dependencies':ordered_dependencies,'core_requests':len(core_rows)}
    (root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    write_report(root)
    print(json.dumps({'passed': True, 'device': dev, 'sources': sources, 'devices': devices}, indent=2))
if __name__ == '__main__':
    verify(Path(sys.argv[1]))
