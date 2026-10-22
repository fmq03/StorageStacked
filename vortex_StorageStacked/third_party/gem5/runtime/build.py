"""Build the reusable platform for one validated project configuration."""
import argparse,json,os,shutil,subprocess
from pathlib import Path
from paths import ROOT,CACHE,BUILD,RUNTIME,MEMSIM_BUILD,STORAGE_ROOT,SETTINGS,ARM_SYSROOT,gem5_binary,host_runtime,dump
from configure import load_config,build_inputs,simx_flags
from storage_dependency import record_build

def run(args,cwd=ROOT):subprocess.run(list(map(str,args)),cwd=cwd,check=True)
def build(config,force=False,ready=False,refresh=False):
    config=Path(config).resolve();c=load_config(config)
    selected={'device':build_inputs(c),'ucie':c['ucie']}
    marker=BUILD/'platform.json';previous=json.loads(marker.read_text()) if marker.exists() else {}
    isa=c['host']['isa'];gem5=gem5_binary(isa);runtime=host_runtime(isa)
    host_marker=BUILD/f'host-{isa}.json'
    # Each gem5 binary embeds the UCIe constants independently.
    host_selected={'isa':isa,**selected}
    host_previous=json.loads(host_marker.read_text()) if host_marker.exists() else {}
    outputs=[gem5,runtime/'libvortex.so',runtime/f'libvortex-gem5-{isa}.so',BUILD/'vortex/sim/simx/libvortex-gem5.so']
    valid=previous==selected and host_previous==host_selected and all(p.exists() for p in outputs)
    if ready:return valid
    if not force and not refresh and valid:return
    if isa=='aarch64':
        if not shutil.which('aarch64-linux-gnu-g++') or not (ARM_SYSROOT/'lib/ld-linux-aarch64.so.1').is_file():
            raise RuntimeError('aarch64 requires aarch64-linux-gnu-g++ and /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 (Ubuntu: g++-aarch64-linux-gnu libc6-dev-arm64-cross)')
    BUILD.mkdir(parents=True,exist_ok=True)
    dump(CACHE/'link.json',c['ucie']);os.environ['STORAGE_LINK_CONFIG']=str(CACHE/'link.json')
    run(['cmake','-S',STORAGE_ROOT/'mem_sim','-B',MEMSIM_BUILD,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_COMPILER='+os.environ['AXI_CXX']])
    run(['cmake','--build',MEMSIM_BUILD,'-j',SETTINGS['jobs']])
    home=ROOT/'third_party/gem5';scons=os.environ['GEM5_SCONS']
    args=['CXX='+os.environ['AXI_CXX'],'CC='+os.environ['AXI_CC'],'PYTHON_CONFIG='+os.environ['PYTHON_CONFIG'],'EXTRAS='+str(ROOT/'integration')+':'+str(STORAGE_ROOT/'storage_axi')]
    target='build/'+gem5.parent.name;build_opt='X86' if isa=='x86_64' else 'ARM'
    if not (gem5.parent/'gem5.build/config').exists():run([scons,'defconfig',target,'build_opts/'+build_opt,*args],home)
    run([scons,'setconfig',target,'RUBY=n','USE_KVM='+('y' if isa=='x86_64' else 'n'),'USE_SYSTEMC=y',*args],home)
    run([scons,target+'/gem5.opt',*args,'-j',SETTINGS['jobs']],home)
    if force or previous.get('device')!=selected['device'] or not outputs[-1].exists():
        ram=ROOT/'third_party/vortex/third_party/ramulator/build'
        if (ram/'CMakeCache.txt').exists() and str(ROOT/'third_party/.cache/tools') not in (ram/'CMakeCache.txt').read_text():shutil.rmtree(ram)
        run(['bash',RUNTIME/'build_device.sh',config])
    if force or refresh or host_previous!=host_selected or not all(p.exists() for p in outputs[1:3]):
        env=dict(os.environ,CONFIGS=simx_flags(c))
        for key in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','DEBUG'):env.pop(key,None)
        if isa=='aarch64':
            for key in ('CPATH','LIBRARY_PATH'):env.pop(key,None)
        for part in ('stub','gem5'):
            subprocess.run(['make','-B','-C',str(BUILD/'vortex/sw/runtime'/part),'HOST_ARCH='+isa,'-j',str(SETTINGS['jobs'])],env=env,check=True)
    record_build();dump(marker,selected);dump(host_marker,host_selected)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--config',required=True);p.add_argument('--force',action='store_true');p.add_argument('--ready',action='store_true');p.add_argument('--refresh',action='store_true');o=p.parse_args()
    if o.ready:raise SystemExit(0 if build(o.config,ready=True) else 1)
    build(o.config,o.force,refresh=o.refresh)
