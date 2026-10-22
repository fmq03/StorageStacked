"""SDK Makefile backend: build declared sources, never select a program by directory name."""
import argparse
import fcntl
import copy
import json
import math
import re
import shutil
from pathlib import Path
from paths import ROOT, CACHE
from configure import load_config, memory_layout
from native_build import bazel
from llm_model import load_model, layout, encode
from elf_model import attach

p=argparse.ArgumentParser();p.add_argument('--config',required=True);p.add_argument('--output',required=True)
p.add_argument('--model-header',default='');p.add_argument('sources',nargs='+');o=p.parse_args()
application_lock=(CACHE/'application.lock').open('a')
fcntl.flock(application_lock,fcntl.LOCK_EX)
c=load_config(o.config);b=c['program'];out=Path(o.output).resolve();out.mkdir(parents=True,exist_ok=True)
stage=ROOT/'coralnpu/sdk/staging'
if stage.exists():shutil.rmtree(stage)
stage.mkdir()
# Everything owned by this Makefile remains local to the project source directory.
for f in Path.cwd().rglob('*'):
    if f.is_file() and f.suffix in ('.h','.hh','.hpp','.cc','.cpp','.c','.S'):
        destination=stage/f.relative_to(Path.cwd());destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,destination)
for source in o.sources:
    if Path(source).is_absolute() or '..' in Path(source).parts or not (stage/source).is_file():raise ValueError('Source paths must stay inside src/')
header=['#pragma once'];model=None
for key,value in memory_layout(c).items():header.append('#define NPU_'+key.upper()+' '+str(value)+'u')
if b['type']=='smoke':header += ['#define SMOKE_INPUT '+str(b['input'])+'u']
elif b['type']=='tiny_llm':
    if not o.model_header or Path(o.model_header).is_absolute() or '..' in Path(o.model_header).parts:raise ValueError('TinyLLM needs a local MODEL_HEADER')
    text=Path(o.model_header).read_text()
    model=json.loads(re.search(r'/\* MODEL_LAYOUT\s*(.*?)\s*MODEL_LAYOUT \*/',text,re.S)[1])
    weights=re.search(r'model_image\[LLM_WEIGHT_WORDS\]\s*=\s*\{(.*?)\};',text,re.S)[1]
    values=[float.fromhex(v.strip().removesuffix('f')) for v in weights.split(',') if v.strip()]
    for name,t in model['tensors'].items():
        offset=t.pop('offset');t['data']=values[offset:offset+math.prod(t['shape'])]
    load_model(model)
    prompt=encode(model,b['prompt'])
    header += [f'#define LLM_PROMPT_LENGTH {len(prompt)}',f'#define LLM_GENERATE {b["generated_tokens"]}',
               f'#define LLM_KV_CACHE {int(b["kv_cache"])}',
               'static volatile unsigned prompt_image[LLM_PROMPT_LENGTH] = {'+','.join(map(str,prompt))+'};']
else:
    header += ['#define APP_'+k+' '+str(v)+'u' for k,v in b['defines'].items()]
(stage/'project_config.h').write_text('\n'.join(header)+'\n')
headers=[str(f.relative_to(stage)) for f in stage.rglob('*') if f.suffix in ('.h','.hh','.hpp')]
build='load("//rules:coralnpu_v2.bzl", "coralnpu_v2_binary")\ncoralnpu_v2_binary(name="program", srcs='+repr(o.sources+headers)+', stack_size_bytes=4096, itcm_size_kbytes='+str(c['npu']['itcm_kib'])+', dtcm_size_kbytes='+str(c['npu']['dtcm_kib'])+', enable_vmem=False, copts=["-Os", "-fno-tree-vectorize", "-fno-tree-slp-vectorize", "-ffp-contract=off", "-march=rv32imf_zve32f_zicsr_zifencei"])\n'
build=build.replace('copts=[', 'copts=['+repr('-ffile-prefix-map='+str(ROOT)+'=.')+', ')
if model:
    build+='genrule(name="program_disassembly", srcs=[":program.elf"], outs=["program.dis"], tools=["@toolchain_coralnpu_v2//:bin/llvm-objdump"], cmd="$(location @toolchain_coralnpu_v2//:bin/llvm-objdump) --disassemble $< > $@")\n'
(stage/'BUILD').write_text(build)
targets=['//sdk/staging:program.elf']
if model:targets.append('//sdk/staging:program_disassembly')
bazel('build',targets)
files=bazel('cquery',['set('+' '.join(targets)+')'],capture=True).splitlines()
elf=next(ROOT/'coralnpu'/f for f in files if f.endswith('/program.elf'))
(out/'program.elf').unlink(missing_ok=True)
shutil.copy2(elf,out/'program.elf')
(out/'program.elf').chmod(0o755)
if model:
    disassembly=next(ROOT/'coralnpu'/f for f in files if f.endswith('/program.dis'))
    instructions=disassembly.read_text()
    for mnemonic in ('vsetvli','vle32.v','vfmul.vf','vfadd.vv','vse32.v'):
        if not re.search(r'\s'+re.escape(mnemonic)+r'\s',instructions):
            raise ValueError('Missing RVV instruction in TinyLLM ELF: '+mnemonic)
    (out/'program.dis').unlink(missing_ok=True)
    shutil.copy2(disassembly,out/'program.dis')
    attach(out/'program.elf',model)
print('Built program.elf')
