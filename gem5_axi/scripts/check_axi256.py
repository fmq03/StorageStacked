#!/usr/bin/env python3
"""Prove full 32-byte beats and rejection of corruption above the old 64-bit bus."""
import csv
import json
from pathlib import Path
import shutil
import sys
import tempfile
from audit_wave import audit
from check import check
from check_aou import check as check_aou

source=Path(sys.argv[1]).resolve()
def rows(path):
    with path.open() as f:return list(csv.DictReader(f))

protocol=json.loads((source/'protocol_summary.json').read_text())
assert protocol['axi_data_bits']==256
events=rows(source/'axi_events.csv')
write=next(r for r in events if r['channel']=='AW')
assert int(write['address'])==0x90000000 and int(write['size'])==5 and int(write['len'])==1
data=[r for r in events if r['channel']=='W'][:2]
expected=bytes((11+i*37)&255 for i in range(64))
assert b''.join(int(r['data']).to_bytes(32,'little') for r in data)==expected
assert all(int(r['strb'])==0xffffffff for r in data)
assert [int(r['last']) for r in data]==[0,1]
vcd=(source/'axi_wave.vcd').read_text()
codes={}
scopes=[]
for line in vcd.splitlines():
    parts=line.split()
    if parts and parts[0]=='$scope':scopes.append(parts[2])
    if parts and parts[0]=='$upscope':scopes.pop()
    if len(parts)>4 and parts[0]=='$var' and scopes==['SystemC'] and parts[4] in ('wdata','rdata','wstrb'):
        codes[parts[4]]=parts[3]
        assert int(parts[2])==(32 if parts[4]=='wstrb' else 256)
    if '$enddefinitions' in line:break
assert set(codes)=={'wdata','rdata','wstrb'}
failures={}
for fault in ('upper_rdata_bit200','upper_wstrb_bit31','aou_upper_wdata_bit200','vcd_upper_wdata_bit200'):
    with tempfile.TemporaryDirectory(prefix='axi256-negative-') as td:
        target=Path(td)
        for name in ('axi_events.csv','transactions.csv','protocol_summary.json','config.json','aou_events.csv','aou_summary.json'):
            shutil.copy2(source/name,target/name)
        (target/'axi_wave.vcd').write_text(vcd)
        if fault.startswith('vcd_'):
            original=int(data[0]['data'])
            changed=0;lines=[]
            for line in vcd.splitlines():
                parts=line.split()
                if len(parts)==2 and parts[1]==codes['wdata'] and parts[0].startswith('b') and set(parts[0][1:])<={'0','1'} and int(parts[0][1:],2)==original:
                    line='b'+format(original^(1<<200),'b')+' '+codes['wdata'];changed+=1
                lines.append(line)
            assert changed
            (target/'axi_wave.vcd').write_text('\n'.join(lines)+'\n')
        else:
            path=target/('aou_events.csv' if fault.startswith('aou_') else 'axi_events.csv')
            rs=rows(path)
            channel='R' if fault=='upper_rdata_bit200' else 'W'
            r=next(r for r in rs if r['channel']==channel)
            key='strb' if fault=='upper_wstrb_bit31' else ('data_hex' if fault.startswith('aou_') else 'data')
            base=16 if key=='data_hex' else 10
            changed=int(r[key],base)^(1<<(31 if key=='strb' else 200))
            r[key]=format(changed,'x') if base==16 else str(changed)
            with path.open('w',newline='') as f:
                writer=csv.DictWriter(f,fieldnames=rs[0].keys());writer.writeheader();writer.writerows(rs)
        try:
            if fault.startswith('vcd_'):audit(target,target/'audit')
            elif fault.startswith('aou_'):check_aou(target)
            else:check(target)
        except AssertionError as e:failures[fault]={'rejected':True,'reason':str(e)}
        else:raise AssertionError('Corruption accepted: '+fault)
result={'passed':True,'axi_data_bits':256,'aligned_64B_write_beats':2,
        'first_write_data_hex':expected.hex(),'negative_checks':failures}
(source/'axi256_check.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
