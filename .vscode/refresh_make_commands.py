#!/usr/bin/env python3
"""Extract active Vortex commands with make dry runs inside its tool environment."""
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT=Path(__file__).resolve().parents[1]
VORTEX=ROOT/'vortex_StorageStacked'
BUILD=VORTEX/'third_party/.cache/build/vortex'
SOURCE=VORTEX/'third_party/vortex'
SKIP={'-o','-MF','-MT','-MQ','-L','-l','-T'}
entries=[]
for part,target,variables in [('sim/simx','libvortex-gem5',['USE_GEM5=1']),('sw/runtime/stub','all',[]),('sw/runtime/gem5','all',[]),('tests/regression/userapp','all',[])]:
    directory=BUILD/part
    command=['make','-B','-n','-C',str(directory),target,*variables,'CONFIGS='+os.environ['NAV_CONFIGS']]
    env=dict(os.environ,CXXFLAGS='-I'+str(VORTEX/'third_party/.cache/tools/cmake-sources/spdlog/include')+' -I'+str(VORTEX/'third_party/.cache/tools/cmake-sources/yaml-cpp/include'))
    proc=subprocess.run(command,text=True,capture_output=True,env=env)
    if proc.returncode:raise RuntimeError(proc.stderr[-2000:])
    for line in proc.stdout.splitlines():
        try:words=shlex.split(line)
        except ValueError:continue
        if not words:continue
        if Path(words[0]).name=='ccache':words.pop(0)
        if not words or not (Path(words[0]).name in ('g++','gcc','clang','clang++') or Path(words[0]).name.endswith(('-g++','-gcc'))):continue
        if any(w in words for w in ['-M','-MM','-E']):continue
        compiler=words[0];flags=[];sources=[];i=1
        while i<len(words):
            w=words[i]
            if w in SKIP:i+=2;continue
            if w in ('-I','-D','-U','-isystem','-include','-iquote','-x'):
                flags+=words[i:i+2];i+=2;continue
            if w in ('-c','-MMD','-MD','-MP','-shared','-fno-tree-loop-distribute-patterns') or w.startswith(('-Wl,','-l','-L')):i+=1;continue
            if w.startswith('-'):flags.append(w)
            else:
                p=(directory/w).resolve()
                if p.suffix in {'.c','.cc','.cpp','.cxx'} and p.is_file():sources.append(p)
            i+=1
        for source in sources:
            entries.append({'directory':str(directory),'file':str(source),'arguments':[compiler,*flags,'-c',str(source)]})
            if part=='tests/regression/userapp':
                for original in (VORTEX/'user').glob('*/src/'+source.name):
                    if original.read_bytes()==source.read_bytes():
                        entries.append({'directory':str(directory),'file':str(original),'arguments':[compiler,*flags,'-I'+str(original.parent),'-c',str(original)]})
(ROOT/'.vscode/make_commands.json').write_text(json.dumps(entries,indent=2)+'\n')
print('Vortex make commands:',len(entries))
