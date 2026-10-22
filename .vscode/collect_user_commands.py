#!/usr/bin/env python3
"""Capture each user's real build recipe with a private generated-header snapshot."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('platform',choices=['vortex','coralnpu']);o=p.parse_args()
PLATFORM=ROOT/(o.platform+'_StorageStacked')
OUT=ROOT/'.vscode/projects'/o.platform
RUNTIME=PLATFORM/('third_party/gem5/runtime' if o.platform=='vortex' else 'coralnpu/runtime')
sys.path.insert(0,str(RUNTIME))
from configure import load_config
if o.platform=='vortex':
    from configure import simx_flags
else:
    from native_build import bazel

SUFFIXES={'.c','.cc','.cpp','.cxx'}
DROP_VALUES={'-o','-MF','-MT','-MQ','-L','-l','-T','-mllvm'}
DROP={'-MMD','-MD','-MP','-shared','-Werror','-Wfatal-errors','-fno-tree-loop-distribute-patterns','-fno-canonical-system-headers'}
VALUE_FLAGS={'-I','-D','-U','-isystem','-include','-iquote','-x','-Xclang','--target','--sysroot','--gcc-toolchain','-isysroot','-resource-dir'}

def clean(words, directory, source, original, snapshot, stage):
    args=['/usr/bin/clang++','-I'+str(original.parent),'-I'+str(snapshot)]
    if o.platform=='coralnpu':
        args.append('--target=riscv32-unknown-elf')
        resource=ROOT/'.vscode/tools/clangd_22.1.0/lib/clang/22'
        if not (resource/'include/riscv_vector.h').is_file():
            raise RuntimeError('Run bash .vscode/setup_verification.sh to install clangd 22 first')
        builtins=ROOT/'.vscode/builtin-headers';builtins.mkdir(exist_ok=True)
        vector=builtins/'riscv_vector.h';vector.unlink(missing_ok=True)
        vector.symlink_to(Path(resource)/'include/riscv_vector.h')
        args += ['-I'+str(builtins)]
    index=1
    while index<len(words):
        word=words[index]
        if words[index:index+4]==['-Xclang','-target-feature','-Xclang','+xvortex']:
            index+=4;continue
        if word in DROP_VALUES:index+=2;continue
        if word in DROP or word.startswith(('-Wl,','-l','-L','-frandom-seed=')):index+=1;continue
        if word in VALUE_FLAGS:
            args+=words[index:index+2];index+=2;continue
        if not word.startswith('-') and Path(word).suffix in SUFFIXES:
            index+=1;continue
        args.append(word);index+=1
    args=[word.replace(str(stage),str(snapshot)) for word in args]
    if '-c' not in args:args.append('-c')
    args.append(str(original))
    return {'directory':str(directory),'file':str(original),'arguments':args}

entries=[];projects=[]
for base in sorted((PLATFORM/'user').iterdir()):
    src=base/'src';config=base/'config.json'
    if not (src/'Makefile').exists() or not config.exists():continue
    destination=OUT/base.name;destination.mkdir(parents=True,exist_ok=True)
    c=load_config(config)
    env=dict(os.environ)
    for key in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','DEBUG'):env.pop(key,None)
    with (destination/'build.log').open('w') as log:
        subprocess.run(['make','-C',str(src),'CONFIG='+str(config),'OUT='+str(destination/'build')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    snapshot=destination/'sources'
    stage=PLATFORM/('third_party/.cache/build/vortex/tests/regression/userapp' if o.platform=='vortex' else 'coralnpu/sdk/staging')
    if snapshot.exists():shutil.rmtree(snapshot)
    snapshot.mkdir()
    for file in stage.rglob('*'):
        if file.is_file() and file.suffix in {'.c','.cc','.cpp','.h','.hh','.hpp','.S'}:
            target=snapshot/file.relative_to(stage);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,target)
    original_sources={f.name:f for f in src.iterdir() if f.suffix in SUFFIXES}
    for name,original in original_sources.items():
        assert (snapshot/name).read_bytes()==original.read_bytes(),str(original)
    collected=[]
    if o.platform=='vortex':
        result=subprocess.run(['make','-B','-n','-C',str(stage),'HOST_ARCH='+c['host']['isa'],'CONFIGS='+simx_flags(c)],text=True,capture_output=True,env=env,check=True)
        for line in result.stdout.splitlines():
            try:words=shlex.split(line)
            except ValueError:continue
            if words and Path(words[0]).name=='ccache':words.pop(0)
            if not words or not (Path(words[0]).name in ['gcc','g++','clang','clang++'] or Path(words[0]).name.endswith(('-gcc','-g++'))):continue
            for word in words:
                if word.startswith('-') or Path(word).suffix not in SUFFIXES:continue
                source=(stage/word).resolve()
                if source.parent==stage and source.name in original_sources:
                    collected.append(clean(words,stage,source,original_sources[source.name],snapshot,stage))
    else:
        actions=json.loads(bazel('aquery',['mnemonic("CppCompile", deps(//sdk/staging:program.elf))','--output=jsonproto'],True))
        # The most recent Bazel invocation refreshes this workspace symlink.
        directory=(PLATFORM/'coralnpu/bazel-out').resolve().parent
        for action in actions.get('actions',[]):
            words=action.get('arguments',[])
            if '-c' not in words:continue
            source=Path(words[words.index('-c')+1])
            if source.parent.as_posix()=='sdk/staging' and source.name in original_sources:
                collected.append(clean(words,directory,source,original_sources[source.name],snapshot,stage))
    indexed={Path(e['file']).name for e in collected}
    if indexed!=set(original_sources):raise RuntimeError(f'{base.name}: missing source actions: {set(original_sources)-indexed}')
    # Generated SDK staging paths must not outlive this capture. Rebase them
    # even when Bazel spells them relative to its execution root.
    for entry in collected:
        entry['arguments']=[word.replace('sdk/staging',str(snapshot)) if 'sdk/staging' in word else word for word in entry['arguments']]
    entries.extend(collected)
    projects.append({'project':str(base.relative_to(ROOT)),'sources':sorted(indexed),'configuration':str(config.relative_to(ROOT)),'snapshot':str(snapshot.relative_to(ROOT))})
    print(o.platform,base.name,len(collected),'source commands',flush=True)
OUT.mkdir(parents=True,exist_ok=True)
(OUT/'commands.json').write_text(json.dumps(entries,indent=2)+'\n')
(OUT/'projects.json').write_text(json.dumps(projects,indent=2)+'\n')
