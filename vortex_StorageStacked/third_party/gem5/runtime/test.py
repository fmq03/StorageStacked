"""One regression run; each selected project executes its default scenario once."""
import argparse,datetime,json,subprocess,sys
from pathlib import Path
from paths import ROOT,CACHE,MEMSIM_BUILD,STORAGE_ROOT,RUNTIME,dump,relative
from report import write_report,write_status
from check_negative import check
p=argparse.ArgumentParser();p.add_argument('--project',choices=('smoke','llm'));o=p.parse_args()
stamp='test-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
output=CACHE/'validation'/stamp;output.mkdir(parents=True);write_status(output,'regression')
cases=[]
def add(name,project,updates):
    c=json.loads((ROOT/'user'/project/'config.json').read_text())
    for group,fields in updates.items():c[group].update(fields)
    path=ROOT/'user'/project/'result'/stamp/(name+'.json');dump(path,c)
    cases.append((name,project,path,c))
if o.project in (None,'smoke'):
    add('smoke','smoke',{})
if o.project in (None,'llm'):
    add('llm','llm',{})

def run(case):
    name,project,path,c=case;result=path.parent/name
    with (output/(name+'.log')).open('w') as log:
        subprocess.run([str(ROOT/'user/run.sh'),project,'--config',relative(path,ROOT/'user'/project),'--output',relative(result,ROOT/'user'/project)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    s=json.loads((result/'summary.json').read_text());assert s['passed']
    return name,result,s
try:
    with (output/'host-config.log').open('w') as log:
        subprocess.run([sys.executable,'-m','unittest','discover','-s',str(RUNTIME/'tests'),'-v'],stdout=log,stderr=subprocess.STDOUT,check=True)
    finished=[run(case) for case in cases]
    by_name={name:(result,s) for name,result,s in finished}
    negative=check(by_name['smoke'][0] if 'smoke' in by_name else None,by_name['llm'][0] if 'llm' in by_name else None)
    native_plan=json.loads(subprocess.check_output(['ctest','--test-dir',str(MEMSIM_BUILD),'--show-only=json-v1'],text=True))
    with (output/'native.log').open('w') as log:subprocess.run(['ctest','--test-dir',str(MEMSIM_BUILD),'--output-on-failure'],stdout=log,stderr=subprocess.STDOUT,check=True)
    with (output/'api.log').open('w') as log:subprocess.run(['python',str(STORAGE_ROOT/'mem_sim/integration/check_online.py'),str(MEMSIM_BUILD/'libstoragestacked_memsim.so'),str(output/'api')],stdout=log,stderr=subprocess.STDOUT,check=True)
    s={'passed':True,'regression_runs':1,'host_configuration_tests_passed':True,'native_tests_passed':len(native_plan['tests']),'api_check':json.loads((output/'api/api_check.json').read_text()),'negative':negative,'cases':{}}
    for name,result,item in finished:
        value=item['smoke']['output'] if 'smoke' in item else item['llm']['generated_text']
        s['cases'][name]={'passed':True,'output':value,'cycles':item['devices']['cycles'],'active_cpus':item['host_execution']['active_cpus'],'cpu_instructions':[core['instructions'] for core in item['host_execution']['cores']],'report':relative(result/'report.md',output)}
    dump(output/'summary.json',s);write_report(output);print('PASS:',relative(output/'report.md'))
except Exception as error:
    write_status(output,'regression',str(error));raise

finally:
    from portable import sanitize
    sanitize(output)
