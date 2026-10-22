#!/usr/bin/env python3
"""Reject wrong model data/compute/token/KV observations, even with paired replies."""
import copy
import csv
import json
import struct
import sys
import tempfile
from pathlib import Path
from llm_model import WEIGHTS, KEY_CACHE, TRACE, REPORT, layout, forward, encode
from verify_llm import check_observations
from result_layout import artifact

root,output=map(Path,sys.argv[1:])
c=json.loads(artifact(root,'resolved.json').read_text())
with artifact(root,'npu_requests.csv').open() as f:original=list(csv.DictReader(f))
_,_,fields,_=layout(c['llm_model'])
logit=TRACE+4*fields['logits']['offset']
results={}
for fault in ('wrong_weight','wrong_logits','wrong_generated_token','wrong_kv','wrong_rvv_count','missing_intermediate'):
    rows=copy.deepcopy(original)
    def word(r):
        mask=int(r['mask']);lane=next(i for i in range(16) if mask&(1<<i))
        return int(r['address'])+lane,lane
    address={'wrong_weight':WEIGHTS,'wrong_logits':logit,'wrong_generated_token':REPORT+64,
             'wrong_kv':KEY_CACHE,'wrong_rvv_count':REPORT+36,'missing_intermediate':TRACE}[fault]
    candidates=[r for r in rows if r['event']=='request' and r['command']=='W' and word(r)[0]==address]
    r=candidates[-1] if fault=='wrong_kv' else candidates[0]
    if fault=='missing_intermediate':rows=[v for v in rows if v['sequence']!=r['sequence']]
    else:
        lane=word(r)[1];data=bytearray.fromhex(r['data'])
        if fault=='wrong_rvv_count':encoded=(int.from_bytes(data[lane:lane+4],'little')+1).to_bytes(4,'little')
        elif fault=='wrong_generated_token':value=(int.from_bytes(data[lane:lane+4],'little')+1)%c['llm_model']['architecture']['vocab_size'];encoded=value.to_bytes(4,'little')
        else:encoded=struct.pack('<f',struct.unpack('<f',data[lane:lane+4])[0]+1.0)
        data[lane:lane+4]=encoded
        for v in rows:
            if v['sequence']==r['sequence']:v['data']=data.hex()
    try:check_observations(c,rows)
    except (AssertionError,KeyError,ValueError) as e:results[fault]={'rejected':True,'reason':str(e)}
    else:raise AssertionError('Corrupted LLM observation accepted: '+fault)
# The independent reference must not leak a future token into an earlier position.
m=c['llm_model'];prefix=encode(m,c['benchmark']['prompt'])[:m['architecture']['context_length']-1]
one=forward(m,prefix+[0]);two=forward(m,prefix+[1])
assert one[:-1]==two[:-1]
results['reference_causal_mask']={'passed':True}
# Reject misleading or unbounded deployments before compilation/simulation.
from configure import validate
from llm_model import encode
invalid={'empty_prompt':{'prompt':''}, 'unknown_character':{'prompt':'你好'},
         'context_overflow':{'prompt':'r'*16,'generated_tokens':2},
         'zero_tokens':{'generated_tokens':0},'too_many_tokens':{'generated_tokens':9},
         'non_boolean_cache':{'kv_cache':1},'unknown_field':{'batch':2}}
original_config=json.loads(artifact(root,'input.json').read_text())
for name,changes in invalid.items():
    candidate=copy.deepcopy(original_config);candidate['program'].update(changes)
    try:
        validate(candidate);encode(m,candidate['program']['prompt'])
    except (ValueError,AssertionError) as e:results['config_'+name]={'rejected':True,'reason':str(e)}
    else:raise AssertionError('Invalid configuration accepted: '+name)
candidate=copy.deepcopy(original_config);candidate['program'].update(prompt='r'*16,generated_tokens=1)
validate(candidate);encode(m,candidate['program']['prompt']);results['config_max_context_single_token']={'passed':True}
output.write_text(json.dumps({'passed':True,'checks':results},indent=2)+'\n')
print(output.read_text())
