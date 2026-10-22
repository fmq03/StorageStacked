"""Compile the selected host ISA and the project's RV32 device sources."""
import argparse,fcntl,json,math,os,re,shutil,subprocess
from pathlib import Path
from paths import ROOT,RUNTIME,CACHE,VORTEX_BUILD,dump
from configure import load_config,simx_flags
from llm_model import load_model,encode
from elf_model import attach
p=argparse.ArgumentParser();p.add_argument('--config',required=True);p.add_argument('--output',required=True);p.add_argument('--model-header',default='');p.add_argument('--host-source',action='append',required=True);p.add_argument('sources',nargs='+');o=p.parse_args()
c=load_config(o.config);b=c['program'];out=Path(o.output).resolve();out.mkdir(parents=True,exist_ok=True)
with (CACHE/'application.lock').open('a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX)
    stage=VORTEX_BUILD/'tests/regression/userapp'
    if stage.exists():shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for f in Path.cwd().rglob('*'):
        if f.is_file() and f.suffix in ('.h','.hh','.hpp','.cc','.cpp','.c','.S'):
            dest=stage/f.relative_to(Path.cwd());dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,dest)
    for source in o.host_source+o.sources:
        if Path(source).is_absolute() or '..' in Path(source).parts or not (stage/source).is_file():raise ValueError('Sources must stay inside src/')
    header=['#pragma once'];model=None;ranges=[]
    if b['type']=='smoke':
        header+=['#define SMOKE_INPUT '+str(b['input'])+'u'];ranges=[(0,4*b['workers']),(0x10000,4*b['workers']),(0x5000c,4)]
    elif b['type']=='tiny_llm':
        if not o.model_header or Path(o.model_header).is_absolute() or '..' in Path(o.model_header).parts:raise ValueError('Use a local MODEL_HEADER')
        text=Path(o.model_header).read_text();model=json.loads(re.search(r'/\* MODEL_LAYOUT\s*(.*?)\s*MODEL_LAYOUT \*/',text,re.S)[1])
        values=[float.fromhex(v.strip().removesuffix('f')) for v in re.search(r'model_image\[LLM_WEIGHT_WORDS\]\s*=\s*\{(.*?)\};',text,re.S)[1].split(',') if v.strip()]
        for name,t in model['tensors'].items():
            offset=t.pop('offset');t['data']=values[offset:offset+math.prod(t['shape'])]
        load_model(model);prompt=encode(model,b['prompt']);positions=len(prompt)+b['generated_tokens']-1
        header += [f'#define LLM_GPU_THREADS {min(model["architecture"]["dim"],c["gpu"]["threads"]*c["gpu"]["warps"])}',f'#define LLM_PROMPT_LENGTH {len(prompt)}',f'#define LLM_GENERATE {b["generated_tokens"]}',f'#define LLM_KV_CACHE {int(b["kv_cache"])}','#ifndef __VORTEX__','static const unsigned prompt_image[LLM_PROMPT_LENGTH] = {'+','.join(map(str,prompt))+'};','#endif']
        ranges=[(0,len(values)*4),(0x10000,16*8*4),(0x20000,16*8*4),(0x30000,positions*148*4),(0x40000,positions*4),(0x50000,136*4)]
    else:
        header+=['#define APP_'+k+' '+str(v)+'u' for k,v in b['defines'].items()]
        ranges=[(int(e['address'],0)-0x90000000,4) for e in b['expect']]+[(0x5000c,4)]
    header += ['#define HOST_CPU_COUNT '+str(c['host']['num_cpus']),'#define APP_WORKGROUPS '+str(b.get('workers',1)),'#ifndef __VORTEX__','struct ReadbackRange { unsigned offset,bytes; };','static const ReadbackRange readback_ranges[] = {'+','.join('{'+str(a)+','+str(n)+'}' for a,n in ranges)+'};','#endif']
    (stage/'project_config.h').write_text('\n'.join(header)+'\n')
    sdk='-I'+str(ROOT/'third_party/vortex/sdk')
    build='ROOT_DIR := $(realpath ../../..)\ninclude $(ROOT_DIR)/config.mk\nPROJECT := application\nSRC_DIR := $(CURDIR)\nSRCS := '+' '.join(o.host_source)+'\nVX_SRCS := '+' '.join(o.sources)+'\nKERNEL_LIB := vortex2\nCXXFLAGS += -I$(SRC_DIR) '+sdk+' -pthread\nLDFLAGS += -pthread\nVX_CFLAGS += -I$(SRC_DIR) '+sdk+'\ninclude $(VORTEX_HOME)/tests/regression/common.mk\nVX_CFLAGS += -ffp-contract=off -fno-vectorize -fno-slp-vectorize\n'
    (stage/'Makefile').write_text(build)
    env=dict(os.environ)
    for key in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','DEBUG'):env.pop(key,None)
    env['CONFIGS']=simx_flags(c)
    isa=c['host']['isa']
    if isa=='aarch64':
        for key in ('CPATH','LIBRARY_PATH'):env.pop(key,None)
    subprocess.run(['make','-C',str(stage),'HOST_ARCH='+isa,'-j',os.environ['BUILD_JOBS']],env=env,check=True)
    app='application'+('-aarch64' if isa=='aarch64' else '')
    for src,dst in ((app,'host.elf'),('kernel.elf','program.elf'),('kernel.vxbin','program.vxbin'),('kernel.dump','program.dis')):shutil.copy2(stage/src,out/dst)
    if model:attach(out/'program.elf',model)
    source_dir=out/'sources'
    if source_dir.exists():shutil.rmtree(source_dir)
    source_dir.mkdir()
    for source in stage.rglob('*'):
        if source.is_file() and source.suffix in ('.h','.hh','.hpp','.cc','.cpp','.c','.S'):
            dest=source_dir/source.relative_to(stage);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
    shutil.copy2(Path.cwd()/'Makefile',source_dir/'Makefile')
    dump(out/'programs.json',{'host':{'sources':o.host_source,'binary':'host.elf','isa':'x86-64' if isa=='x86_64' else 'aarch64','runs_on':'gem5 TimingSimpleCPU'},'device':{'sources':o.sources,'binary':'program.elf','image':'program.vxbin','isa':'RV32','runs_on':'Vortex SimX'},'source_snapshot':'sources/'})
    dump(out/'readback.json',{'base':0x90000000,'size':0x60000,'ranges':[{'offset':a,'bytes':n} for a,n in ranges]})
    print('Built host.elf, program.elf and program.vxbin')
