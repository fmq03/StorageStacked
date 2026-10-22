#!/usr/bin/env python3
"""Prove the boundary checker rejects representative data/mask/count corruption."""
import csv
import json
import shutil
import tempfile
from pathlib import Path
import sys
from check_aou import check

source=Path(sys.argv[1]).resolve()
results={}
for fault in ('wide_wdata','wide_wstrb','endpoint_count'):
    with tempfile.TemporaryDirectory(prefix='aou-negative-') as temp:
        target=Path(temp)
        for p in source.iterdir():
            if p.suffix in ('.csv','.json'): shutil.copy2(p,target/p.name)
        (target/'axi_wave.vcd').symlink_to(source/'axi_wave.vcd')
        if fault=='endpoint_count':
            p=target/'aou_summary.json'; data=json.loads(p.read_text())
            data['memory_completed']+=1; p.write_text(json.dumps(data))
        else:
            p=target/'aou_events.csv'
            with p.open() as f:
                reader=csv.DictReader(f); fields=reader.fieldnames; rows=list(reader)
            row=next(r for r in rows if r['channel']=='W')
            key='data_hex' if fault=='wide_wdata' else 'strb_hex'
            row[key]=format(int(row[key],16)^1,'x')
            with p.open('w') as f:
                writer=csv.DictWriter(f,fieldnames=fields); writer.writeheader(); writer.writerows(rows)
        try: check(target)
        except AssertionError: results[fault]='rejected'
        else: raise AssertionError('corruption accepted: '+fault)
(source.parent/'negative_checks.json').write_text(json.dumps(results,indent=2)+'\n')
print('AoU negative checks PASS',results)
