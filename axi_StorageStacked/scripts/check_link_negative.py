#!/usr/bin/env python3
"""Ensure the passive-log checker rejects raw-frame/payload/timing corruption."""
import csv,json,shutil,sys,tempfile
from pathlib import Path
from inspect_link import inspect
source=Path(sys.argv[1]).resolve();result={}
for fault in ('physical_frame','delivered_payload','rx_timestamp'):
    with tempfile.TemporaryDirectory(prefix='link-negative-') as name:
        d=Path(name)
        for p in source.iterdir():
            if p.suffix in ('.csv','.json'):shutil.copy2(p,d/p.name)
        (d/'axi_wave.vcd').symlink_to(source/'axi_wave.vcd')
        with (d/'ucie_flits.csv').open() as f:
            reader=csv.DictReader(f);fields=reader.fieldnames;rows=list(reader)
        event={'physical_frame':'TX_FRAME','delivered_payload':'RX_FDI','rx_timestamp':'RX_FRAME'}[fault]
        r=next(r for r in rows if r['event']==event)
        if fault=='rx_timestamp':r['tick_fs']=str(int(r['tick_fs'])-1)
        else:
            raw=bytearray.fromhex(r['hex']);raw[10]^=1;r['hex']=raw.hex()
        for filename,filtered in [('ucie_flits.csv',rows),('ucie_soc.csv',[r for r in rows if r['endpoint']=='SOC']),('ucie_mem.csv',[r for r in rows if r['endpoint']=='MEM'])]:
            with (d/filename).open('w') as f:
                w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(filtered)
        try:inspect(d)
        except AssertionError:result[fault]='rejected'
        else:raise AssertionError('corruption accepted: '+fault)
(source.parent/'link_negative_checks.json').write_text(json.dumps(result,indent=2)+'\n')
print('Link negative checks PASS',result)
