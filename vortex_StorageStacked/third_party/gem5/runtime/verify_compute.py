"""Check returned device bytes against configuration and an independent decoder."""
from collections import Counter,defaultdict
import csv,json,math,re,struct
from llm_model import WEIGHTS,KEY_CACHE,VALUE_CACHE,TRACE,TOKENS,REPORT,layout,encode,reference
from paths import dump


def readback(root):
    memory={}
    for address,data in re.findall(r'^READBACK ([0-9a-f]+) ([0-9a-f]+)$',(root/'run.log').read_text(),re.M):
        base=int(address,16)
        for i,b in enumerate(bytes.fromhex(data)):
            if base+i in memory:assert memory[base+i]==b,'Conflicting returned output bytes'
            memory[base+i]=b
    ranges=json.loads((root/'build/readback.json').read_text())
    required={ranges['base']+r['offset']+i for r in ranges['ranges'] for i in range(r['bytes'])}
    assert required==memory.keys(),'Missing or unexpected host readback bytes'
    # The host must return exactly the bytes independently reconstructed from
    # online memory, whose AXI/Flit/read/DFI path is checked by check_memsim.py.
    final={}
    with (root/'memsim_image.csv').open() as stream:
        for r in csv.DictReader(stream):
            base=int(r['address'],0)
            for i,b in enumerate(bytes.fromhex(r['data'])):
                if base+i in required:final[base+i]=b
    assert memory==final,'Host readback differs from final online memory'
    return memory


def check_outputs(c,memory):
    def raw(address):return bytes(memory[address+i] for i in range(4))
    def word(address):return int.from_bytes(raw(address),'little')
    assert word(REPORT+12)==0x600d0000,'Device completion status'
    b=c['benchmark']
    if b['name']=='smoke':
        expected=(b['input']+1)&0xffffffff
        for i in range(b['workers']):
            assert word(WEIGHTS+4*i)==b['input'],'SMOKE input readback mismatch'
            assert word(KEY_CACHE+4*i)==expected,'SMOKE result mismatch'
        return {'smoke':{'passed':True,'input':b['input'],'output':expected,'expected_output':expected,'workers':b['workers']}}
    if b['name']=='custom':
        for e in b['expect']:assert word(int(e['address'],0))==e['value'],'Custom output mismatch'
        return {'custom':{'passed':True,'checked_words':len(b['expect'])}}
    model=c['llm_model'];ref=reference(model,b);flat,offsets,traces,stride=layout(model)
    dim=model['architecture']['dim'];ctx=model['architecture']['context_length']
    max_error=defaultdict(float);counts=Counter()
    def floating(address,target,label):
        actual=struct.unpack('<f',raw(address))[0]
        assert math.isfinite(actual) and abs(actual-target)<=2e-5+2e-5*abs(target),f'{label} mismatch at {address:#x}: {actual} != {target}'
        max_error[label]=max(max_error[label],abs(actual-target));counts[label]+=1
    for i,value in enumerate(flat):assert raw(WEIGHTS+4*i)==struct.pack('<f',value),f'Weight mismatch at word {i}'
    for base,field in ((KEY_CACHE,'k'),(VALUE_CACHE,'v')):
        for pos in range(ctx):
            for j in range(dim):floating(base+4*(pos*dim+j),ref['records'][pos][field][j] if pos<len(ref['records']) else 0.,'kv_'+field)
    for pos,record in enumerate(ref['records']):
        for name,info in traces.items():
            for j,value in enumerate(record[name]):floating(TRACE+4*(pos*stride+info['offset']+j),value,name)
    for i,token in enumerate(ref['tokens']):assert word(TOKENS+4*i)==token,'Input/generated token mismatch'
    prompt=encode(model,b['prompt']);calls=0
    reports={0:0x4c4c4d31,1:1,2:2,3:0x600d0000,8:0}
    for step,token in enumerate(ref['generated']):
        calls+=1 if b['kv_cache'] and step else len(prompt)+step
        reports.update({16+step:token,32+step:0x70000000+step,64+step:0x71000000+step,96+step:calls})
    gpu=c['architecture']['simx'];threads=min(dim,gpu['threads']*gpu['warps'])
    columns=[dim]*4+[model['architecture']['hidden_dim'],dim,model['architecture']['vocab_size']]
    per_thread=[calls*sum(len(range(t,n,threads)) for n in columns) for t in range(threads)]
    reports.update({9:7*calls,10:threads,11:threads,12:(threads+gpu['threads']-1)//gpu['threads']})
    reports.update({128+t:count for t,count in enumerate(per_thread)})
    assert word(REPORT+52)<gpu['cores'],'GPU execution core outside configured cores'
    for i,value in reports.items():assert word(REPORT+4*i)==value,f'Report mismatch at word {i}'
    return {'llm':{'passed':True,'model_version':model['version'],'model_parameters':sum(len(t['data']) for t in model['tensors'].values()),
        'architecture':model['architecture'],'prompt':b['prompt'],'prompt_tokens':prompt,'generated_token_ids':ref['generated'],
        'generated_text':ref['text'],'kv_cache':b['kv_cache'],'processed_token_positions':len(ref['records']),
        'forward_calls':calls,'verified_float_values':sum(counts.values()),'checked_values_by_stage':dict(counts),
        'execution':{'mode':'SIMT FP32 linear layers','workgroups':1,'threads':threads,'active_threads':threads,
                     'warps':reports[12],'core':word(REPORT+52),'linear_calls':7*calls,'columns_per_thread':per_thread},
        'maximum_absolute_error_by_stage':dict(max_error),'numeric_tolerance':{'absolute':2e-5,'relative':2e-5},
        'scope':'Actual GPU output and online memory; independent full-prefix FP64 reference'}}


def verify(root,c):
    result=check_outputs(c,readback(root))
    for name,value in result.items():dump(root/(name+'_summary.json'),value)
    return result
