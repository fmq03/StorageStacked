#!/usr/bin/env python3
"""Fetch the fixed Vortex v3.0 toolchain into a project-owned directory."""
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,os
from pathlib import Path
import subprocess,tarfile
root=Path(os.environ['SS_DEPS_ROOT'])
cache=root/'xpu-downloads/vortex';cache.mkdir(parents=True,exist_ok=True)
dest=root/'xpu-toolchains';dest.mkdir(exist_ok=True)
lock=json.loads((Path(__file__).parent/'xpu-artifacts.lock.json').read_text())['vortex']
expected={r['url']:r['sha256'] for r in lock}
revision='86c525b57eecc4fc44b9a4494db02771ea779b52'
base='https://raw.githubusercontent.com/vortexgpgpu/vortex-toolchain-prebuilt/'+revision+'/'
groups={
'llvm-vortex':[f'llvm-vortex/ubuntu/focal/llvm-vortex.tar.bz2.parta{x}' for x in 'abc'],
'riscv32-gnu-toolchain':[f'riscv32-gnu-toolchain/ubuntu/focal/riscv32-gnu-toolchain.tar.bz2.parta{x}' for x in 'abcdefghijk'],
'libc32':['libc32/libc32.tar.bz2'],'libcrt32':['libcrt32/libcrt32.tar.bz2']}
def fetch(rel):
    p=cache/Path(rel).name
    if not p.exists():
        if os.environ.get('SS_OFFLINE')=='1':raise RuntimeError('离线缓存缺少 '+str(p))
        tmp=p.with_name(p.name+'.download')
        env=dict(os.environ);env.pop('LD_LIBRARY_PATH',None)
        subprocess.run(['curl','-fsSL','--connect-timeout','20','--max-time','180','--retry','3',base+rel,'-o',str(tmp)],check=True,env=env)
        tmp.rename(p)
    digest=hashlib.sha256(p.read_bytes()).hexdigest()
    if digest!=expected[base+rel]:raise RuntimeError('Toolchain checksum mismatch: '+str(p))
    return {'url':base+rel,'sha256':digest,'bytes':p.stat().st_size}
with ThreadPoolExecutor(max_workers=4) as pool:
    records=list(pool.map(fetch,[f for files in groups.values() for f in files]))
(cache/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
for name,files in groups.items():
    if (dest/name).exists():continue
    archive=cache/(name+'.joined.tar.bz2')
    with archive.open('wb') as out:
        for f in files:
            with (cache/Path(f).name).open('rb') as src:
                while block:=src.read(1024*1024):out.write(block)
    with tarfile.open(archive) as t:t.extractall(dest,filter='data')
    print('Installed',name,flush=True)
