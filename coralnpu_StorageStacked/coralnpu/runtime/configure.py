"""Validate one user project JSON. Project names are directory names, not built-in targets."""
import json
import math
import re
from pathlib import Path
from paths import ROOT, dump
DEVICE = 'coralnpu'

def keys(obj, expected, label):
    if not isinstance(obj, dict) or set(obj) != set(expected.split()):
        raise ValueError(label + ': expected fields ' + expected)

def integer(v, lo, hi, name):
    if type(v) is not int or not lo <= v <= hi:
        raise ValueError(f'{name} must be an integer in [{lo}, {hi}]')

def validate(c):
    keys(c, 'program npu axi ucie memsim simulation', 'project')
    n=c['npu']
    if not isinstance(n,dict):raise ValueError('npu must be an object')
    optional={'fetch_l0_enabled','itcm_kib','dtcm_kib'}
    keys({k:v for k,v in n.items() if k not in optional}, 'clock_mhz', 'npu')
    for key,value in (('fetch_l0_enabled',False),('itcm_kib',8),('dtcm_kib',32)):n.setdefault(key,value)
    if type(n['fetch_l0_enabled']) is not bool:raise ValueError('npu.fetch_l0_enabled must be boolean')
    for key in ('itcm_kib','dtcm_kib'):integer(n[key],1,1024,'npu.'+key)
    if (n['itcm_kib'],n['dtcm_kib']) not in ((8,32),(512,512),(1024,1024)):
        raise ValueError('Supported ITCM/DTCM presets: 8/32, 512/512, 1024/1024 KiB')
    integer(c['npu']['clock_mhz'], 1, 10000, 'npu.clock_mhz')
    if 10**9 % c['npu']['clock_mhz']: raise ValueError('NPU period must be integral fs')
    if not isinstance(c['axi'],dict): raise ValueError('axi must be an object')
    keys({k:v for k,v in c['axi'].items() if k!='burst_beats'}, 'period_ns outstanding planes stalls replay', 'axi')
    beats=c['axi'].get('burst_beats',4)
    if type(beats) is not int or beats not in (1,2,4,8,16):
        raise ValueError('axi.burst_beats must be 1, 2, 4, 8 or 16')
    for k, hi in [('period_ns',1000), ('outstanding',128), ('planes',4)]: integer(c['axi'][k],1,hi,'axi.'+k)
    for k in ('stalls','replay'):
        if type(c['axi'][k]) is not bool: raise ValueError('axi.'+k+' must be boolean')
    keys(c['ucie'], 'lanes rate_gtps bits_per_symbol tat_ns', 'ucie')
    integer(c['ucie']['lanes'],1,256,'ucie.lanes')
    integer(c['ucie']['bits_per_symbol'],1,2,'ucie.bits_per_symbol')
    for k,lo,hi in [('rate_gtps',0,1000),('tat_ns',0,1000000)]:
        v=c['ucie'][k]
        if type(v) not in (int,float) or not math.isfinite(v) or not lo <= v <= hi or (k=='rate_gtps' and v==0):
            raise ValueError('invalid ucie.'+k)
    keys(c['memsim'], 'channels scale queue slots response_hold standard', 'memsim')
    for k in ('channels','scale','queue','slots'): integer(c['memsim'][k],1,1023 if k=='slots' else 1024,'memsim.'+k)
    integer(c['memsim']['response_hold'],0,1000000,'memsim.response_hold')
    if c['memsim']['standard'] not in ('hbm3','hbm4','lpddr5','lpddr6'): raise ValueError('unsupported memsim.standard')
    keys(c['simulation'],'max_ticks','simulation'); integer(c['simulation']['max_ticks'],1,10**17,'max_ticks')
    b=c['program']; kind=b.get('type')
    if kind=='smoke':
        keys(b,'type input','program'); integer(b['input'],0,0xffffffff,'program.input')
    elif kind=='tiny_llm':
        keys(b,'type prompt generated_tokens kv_cache','program')
        integer(b['generated_tokens'],1,8,'program.generated_tokens')
        if type(b['kv_cache']) is not bool: raise ValueError('kv_cache must be boolean')
        if not isinstance(b['prompt'],str) or not b['prompt'] or len(b['prompt'])+b['generated_tokens']-1>16:
            raise ValueError('prompt and generated tokens exceed the model context (16)')
    elif kind=='custom':
        keys(b,'type defines expect','program')
        if not isinstance(b['defines'],dict): raise ValueError('defines must be an object')
        for name,v in b['defines'].items():
            if not re.fullmatch('[A-Z][A-Z0-9_]*',name): raise ValueError('define names must use A-Z, 0-9 and underscore')
            integer(v,0,0xffffffff,'define '+name)
        if not isinstance(b['expect'],list) or not b['expect']: raise ValueError('custom programs require at least one expected output word')
        for e in b['expect']:
            keys(e,'address value','expect')
            address=int(e['address'],0)
            if address%4 or not 0x90000000<=address<0xc0000000: raise ValueError('expected address must be an aligned external memory word')
            integer(e['value'],0,0xffffffff,'expected value')
    else: raise ValueError('program.type must be smoke, tiny_llm or custom')
    return c

def load_config(path):
    return validate(json.loads(Path(path).read_text()))

def resolved_parts(c):
    a={'npu':c['npu'],'device_clock_mhz':c['npu']['clock_mhz'],'axi':{'burst_beats':4,**c['axi']},'memory':c['memsim'],'max_ticks':c['simulation']['max_ticks'],'ucie':c['ucie']}
    b={'name':c['program']['type'],**{k:v for k,v in c['program'].items() if k!='type'}}
    return a,b

def load(path):
    return resolved_parts(load_config(path))


def rtl_config(c):
    return {key:c['npu'][key] for key in ('fetch_l0_enabled','itcm_kib','dtcm_kib')}

def memory_layout(c):
    n=c['npu'];small=(n['itcm_kib'],n['dtcm_kib'])==(8,32)
    return {'itcm_base':0,'itcm_bytes':n['itcm_kib']*1024,
            'dtcm_base':0x10000 if small else 0x100000,'dtcm_bytes':n['dtcm_kib']*1024,
            'csr_base':0x30000 if small else 0x200000}
