"""Negative acceptance: reject invalid settings and corrupted device outputs."""
import copy,json,struct
from pathlib import Path
from configure import validate
from verify_compute import readback,check_outputs
from llm_model import WEIGHTS,KEY_CACHE,TRACE,REPORT
from verify_host import check_execution,verify as verify_host


def check(smoke=None,llm=None):
    result={}
    def reject(name,fn):
        try:fn()
        except (AssertionError,ValueError,KeyError,TypeError):result[name]=True
        else:raise AssertionError('Did not reject '+name)
    root=smoke or llm;config=json.loads((root/'input.json').read_text())
    resolved=json.loads((root/'resolved.json').read_text())
    evidence=json.loads((root/'host_summary.json').read_text())
    for name in ('idle_cpu','missing_worker','duplicate_thread','work_partition'):
        bad=copy.deepcopy(evidence)
        if name=='idle_cpu':bad['cores'][-1]['instructions']=0
        elif name=='missing_worker':bad['phases']['prepare'].pop()
        elif name=='duplicate_thread':bad['phases']['prepare'][1]['tid']=bad['phases']['prepare'][0]['tid']
        else:bad['phases']['check'][0]['end']+=1
        reject(name,lambda bad=bad:check_execution(bad,resolved))
    wrong_isa=copy.deepcopy(resolved)
    current=wrong_isa['architecture']['host'].get('isa','x86_64')
    wrong_isa['architecture']['host']['isa']='aarch64' if current=='x86_64' else 'x86_64'
    reject('host_elf_isa_mismatch',lambda:verify_host(root,wrong_isa))
    for value in ('armhf','riscv64','arm','',None,True,123,[]):
        bad=copy.deepcopy(config);bad['host']['isa']=value
        reject('host.isa.'+repr(value),lambda bad=bad:validate(bad))
    for group,key,value in [('gpu','cores',3),('gpu','l1i_bytes',5000),('host','num_cpus',0),('host','l2','3KiB'),('host','tlb_entries',0),('axi','outstanding',0),('axi','period_ns',True),('axi','replay','yes'),('ucie','lanes',0),('ucie','bits_per_symbol',3),('ucie','rate_gtps',0),('memsim','standard','unknown'),('memsim','slots',1024),('simulation','max_ticks',0)]:
        c=copy.deepcopy(config);c[group][key]=value;reject(group+'.'+key,lambda c=c:validate(c))
    if smoke:
        c=json.loads((smoke/'resolved.json').read_text());memory=readback(smoke)
        for name,address in [('smoke_output',KEY_CACHE),('smoke_status',REPORT+12)]:
            bad=memory.copy();bad[address]^=1;reject(name,lambda bad=bad:check_outputs(c,bad))
    if llm:
        c=json.loads((llm/'resolved.json').read_text());memory=readback(llm)
        for name,address in [('weight',WEIGHTS),('kv',KEY_CACHE),('intermediate',TRACE),('generated_token',REPORT+64),('forward_count',REPORT+384),
                             ('linear_count',REPORT+36),('active_gpu_threads',REPORT+44),('thread_column_count',REPORT+512)]:
            bad=memory.copy()
            if name in ('kv','intermediate'):
                value=struct.unpack('<f',bytes(bad[address+i] for i in range(4)))[0]+1
                bad.update({address+i:b for i,b in enumerate(struct.pack('<f',value))})
            else:bad[address]^=1
            reject(name,lambda bad=bad:check_outputs(c,bad))
        bad=memory.copy();del bad[TRACE];reject('missing_output',lambda:check_outputs(c,bad))
    return {'passed':True,'checks':result}
