#!/usr/bin/env python3
"""Record accelerator binaries and reject a second embedded SystemC runtime."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root=Path(os.environ['SS_ROOT']);deps=Path(os.environ['SS_DEPS_ROOT'])
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
paths={
    'vortex':root/'vortex-gpu/vxbuild/sim/simx/libvortex-gem5.so',
    'coralnpu':root/'coralnpu/bazel-bin/gem5int/libcoralnpu-gem5.so',
    'runtime':root/'vortex-gpu/vxbuild/sw/runtime/libvortex.so',
    'runtime_backend':root/'vortex-gpu/vxbuild/sw/runtime/libvortex-gem5-x86_64.so',
    'gpu_kernel':root/'vortex-gpu/vxbuild/tests/regression/vecadd/kernel.vxbin',
    'npu_kernel':root/'build/xpu/ddr_touch.elf',
    'gpu_host':root/'vortex-gpu/vxbuild/tests/regression/vecadd/vecadd',
    'npu_host':root/'gem5_new/workloads/shared_buffer/build/host_main',
    'three_host':root/'gem5_new/workloads/three_source/build/host_main',
}
files={}
for name,path in paths.items():
    item={'path':str(path),'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest()}
    if name in ('vortex','coralnpu','runtime','runtime_backend'):
        item['ldd']=subprocess.check_output(['ldd',str(path)],text=True)
        assert 'not found' not in item['ldd'] and 'libsystemc' not in item['ldd'].lower(),item
    if name in ('vortex','coralnpu'):
        # ldd alone cannot detect a statically linked copy.
        symbols=subprocess.check_output(['nm','-a','-C',str(path)],text=True)
        matches=[line for line in symbols.splitlines() if any(s in line for s in ('sc_core::','sc_dt::','sc_main'))]
        assert not matches,(name,matches[:10])
        assert symbols.strip(),'Audit needs unstripped libraries'
        item['embedded_systemc_symbols']=0
    files[name]=item
private=[json.loads(p.read_text()) for p in (deps/'xpu-sysroot/conda-meta').glob('*.json')]
expected={s for s in (root/'env/xpu-runtime-linux-64.lock').read_text().splitlines() if s.startswith('https://')}
assert expected=={p['url']+'#'+p['md5'] for p in private}
cmake_sources=[]
cmake_cache=(root/'vortex-gpu/vortex/third_party/ramulator/build/CMakeCache.txt').read_text()
simx_headers=(root/'vortex-gpu/vxbuild/sim/simx/obj/sim_common/dram_sim.d').read_text()
for item in json.loads((root/'env/xpu-artifacts.lock.json').read_text())['cmake_sources']:
    archive=deps/'xpu-downloads/cmake'/(item['name']+'-'+item['revision']+'.tar.gz')
    source=deps/'cmake-sources'/item['name']
    assert hashlib.file_digest(archive.open('rb'),'sha256').hexdigest()==item['sha256']
    assert (source/'.storagestacked-source.sha256').read_text().strip()==item['sha256']
    key='FETCHCONTENT_SOURCE_DIR_'+item['name'].upper()
    assert any(line.startswith(key+':') and line.split('=',1)[-1]==str(source)
               for line in cmake_cache.splitlines()), key
    if item['name'] in ('spdlog','yaml-cpp'):
        assert str(source/'include') in simx_headers, item['name']
    cmake_sources.append({**item,'source_directory':str(source)})
versions={}
for name,cmd in {
    'bazel':[str(deps/'xpu-tools/bin/bazel'),'--version'],
    'vortex_llvm':[str(deps/'xpu-toolchains/llvm-vortex/bin/clang++'),'--version'],
}.items():versions[name]=subprocess.check_output(cmd,text=True)
(out/'xpu_manifest.json').write_text(json.dumps({'passed':True,'files':files,'versions':versions,
    'cmake_sources':cmake_sources,
    'private_runtime_packages':[{k:p[k] for k in ('name','version','build','url','md5')} for p in private],
    'systemc':'Only gem5 native SystemC; accelerator libraries have no SystemC symbols'},indent=2)+'\n')
print('XPU binary and SystemC audit passed')
