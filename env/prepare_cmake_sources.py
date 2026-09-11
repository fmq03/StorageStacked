#!/usr/bin/env python3
"""Restore Ramulator's pinned build dependencies from original source archives."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

deps = Path(os.environ['SS_DEPS_ROOT'])
cache = deps / 'xpu-downloads/cmake'
sources = deps / 'cmake-sources'
cache.mkdir(parents=True, exist_ok=True)
sources.mkdir(parents=True, exist_ok=True)
lock = json.loads((Path(__file__).parent / 'xpu-artifacts.lock.json').read_text())
for item in lock['cmake_sources']:
    archive = cache / (item['name'] + '-' + item['revision'] + '.tar.gz')
    if not archive.exists():
        if os.environ.get('SS_OFFLINE') == '1':
            raise RuntimeError('离线缓存缺少 ' + str(archive))
        env = dict(os.environ)
        env.pop('LD_LIBRARY_PATH', None)
        subprocess.run(['curl', '-fsSL', '--retry', '3', '--max-time', '180',
                        item['url'], '-o', str(archive)], check=True, env=env)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != item['sha256']:
        raise RuntimeError('Source archive checksum mismatch: ' + str(archive))
    target = sources / item['name']
    marker = target / '.storagestacked-source.sha256'
    if target.exists():
        if not marker.exists() or marker.read_text().strip() != item['sha256']:
            raise RuntimeError('Existing source cache differs from lock: ' + str(target))
        continue
    with tempfile.TemporaryDirectory(dir=sources) as temporary:
        with tarfile.open(archive) as source:
            source.extractall(temporary, filter='data')
        (Path(temporary) / (item['name'] + '-' + item['revision'])).rename(target)
    marker.write_text(item['sha256'] + '\n')
    print('Prepared CMake source:', item['name'], item['revision'])
