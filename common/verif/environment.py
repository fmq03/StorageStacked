"""Record tool and kernel identities separately from machine-specific paths."""
import hashlib
import os
from pathlib import Path
import platform
import re
import subprocess

def version(command):
    return subprocess.check_output([str(command),'--version'],text=True,stderr=subprocess.STDOUT).splitlines()[0]

def describe(cache=None,bundled=None,gem5=None,compiler=None,standard=None):
    values={}
    if cache and not Path(cache).is_file():
        cache=next((parent/'CMakeCache.txt' for parent in Path(cache).parent.parents if (parent/'CMakeCache.txt').is_file()),None)
    if cache and Path(cache).is_file():
        for line in Path(cache).read_text().splitlines():
            if '=' in line and ':' in line.split('=',1)[0]:
                key,value=line.split('=',1);values[key.split(':',1)[0]]=value
    compiler=values.get('CMAKE_CXX_COMPILER') or compiler or os.environ.get('CXX') or 'c++'
    standard=str(values.get('STORAGE_CXX_STANDARD') or standard or 17)
    prefix=values.get('STORAGE_SYSTEMC_PREFIX')
    if gem5:
        header=Path(gem5)/'src/systemc/ext/utils/functions.hh'
        text=header.read_text();match=re.search(r'^#define SC_VERSION "([^"]+)"',text,re.M)
        systemc=dict(implementation='gem5',version=match.group(1) if match else 'unknown',cxx_standard=standard)
        paths=dict(headers=str(header.parent.parent),source=str(Path(gem5)/'src/systemc'))
    else:
        selected=values.get('STORAGE_SYSTEMC_INCLUDE') if (prefix or values.get('STORAGE_SYSTEMC_TARGET')) else bundled
        include=Path(selected) if selected else None
        header=include/'sysc/kernel/sc_ver.h' if include else None
        kernel_version='unknown'
        if header and header.is_file():
            text=header.read_text()
            parts=[re.search(r'^#define\s+SC_VERSION_'+part+r'\s+(\d+)',text,re.M).group(1) for part in ('MAJOR','MINOR','PATCH')]
            kernel_version='.'.join(parts)
        systemc=dict(implementation='Accellera' if header and header.is_file() else 'host-target',
                     version=kernel_version,cxx_standard=standard)
        paths=dict(headers=str(include) if include else 'provided by host target',
                   source=str(include.parent) if include else 'provided by host target',
                   library=values.get('STORAGE_SYSTEMC_LIBRARY') or values.get('STORAGE_SYSTEMC_TARGET') or 'bundled storage_systemc target')
    repo=Path(__file__).resolve().parents[2]
    lock=repo/'common/toolchain/conda-linux-64.lock'
    return dict(toolchain=dict(compiler=version(compiler),cmake=version('cmake'),python=platform.python_version(),
                               lock_sha256=hashlib.sha256(lock.read_bytes()).hexdigest()),
                systemc=systemc,systemc_paths=paths,compiler_path=str(compiler),
                systemc_macros=['SC_INCLUDE_FX','SC_INCLUDE_DYNAMIC_PROCESSES'] if not gem5 else [])
