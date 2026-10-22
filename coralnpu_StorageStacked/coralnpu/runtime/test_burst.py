#!/usr/bin/env python3
"""Exercise real RTL requests and online memory for every supported burst geometry."""
import argparse
import copy
import csv
import datetime
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path
from paths import ROOT, RUNTIME, CACHE, relative, dump
from configure import validate, resolved_parts
from result_layout import artifact

stamp='burst-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path)
options=parser.parse_args()
out=options.output if options.output is not None else CACHE/'validation'/stamp
if not out.is_absolute():out=ROOT/out
out.mkdir(parents=True)
base=ROOT/'user/burst'
original=json.loads((base/'config.json').read_text())
invalid_configs={}
for invalid in (0,3,17,True,1.5):
    candidate=copy.deepcopy(original);candidate['axi']['burst_beats']=invalid
    try:validate(candidate)
    except ValueError as error:invalid_configs[repr(invalid)]={'rejected':True,'reason':str(error)}
    else:raise AssertionError('Invalid burst configuration accepted')
legacy=copy.deepcopy(original);del legacy['axi']['burst_beats']
assert resolved_parts(validate(legacy))[0]['axi']['burst_beats']==4
results={}
for beats in (1,2,4,8,16):
    config=copy.deepcopy(original)
    config['axi']['burst_beats']=beats
    # Include capacity backpressure and link replay with the longest burst.
    if beats==16:
        config['axi']['outstanding']=1
        config['axi']['replay']=True
    config_path=base/'result'/stamp/'configs'/f'beats-{beats}.json'
    config_path.parent.mkdir(parents=True,exist_ok=True)
    dump(config_path,config)
    directory=base/'result'/stamp/f'beats-{beats}'
    print('Running burst '+str(beats)+' beats',flush=True)
    with (out/f'beats-{beats}.log').open('w') as log:
        subprocess.run(['bash','user/run.sh','burst','--config',relative(config_path,base),
                        '--output',relative(directory,base)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    summary=json.loads((directory/'summary.json').read_text())
    assert summary['passed'] and summary['axi_burst']['beats_per_request']==beats
    with artifact(directory,'axi_events.csv').open() as f:
        events=list(csv.DictReader(f))
    addresses=[r for r in events if r['channel'] in ('AW','AR')]
    assert all(int(r['len'])==beats-1 and (int(r['len'])+1)*(1<<int(r['size']))==16 for r in addresses)
    assert any(int(r['address'])&4095==4080 for r in addresses)
    assert any(int(r['address'])&4095==0 for r in addresses)
    assert {int(r['address'])%32 for r in addresses}=={0,16}
    stalls=next(iter(summary['wave'].values()))['stalled_edges']
    assert stalls['W']>0 and stalls['R']>0
    results[str(beats)]={'passed':True,'axi_burst':summary['axi_burst'],
                        'stalled_edges':stalls,'report':relative(directory/'summary.json')}
    if beats==4:
        spec=importlib.util.spec_from_file_location('burst_checker',RUNTIME/'checks/check.py')
        checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)
        negatives={}
        for fault in ('early_wlast','missing_wlast','early_rlast','mid_read_data','wrong_burst_len','missing_write_beat'):
            with tempfile.TemporaryDirectory(prefix='burst-negative-') as temporary:
                target=Path(temporary)
                for name in ('transactions.csv','protocol_summary.json','config.json'):
                    (target/name).write_bytes(artifact(directory,name).read_bytes())
                rows=copy.deepcopy(events)
                if fault in ('early_wlast','missing_wlast'):
                    row=next(r for r in rows if r['channel']=='W' and int(r['last'])==(fault=='missing_wlast'))
                    row['last']=str(1-int(row['last']))
                elif fault=='early_rlast':
                    next(r for r in rows if r['channel']=='R' and r['last']=='0')['last']='1'
                elif fault=='mid_read_data':
                    reads=[r for r in rows if r['channel']=='R']
                    row=reads[1];address=int(next(r for r in rows if r['channel']=='AR')['address'])+4
                    row['data']=str(int(row['data'])^(1<<(8*(address%32))))
                elif fault=='wrong_burst_len':next(r for r in rows if r['channel']=='AW')['len']='2'
                else:rows.pop(next(i for i,r in enumerate(rows) if r['channel']=='W'))
                with (target/'axi_events.csv').open('w',newline='') as f:
                    writer=csv.DictWriter(f,fieldnames=events[0].keys());writer.writeheader();writer.writerows(rows)
                try:checker.check(target)
                except (AssertionError,KeyError,ValueError) as error:
                    negatives[fault]={'rejected':True,'reason':str(error)}
                else:raise AssertionError('Corrupted burst accepted: '+fault)
        results[str(beats)]['negative_checks']=negatives
dump(out/'summary.json',{'passed':True,'cases':results,'invalid_configs':invalid_configs,
                        'legacy_default_beats':4,'scope':'Real CoralNPU RTL -> AXI INCR bursts -> UCIe -> online mem_sim; byte masks, both lanes, 4KiB boundaries, backpressure and replay'})
print('PASS: '+relative(out/'summary.json'),flush=True)
