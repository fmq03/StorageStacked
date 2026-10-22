"""Run smoke and TinyLLM for all TCM/L0 variants with numeric and storage checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import datetime
import fcntl
import json
import os
import re
import signal
import subprocess
import sys
import threading
from paths import ROOT, RUNTIME, CACHE, dump, relative
from session import finish
from result_layout import artifact

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--name',default='rtl-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
p.add_argument('--jobs',type=int,default=4)
o=p.parse_args()
if not re.fullmatch('[a-zA-Z0-9_-]+',o.name):raise ValueError('name must be one directory name')
if not 1<=o.jobs<=16:raise ValueError('jobs must be in 1..16')
out=CACHE/'validation'/o.name;out.mkdir(parents=True,exist_ok=False)
cases={};negative={};prepared=[]
active=set();active_lock=threading.Lock();stopping=threading.Event()

def invoke(arguments, stream, cwd=ROOT):
    with active_lock:
        if stopping.is_set():raise RuntimeError('Stopped after another RTL case failed')
        process=subprocess.Popen(list(map(str,arguments)),cwd=cwd,stdout=stream,
                                 stderr=subprocess.STDOUT,start_new_session=True)
        active.add(process)
    try:
        code=process.wait()
        if code:raise subprocess.CalledProcessError(code,arguments)
    finally:
        with active_lock:active.discard(process)

def stop_cases(futures):
    stopping.set()
    for future in futures:future.cancel()
    with active_lock:
        for process in active:
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass

def run_case(item):
    name,directory=item
    stage='simulate';code=1
    try:
        print('Running '+name,flush=True)
        binary=(directory/(directory/'build/simulator.path').read_text().strip()).resolve()
        with (directory/'run.log').open('w') as stream:
            invoke([sys.executable,RUNTIME/'session.py','status',directory,stage],stream)
            invoke([binary,'resolved.json','.'],stream,cwd=directory)
        stage='validate'
        with (directory/'validation.log').open('w') as stream:
            invoke([sys.executable,RUNTIME/'session.py','status',directory,stage],stream)
            invoke([sys.executable,RUNTIME/'validate.py',directory],stream)
        s=json.loads((directory/'summary.json').read_text());assert s['passed']
        code=0
        return name,{'passed':True,'report':relative(directory/'report.md',out),
                     'cycles':s['devices']['cycles'],'npu_model':s['npu_model'],
                     'output':s['smoke']['output'] if 'smoke' in s else s['llm']['generated_text'],
                     'checks':{key:value['passed'] for key,value in s['checks'].items()},
                     'wave_passed':all(value['passed'] for value in s['wave'].values())}
    finally:
        finish(directory,stage,code)

try:
    # Freeze all binaries before taking a shared lock for parallel simulations.
    with (CACHE/'build.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        for itcm,dtcm,l0 in ((i,d,l0) for i,d in ((8,32),(512,512),(1024,1024)) for l0 in (False,True)):
            for project in ('smoke','llm'):
                base=ROOT/'user'/project
                c=json.loads((base/'config.json').read_text())
                c['npu'].update(itcm_kib=itcm,dtcm_kib=dtcm,fetch_l0_enabled=l0)
                name=f'i{itcm}-d{dtcm}-l0{int(l0)}-{project}'
                config=base/'result'/o.name/'configs'/(name+'.json')
                config.parent.mkdir(parents=True,exist_ok=True);dump(config,c)
                directory=base/'result'/o.name/name
                print('Preparing '+name,flush=True)
                with (out/(name+'.log')).open('w') as stream:
                    invoke([sys.executable,RUNTIME/'session.py','prepare',project,relative(config,base),relative(directory,base)],stream)
                try:
                    with (directory/'build.log').open('w') as stream:
                        invoke(['make','-C',base/'src','CONFIG='+relative(directory/'input.json',base/'src'),
                                'OUT='+relative(directory/'build',base/'src')],stream)
                        invoke([sys.executable,RUNTIME/'session.py','resolve',directory],stream)
                except Exception:
                    finish(directory,'build',1)
                    raise
                prepared.append((name,directory))
        fcntl.flock(lock,fcntl.LOCK_SH)
        with ThreadPoolExecutor(max_workers=o.jobs) as pool:
            futures=[pool.submit(run_case,item) for item in prepared]
            try:
                for future in as_completed(futures):
                    name,result=future.result();cases[name]=result
                    dump(out/'summary.json',{'passed':False,'cases':cases})
            except BaseException:
                stop_cases(futures)
                raise
    source=ROOT/'user/smoke/result'/o.name/'i8-d32-l00-smoke'
    binary=(source/(source/'build/simulator.path').read_text().strip()).resolve()
    for name,fields,message in (
        ('wrong_loaded_model',{'itcm_kib':512,'dtcm_kib':512},'Loaded RTL TCM geometry differs'),
        ('wrong_loaded_l0',{'fetch_l0_enabled':True},'Loaded RTL features differ'),
    ):
        bad=json.loads(artifact(source,'resolved.json').read_text())
        bad['architecture']['npu'].update(fields)
        directory=out/name;directory.mkdir();dump(directory/'resolved.json',bad)
        with (CACHE/'build.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_SH)
            run=subprocess.run([str(binary),str(directory/'resolved.json'),str(directory)],cwd=source,capture_output=True,text=True)
        (directory/'run.log').write_text(run.stdout+run.stderr)
        assert run.returncode!=0 and message in run.stderr
        negative[name]={'rejected':True,'log':relative(directory/'run.log',out)}
    dump(out/'summary.json',{'passed':True,'cases':cases,'negative':negative})
    print('PASS: '+relative(out/'summary.json'))
except Exception as error:
    dump(out/'summary.json',{'passed':False,'cases':cases,'error':str(error)})
    raise
