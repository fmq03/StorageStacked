#!/usr/bin/env python3
"""Ensure corruption cannot pass the independent online memory checker."""
import csv
import json
from pathlib import Path
import shutil
import sys
import tempfile
from check_memsim import check, rows

source = Path(sys.argv[1])
files = ['config.json','memsim_config.json','memsim_core.json','memsim_bridge_summary.json',
         'axi_flit_path.csv','aou_events.csv','memsim_bridge.csv','memsim_commands.csv',
         'memsim_dfi_signals.csv','memsim_image.csv']

def edit(path, predicate, field, value):
    data=rows(path)
    row=next(r for r in data if predicate(r))
    row[field]=value(row[field])
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=data[0].keys());w.writeheader();w.writerows(data)

cases = [
 ('unknown_completion','memsim_bridge.csv',lambda r:r['event']=='complete','mem_id',lambda _: '999999'),
 ('early_response','memsim_bridge.csv',lambda r:r['event']=='return','tick',lambda _:'0'),
 ('write_mask','memsim_bridge.csv',lambda r:r['event']=='accept' and r['command']=='W','mask',lambda s:'0'+s[1:]),
 ('dfi_read_byte','memsim_dfi_signals.csv',lambda r:r['kind']=='READ_DATA','dfi_rddata',lambda s:('00' if s[:2]!='00' else 'ff')+s[2:]),
 ('command_time','memsim_commands.csv',lambda r:r['command']=='WR','cycle',lambda s:str(int(s)+1)),
 ('final_image','memsim_image.csv',lambda r:True,'data',lambda s:('00' if s[:2]!='00' else 'ff')+s[2:]),
]
result={}
with tempfile.TemporaryDirectory(prefix='memsim-negative-') as tmp:
    dst=Path(tmp)
    for name,file,pred,field,value in cases:
        for f in files: shutil.copyfile(source/f,dst/f)
        edit(dst/file,pred,field,value)
        try: check(dst)
        except (AssertionError,KeyError,IndexError) as e: result[name]={'rejected':True,'reason':str(e)}
        else: raise AssertionError('Checker accepted corruption: '+name)
(source/'memsim_negative.json').write_text(json.dumps(result,indent=2)+'\n')
print('mem_sim corruption checks PASS:', ', '.join(result))
