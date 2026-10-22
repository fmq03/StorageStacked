#!/usr/bin/env python3
"""One regression run; each selected project executes its default device scenario once."""
import argparse
import datetime
import json
import os
import subprocess
import sys
from paths import ROOT,RUNTIME,CACHE,MEMSIM_BUILD,STORAGE_ROOT,relative,dump
from report import write_status
from portable import sanitize
p=argparse.ArgumentParser();p.add_argument('--project',choices=['smoke','llm']);o=p.parse_args()
stamp='test-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
# Full-suite bookkeeping is an internal maintenance artifact, never a user configuration.
out=CACHE/'validation'/stamp;out.mkdir(parents=True);write_status(out,'running')
def read(p):return json.loads(p.read_text())
def invoke(cmd,log):
    with (out/log).open('w') as stream:subprocess.run(list(map(str,cmd)),cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,check=True)
cases={};directories={}
def case(project):
    name=project
    base=ROOT/'user'/project
    directory=base/'result'/stamp/name
    config=base/'result'/stamp/'configs'/(name+'.json');config.parent.mkdir(parents=True,exist_ok=True)
    c=read(base/'config.json')
    dump(config,c)
    print('Running '+project+'/'+name,flush=True)
    invoke(['bash','user/run.sh',project,'--config',relative(config,base),'--output',relative(directory,base)],name+'.log')
    result=read(directory/'summary.json');assert result['passed'];cases[name]=result;directories[name]=directory
try:
    os.environ['PYTHONPATH']=str(RUNTIME)+os.pathsep+os.environ.get('PYTHONPATH','')
    invoke([sys.executable,'-m','unittest','discover','-s',relative(RUNTIME/'tests'),'-v'],'configuration-unit.log')
    invoke([sys.executable,relative(RUNTIME/'check.py')],'environment.log')
    plan=json.loads(subprocess.check_output(['ctest','--test-dir',relative(MEMSIM_BUILD),'--show-only=json-v1'],cwd=ROOT,text=True))
    invoke(['ctest','--test-dir',relative(MEMSIM_BUILD),'--output-on-failure'],'native.log')
    invoke([sys.executable,relative(STORAGE_ROOT/'mem_sim/integration/check_online.py'),relative(MEMSIM_BUILD/'libstoragestacked_memsim.so'),relative(out/'api')],'api.log')
    if o.project in (None,'smoke'):
        case('smoke')
        for script in ('check_smoke_negative','check_negative'):
            invoke([sys.executable,relative(RUNTIME/(script+'.py')),relative(directories['smoke']),relative(out/(script+'.json'))],script+'.log')
    if o.project in (None,'llm'):
        case('llm')
        invoke([sys.executable,relative(RUNTIME/'check_llm_negative.py'),relative(directories['llm']),relative(out/'check_llm_negative.json')],'llm-negative.log')
    negatives={p.stem:read(p) for p in out.glob('check*negative.json')};assert all(v['passed'] for v in negatives.values())
    burst_tests=None
    if o.project is None:
        invoke([sys.executable,relative(RUNTIME/'test_burst.py'),'--output',relative(out/'burst-matrix')],'burst.log')
        burst_tests=read(out/'burst-matrix/summary.json');assert burst_tests['passed']
    result={'passed':True,'regression_runs':1,'core':'CoralNPU RVV','configuration_unit_tests_passed':True,'native_tests_passed':len(plan['tests']),'api_check':read(out/'api/api_check.json'),'negative_checks':negatives,
            'cases':{name:{'passed':True,'cycles':s['devices']['cycles'],'transactions':s['sources']['coralnpu']['transactions'],
                           'output':s['smoke']['output'] if 'smoke' in s else s['llm']['generated_text'],
                           'report':relative(directories[name]/'report.md',ROOT)} for name,s in cases.items()}}
    if burst_tests is not None:result['burst_tests']=burst_tests
    dump(out/'summary.json',result)
    lines=['# 回归报告','',f"PASS：{len(cases)} 个设备场景、{len(plan['tests'])} 项原生测试、在线 C ABI 和错误拒绝检查通过。",'',
           '| 场景 | 输出 | NPU 周期 | 外部事务 | 报告 |','|---|---|---:|---:|---|']
    for name,s in result['cases'].items():lines.append(f"| {name} | {s['output']} | {s['cycles']} | {s['transactions']} | [查看]({relative(directories[name]/'report.md',out)}) |")
    (out/'report.md').write_text('\n'.join(lines)+'\n')
    print('PASS: '+relative(out/'report.md'))
except Exception as error:
    write_status(out,'acceptance',error);raise
finally:sanitize(out)
