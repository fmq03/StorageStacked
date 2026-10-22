"""Select and incrementally rebuild a bounded native RTL variant."""
import shutil
import os
import tempfile
from paths import ROOT, BUILD, dump
from configure import rtl_config, memory_layout
from native_build import bazel


def variant_name(c):
    n=c['npu']
    return f"i{n['itcm_kib']}-d{n['dtcm_kib']}-l0{int(n['fetch_l0_enabled'])}"


def model_name(c):
    n=c['npu'];base='StorageNpuL0'+('On' if n['fetch_l0_enabled'] else 'Off')
    if n['itcm_kib']==8:return base+'Axi'
    if n['itcm_kib']==1024:return base+'HighmemAxi'
    return base+f"_ITCM{n['itcm_kib']}KB_DTCM{n['dtcm_kib']}KBAxi"


def native_library(c):
    return BUILD/'rtl'/variant_name(c)/('libcoralnpu-'+variant_name(c)+'.so')


def copy_artifact(source, destination):
    descriptor,name=tempfile.mkstemp(prefix='.'+destination.name+'-',dir=destination.parent)
    os.close(descriptor)
    from pathlib import Path
    temporary=Path(name)
    try:
        shutil.copy2(source,temporary)
        os.replace(temporary,destination)
    finally:
        temporary.unlink(missing_ok=True)



def build_native(c):
    name=variant_name(c);model=model_name(c)
    target='//native:'+name+'.so'
    bazel('build',[target])
    directory=native_library(c).parent;directory.mkdir(parents=True,exist_ok=True)
    files={name+'.so':native_library(c).name,model+'.sv':'model.sv',
           'V'+model+'_parameters.h':'model_parameters.h'}
    source=ROOT/'coralnpu/bazel-bin/native'
    for original,destination in files.items():
        src=source/original;dst=directory/destination
        if not src.is_file():raise RuntimeError('Missing generated RTL artifact: '+original)
        copy_artifact(src,dst)
    dump(directory/'build.json',{'rtl':rtl_config(c),'layout':memory_layout(c),
                               'target':target,'model':model,
                               'files':[{'name':f,'bytes':(directory/f).stat().st_size} for f in files.values()]})
    if name=='i8-d32-l00':copy_artifact(native_library(c),BUILD/'libcoralnpu-native.so')
    print('RTL variant ready: '+name,flush=True)
    return native_library(c)
