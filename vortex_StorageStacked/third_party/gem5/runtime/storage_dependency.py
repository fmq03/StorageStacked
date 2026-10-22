"""Locate the independent storage sources and record their version."""
import json
import subprocess
from paths import ROOT, STORAGE_ROOT, MEMSIM_BUILD, BUILD, RUNTIME, relative, dump
expected = json.loads((RUNTIME/'defaults/storage_dependency.json').read_text())
def storage_version():
    if not (STORAGE_ROOT/'storage_axi/storage_config.hh').is_file(): raise RuntimeError('Missing storage sources; configure ./build.sh --storage')
    version = (STORAGE_ROOT/'VERSION').read_text().strip()
    if version != expected['version']: raise RuntimeError('Incompatible storage version')
    result = {'root':relative(STORAGE_ROOT), 'version':version}
    if (STORAGE_ROOT/'.git').exists():
        result['commit'] = subprocess.check_output(['git','-C',str(STORAGE_ROOT),'rev-parse','HEAD'],text=True).strip()
        result['dirty'] = bool(subprocess.check_output(['git','-C',str(STORAGE_ROOT),'status','--porcelain'],text=True))
    return result
def record_build(): dump(BUILD/'storage.json',storage_version())
def require_build():
    p=BUILD/'storage.json'
    if not p.exists(): raise RuntimeError('Build the platform first: ./build.sh')
    saved=json.loads(p.read_text());current=storage_version()
    if any(saved[k]!=current[k] for k in ('root','version')): raise RuntimeError('Storage selection changed; run ./build.sh')
