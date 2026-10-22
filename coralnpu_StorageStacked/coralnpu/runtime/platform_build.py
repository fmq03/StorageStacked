"""Build platform libraries and one UCIe configuration in private, disposable caches."""
import json
import os
import shutil
import subprocess
from pathlib import Path
from paths import ROOT,CACHE,BUILD,MEMSIM_BUILD,STORAGE_ROOT,SETTINGS,relative,dump
from rtl_build import build_native, variant_name
from configure import load_config
from storage_dependency import storage_version

def command(args):
    subprocess.run(list(map(str,args)),cwd=ROOT,check=True)

def build_platform():
    storage_version();BUILD.mkdir(parents=True,exist_ok=True)
    command(['cmake','-S',relative(STORAGE_ROOT/'mem_sim'),'-B',relative(MEMSIM_BUILD),'-G','Ninja',
             '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_COMPILER='+os.environ['AXI_CXX']])
    command(['cmake','--build',relative(MEMSIM_BUILD),'-j',SETTINGS['jobs']])
    build_native(load_config(ROOT/'user/smoke/config.json'))
    dump(BUILD/'storage.json',storage_version())

def build_simulator(c):
    u=c['ucie'];name='link-'+ '-'.join(format(u[k],'.12g') for k in ('lanes','rate_gtps','bits_per_symbol','tat_ns'))
    native=build_native(c)
    directory=BUILD/(name+'-'+variant_name(c))
    args=['cmake','-S','integration','-B',relative(directory),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
          '-DCMAKE_CXX_COMPILER='+os.environ['AXI_CXX'],'-DCMAKE_C_COMPILER='+os.environ['AXI_CC'],
          '-DCMAKE_ASM_COMPILER='+os.environ['AXI_CC'],'-DSTORAGE_PATH='+relative(STORAGE_ROOT)]
    args.append('-DNPU_NATIVE_LIBRARY='+str(native))
    for k,v in u.items():args.append('-DLINK_'+k.upper()+'='+str(v))
    command(args);command(['cmake','--build',relative(directory),'-j',SETTINGS['jobs']])
    return directory/'coralnpu_sim'
