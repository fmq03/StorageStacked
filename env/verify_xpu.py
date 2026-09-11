#!/usr/bin/env python3
"""Acceptance for CPU-hosted accelerators and memory timing feedback."""
import csv
import json
from pathlib import Path
import re
import sys
from native_tests import passed_count
from hettrace.reader import CHAN_AR,CHAN_AW,CHAN_B,CHAN_R,CHAN_W,read_records
from hettrace.validate import validate_dir,format_report

root=Path(sys.argv[1])
def read(path):return json.loads(path.read_text())
def rows(path):
    with path.open() as f:return list(csv.DictReader(f))
def finish(p):return int(re.findall(r'EXIT: .* code 0 tick (\d+)',(p/'run.log').read_text())[-1])

cases={};npu_sequences={}
for name,sources in [('npu',{'host','coralnpu'}),('gpu',{'host','vortex'}),
                     ('three',{'host','vortex','coralnpu'}),('three_slow',{'host','vortex','coralnpu'})]:
    p=root/name;log=(p/'run.log').read_text()
    if name=='gpu':assert 'PASSED!' in log
    else:assert '全部通过' in log
    if name.startswith('three'):assert 'NPU ok (tag=0x600d sum=0x17e0)' in log and 'Vortex ok' in log
    checks={key:read(p/(key+'.json')) for key in ('check_summary','aou_check_summary','memsim_check','memsim_core')}
    assert all(c['passed'] for c in checks.values())
    issues,summaries=validate_dir(str(p/'hettrace'),ticks_per_second=10**15)
    (p/'hettrace/validation.txt').write_text(format_report(issues,summaries)+'\n')
    assert not [i for i in issues if i.level=='ERROR']
    assert {s.name for s in summaries}==sources and all(s.transactions>0 for s in summaries)
    tx=rows(p/'transactions.csv');records={s:list(read_records(str(p/'hettrace'/f'{s}.hettrace'))) for s in sources}
    assert sum(s.transactions for s in summaries)==len(tx)
    for ch,cmd in ((CHAN_R,'R'),(CHAN_W,'W')):
        assert sum(r.size for rs in records.values() for r in rs if r.chan==ch)==sum(int(t['bytes']) for t in tx if t['command']==cmd)
    observed={}
    for source,rs in records.items():
        starts={r.txn:r.tick for r in rs if r.chan in (CHAN_AW,CHAN_AR)}
        ends={}
        for r in rs:
            if r.chan in (CHAN_R,CHAN_B):ends[r.txn]=max(r.tick,ends.get(r.txn,0))
        assert starts.keys()==ends.keys()
        latencies=[ends[k]-starts[k] for k in starts]
        assert min(latencies)>0 and max(r.tick for r in rs)<=finish(p)
        observed[source]={'transactions':len(starts),'bytes':sum(r.size for r in rs if r.chan in (CHAN_R,CHAN_W)),
            'first_tick_fs':min(r.tick for r in rs),'last_tick_fs':max(r.tick for r in rs),
            'roundtrip_sum_ns':sum(latencies)/1e6,'roundtrip_mean_ns':sum(latencies)/len(latencies)/1e6}
    device={}
    if 'coralnpu' in sources:
        m=re.search(r'kernel finished after (\d+) cycles',log);assert m
        device['npu_cycles']=int(m[1])
        assert re.search(r'AXI timing summary reads=64 .* writes=64 ',log)
        assert observed['coralnpu']['transactions']==128
        npu_sequences[name]=[(r.chan,r.addr,r.size,r.strb) for r in records['coralnpu'] if r.chan in (CHAN_AW,CHAN_AR)]
    if 'vortex' in sources:
        m=re.search(r'VortexGPGPU timing summary: core_read=(\d+) core_write=(\d+) cp_read=(\d+) cp_write=(\d+) completed=(\d+).* cp_cycles=(\d+) vortex_cycles=(\d+)',log)
        assert m
        device.update(zip(('gpu_core_read','gpu_core_write','gpu_cp_read','gpu_cp_write','gpu_completed','gpu_cp_cycles','gpu_cycles'),map(int,m.groups())))
        assert device['gpu_core_read']>0 and device['gpu_core_write']>0 and device['gpu_cp_read']>0 and device['gpu_cp_write']>0
        assert device['gpu_completed']==sum(device[k] for k in ('gpu_core_read','gpu_core_write','gpu_cp_read','gpu_cp_write'))
    cases[name]={'exit_tick_fs':finish(p),'sources':observed,'devices':device,**checks}

fast,slow=cases['three'],cases['three_slow']
assert read(root/'three_slow/memsim_config.json')['period_fs']==4*read(root/'three/memsim_config.json')['period_fs']
assert npu_sequences['three']==npu_sequences['three_slow']
assert slow['exit_tick_fs']>fast['exit_tick_fs']
assert slow['devices']['npu_cycles']>fast['devices']['npu_cycles']
assert all(slow['devices'][k]==fast['devices'][k] for k in ('gpu_core_read','gpu_core_write'))
assert slow['devices']['gpu_cycles']>fast['devices']['gpu_cycles']
for s in ('vortex','coralnpu'):
    assert slow['sources'][s]['roundtrip_mean_ns']>fast['sources'][s]['roundtrip_mean_ns']
feedback={'passed':True,'scale':4,'host_finish_delta_ns':(slow['exit_tick_fs']-fast['exit_tick_fs'])/1e6,
    'npu_cycles':[fast['devices']['npu_cycles'],slow['devices']['npu_cycles']],
    'gpu_cycles':[fast['devices']['gpu_cycles'],slow['devices']['gpu_cycles']],
    'note':'CPU/CP polling counts may change; compare the same computation and actual source responses.'}
wave=read(root/'wave_audit/summary.json');assert len(wave)==4 and all(v['passed'] for v in wave.values())
api=read(root/'api/api_check.json');assert api['passed'] and api['high_address_no_alias'] and api['sparse_zero_initialized']
native_count=passed_count(root)
assert read(root/'environment/xpu_manifest.json')['passed']
result={'passed':True,'scope':'CPU-hosted Vortex SimX and CoralNPU RTL -> AXI/UCIe -> online mem_sim -> original response path',
    'axi_data_bits':read(root/'three/protocol_summary.json').get('axi_data_bits',64),
    'cases':cases,'memory_feedback':feedback,'api_check':api,'native_tests_passed':native_count,
    'limitations':['Host program/stack use gem5 local memory; target buffers use the online link',
      'No general AoU functional/atomic access, checkpoints or cache-coherence support',
      'NPU local ELF/reset initialization precedes timed execution; one launch per simulation',
      'HBM4 provisional behavioral PHY; GPU SimX is not full GPU RTL',
      'CPU orchestrates separate GPU and NPU buffers; no direct GPU-NPU shared-buffer workload']}
(root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'passed':True,'cases':list(cases),'memory_feedback':feedback},indent=2))
