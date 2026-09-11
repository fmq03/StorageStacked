#!/usr/bin/env python3
"""Give the fixed LLVM binaries their private glibc loader, without global preload."""
import os
from pathlib import Path
import subprocess
root=Path(os.environ['SS_DEPS_ROOT'])
llvm=root/'xpu-toolchains/llvm-vortex'
libc=root/'xpu-sysroot/x86_64-conda-linux-gnu/sysroot/lib64'
patch=root/'xpu-sysroot/bin/patchelf'
env=dict(os.environ);env.pop('LD_LIBRARY_PATH',None)
seen=set()
for path in (llvm/'bin').iterdir():
    p=path.resolve()
    if not p.is_file() or p in seen:continue
    seen.add(p)
    with p.open('rb') as f:
        if f.read(4)!=b'\x7fELF':continue
    probe=subprocess.run([str(patch),'--print-interpreter',str(p)],env=env,capture_output=True)
    if probe.returncode:continue
    subprocess.run([str(patch),'--set-interpreter',str(libc/'ld-linux-x86-64.so.2'),
                    '--force-rpath','--set-rpath','$ORIGIN/../lib:'+str(libc)+':'+os.environ['SS_PREFIX']+'/lib',str(p)],check=True,env=env)
print('Prepared LLVM with private glibc:',libc)
