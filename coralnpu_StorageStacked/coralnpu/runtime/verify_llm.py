"""Verify observed NPU writes against an independent full-prefix decoder reference."""
import collections
import csv
import json
import math
import struct
from llm_model import WEIGHTS, KEY_CACHE, VALUE_CACHE, TRACE, TOKENS, REPORT, layout, encode, reference

ABS_TOL, REL_TOL = 0.00002, 0.00002

def check_observations(c, source):
    b,m=c['benchmark'],c['llm_model']
    ref=reference(m,b)
    flat,offsets,traces,stride=layout(m)
    d,ctx=m['architecture']['dim'],m['architecture']['context_length']
    prompt=encode(m,b['prompt']);g=b['generated_tokens'];p=len(prompt)
    visits=collections.Counter()
    expected_calls=[];calls=0
    for step in range(g):
        positions=range(p+step-1,p+step) if b['kv_cache'] and step else range(p+step)
        for pos in positions:visits[pos]+=1;calls+=1
        expected_calls.append(calls)
    expected={};kind={}
    def add(address,values,label):
        assert address not in expected
        expected[address]=list(values);kind[address]=label
    for i,value in enumerate(flat):add(WEIGHTS+4*i,[value],'weight')
    for base,field in ((KEY_CACHE,'k'),(VALUE_CACHE,'v')):
        for pos in range(ctx):
            for j in range(d):
                add(base+4*(pos*d+j),[0.0]+([ref['records'][pos][field][j]]*visits[pos] if pos in visits else []),'kv_'+field)
    for pos,r in enumerate(ref['records']):
        for name,info in traces.items():
            for j,value in enumerate(r[name]):add(TRACE+4*(pos*stride+info['offset']+j),[value]*visits[pos],name)
    for i,token in enumerate(ref['tokens']):add(TOKENS+4*i,[token],'token_input')
    reports={0:0x4c4c4d31,1:1,2:2,8:0,9:7*calls}
    for step,token in enumerate(ref['generated']):
        reports.update({16+step:token,32+step:0x70000000+step,64+step:0x71000000+step,96+step:expected_calls[step]})
    for i,value in reports.items():add(REPORT+4*i,[value],'report')
    response={r['sequence']:r for r in source if r['event']=='response'}
    observed=collections.Counter();max_error=collections.defaultdict(float);marker_ticks={};traffic=collections.defaultdict(collections.Counter)
    tensor_reads=collections.Counter();raw_writes=[]
    for r in source:
        if r['event']!='request':continue
        address=int(r['address']);is_write=r['command']=='W'
        region=('weights' if WEIGHTS<=address<KEY_CACHE else 'keys' if KEY_CACHE<=address<VALUE_CACHE
                else 'values' if VALUE_CACHE<=address<TRACE else 'intermediates' if TRACE<=address<TOKENS
                else 'tokens' if TOKENS<=address<REPORT else 'report' if REPORT<=address<REPORT+4096 else None)
        assert region is not None, 'LLM request outside declared regions'
        traffic[region]['writes' if is_write else 'reads']+=1
        if not is_write:
            if region=='weights':
                for name,off in offsets.items():
                    if WEIGHTS+4*off<=address<WEIGHTS+4*(off+len(m['tensors'][name]['data'])):tensor_reads[name]+=1
            continue
        mask=int(r['mask']);enabled=[i for i in range(16) if mask&(1<<i)]
        assert len(enabled)==4 and enabled==list(range(enabled[0],enabled[0]+4)) and enabled[0]%4==0
        word=address+enabled[0];raw=bytes.fromhex(r['data'])[enabled[0]:enabled[0]+4]
        assert word in expected, 'Unexpected LLM write at %#x'%word
        index=observed[word];assert index<len(expected[word]), 'Extra LLM write at %#x'%word
        target=expected[word][index];label=kind[word]
        if label in ('report','token_input'):
            got=int.from_bytes(raw,'little');assert got==target, f'{label} mismatch at {word:#x}: {got} != {target}'
        elif label=='weight':
            assert raw==struct.pack('<f',target), f'Weight staging mismatch at {word:#x}'
        else:
            got=struct.unpack('<f',raw)[0]
            assert math.isfinite(got) and abs(got-target)<=ABS_TOL+REL_TOL*abs(target), f'{label} mismatch at {word:#x}: {got} != {target}'
            max_error[label]=max(max_error[label],abs(got-target))
        observed[word]+=1
        rsp=response[r['sequence']]
        assert rsp['command']=='W' and rsp['data']==r['data'] and int(rsp['tick'])>int(r['tick'])
        if label=='report':marker_ticks[(word-REPORT)//4]=int(rsp['tick'])
        raw_writes.append((word,raw))
    assert all(observed[a]==len(v) for a,v in expected.items()), 'Missing weight/KV/intermediate/token/report writes'
    assert set(tensor_reads)==set(m['tensors']), 'An inference weight tensor was not read through external memory'
    assert all(traffic[r]['reads']>0 for r in ('weights','keys','values','tokens','report'))
    starts=[marker_ticks[32+i] for i in range(g)];ends=[marker_ticks[64+i] for i in range(g)]
    ready=[marker_ticks[16+i] for i in range(g)]
    assert marker_ticks[0]<marker_ticks[1]<starts[0]
    for i in range(g):
        assert starts[i]<ready[i]<ends[i]
        if i:assert ends[i-1]<starts[i]
    assert ends[-1]<marker_ticks[2]
    return {'passed':True,'model_version':m['version'],'model_parameters':sum(len(t['data']) for t in m['tensors'].values()),
            'architecture':m['architecture'],'prompt':b['prompt'],'prompt_tokens':prompt,
            'generated_token_ids':ref['generated'],'generated_text':ref['text'],'kv_cache':b['kv_cache'],
            'execution':{'core':'RvvCoreMiniAxi','linear':'RVV FP32','rvv_linear_calls':7*calls},
            'processed_token_positions':p+g-1,'forward_calls':calls,'verified_float_writes':sum(observed[a] for a in expected if kind[a] not in ('weight','report','token_input')),
            'numeric_tolerance':{'absolute':ABS_TOL,'relative':REL_TOL},'maximum_absolute_error_by_stage':dict(max_error),
            'traffic':{k:dict(v) for k,v in traffic.items()},'weight_tensor_reads':dict(tensor_reads),
            'timing':{'unit':'ns','initialization_ns':(marker_ticks[1]-marker_ticks[0])/1e6,
                      'prefill_to_first_token_ns':(ready[0]-starts[0])/1e6,
                      'decode_inter_token_ns':[(ready[i]-ready[i-1])/1e6 for i in range(1,g)],
                      'decode_tokens_per_simulated_second':(g-1)*1e15/(ready[-1]-ready[0]) if g>1 else None,
                      'inference_to_last_token_ns':(ready[-1]-starts[0])/1e6,
                      'phase_duration_ns':[(ends[i]-starts[i])/1e6 for i in range(g)],
                      'scope':'RTL simulation including observation/marker traffic; excludes model staging and startup from prefill/decode'},
            'scope':'Trained miniature character decoder functional benchmark; RVV FP32 linear layers with scalar normalization/attention, one layer, no general-language quality claim'}

def verify_llm(root,c):
    with (root/'npu_requests.csv').open() as stream:source=list(csv.DictReader(stream))
    result=check_observations(c,source)
    (root/'llm_summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
