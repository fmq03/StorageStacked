#!/usr/bin/env python3
"""Acceptance gates for the online CPU/mem_sim integration suite."""
import csv
import json
from pathlib import Path
import re
import sys
from native_tests import passed_count
from hettrace.reader import CHAN_R, CHAN_W, read_header, read_records
from hettrace.validate import format_report, validate_dir

root=Path(sys.argv[1])
def read(p): return json.loads(p.read_text())
def rows(p):
    with p.open() as f:return list(csv.DictReader(f))
def finish(p):return int(re.findall(r'EXIT: .* tick (\d+)',(p/'run.log').read_text())[-1])
def insts(p):return int(re.search(r'^simInsts\s+(\d+)',(p/'stats.txt').read_text(),re.M)[1])
native=(root/'native-tests.log').read_text()
native_count=passed_count(root)
api=read(root/'api/api_check.json');assert api['passed'] and api['queue_retry']
cases={}
for name in ('directed','replay','shallow','held','period_3ns','cpu','cpu_slow'):
    p=root/name
    checks={key:read(p/(key+'.json')) for key in ('check_summary','aou_check_summary','memsim_check','memsim_core')}
    assert all(c['passed'] for c in checks.values())
    issues,summaries=validate_dir(str(p/'hettrace'),require_heterogeneous=False,ticks_per_second=10**15)
    (p/'hettrace/validation.txt').write_text(format_report(issues,summaries)+'\n')
    assert not [i for i in issues if i.level=='ERROR']
    header=read_header(str(p/'hettrace/host.hettrace'));assert header.ticks_per_second==10**15
    records=list(read_records(str(p/'hettrace/host.hettrace')));tx=rows(p/'transactions.csv')
    for ch,cmd in ((CHAN_R,'R'),(CHAN_W,'W')):
        assert sum(r.size for r in records if r.chan==ch)==sum(int(t['bytes']) for t in tx if t['command']==cmd)
    assert max(r.tick for r in records)<=finish(p)
    if name.startswith('cpu'):
        assert 'CPU MEMSIM PASS checksum=24416 boundary64=ok' in (p/'run.log').read_text()
        assert len(tx)==452 and insts(p)>0
    cases[name]={'exit_tick_fs':finish(p),'transactions':len(tx),'het_records':len(records),**checks}
assert cases['shallow']['memsim_check']['submit_stalls']>0
assert cases['held']['memsim_check']['response_stalls']>0
assert cases['replay']['aou_check_summary']['crc_errors']>0
fast,slow=root/'cpu',root/'cpu_slow'
a,b=rows(fast/'transactions.csv'),rows(slow/'transactions.csv')
assert insts(fast)==insts(slow)
assert read(slow/'memsim_config.json')['period_fs']==4*read(fast/'memsim_config.json')['period_fs']
deltas=[]
for x,y in zip(a,b):
    assert all(x[k]==y[k] for k in ('command','address','bytes','status'))
    deltas.append((int(y['end_resp_tick'])-int(y['begin_tick']))-(int(x['end_resp_tick'])-int(x['begin_tick'])))
finish_delta=finish(slow)-finish(fast)
assert finish_delta>0 and sum(deltas)==finish_delta, (finish_delta,sum(deltas))
assert sum(d>0 for d in deltas)>len(deltas)//2
negative=read(root/'directed/memsim_negative.json');assert len(negative)==6 and all(v['rejected'] for v in negative.values())
wave=read(root/'wave_audit/summary.json');assert len(wave)==7 and all(v['passed'] for v in wave.values())
result={'passed':True,'scope':'X86 CPU -> gem5 native TLM/SystemC -> AXI -> AXI2Flit -> UCIe -> online mem_sim HBM4 behavioral PHY',
        'axi_data_bits':read(root/'directed/protocol_summary.json').get('axi_data_bits',64),
        'native_tests_passed':native_count,'api_check':api,'cases':cases,'negative_checks':negative,'cpu_feedback':{'passed':True,'simulated_instructions':insts(fast),
        'transactions':len(a),'finish_delta_ns':finish_delta/1e6,'transaction_delta_sum_ns':sum(deltas)/1e6,
        'slower_transactions':sum(d>0 for d in deltas),'transaction_delta_min_ns':min(deltas)/1e6,'transaction_delta_max_ns':max(deltas)/1e6},
        'limitations':['CPU target window is uncached; not cache coherence verification','HBM4 provisional preset, not calibrated device timing','This suite covers CPU/tester; accelerators are covered by run_xpu.sh']}
(root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'passed':True,'cases':list(cases),'cpu_feedback':result['cpu_feedback']},indent=2))
