#!/usr/bin/env python3
"""Restore Ramulator's pinned build dependencies from original source archives."""
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
lock = json.loads((Path(__file__).parent / 'defaults/xpu-artifacts.lock.json').read_text())
for item in lock['cmake_sources']:
    target = sources / item['name']
    marker = target / '.storagestacked-source-revision'
    if target.is_dir() and marker.exists() and marker.read_text().strip() == item['revision']:
        continue
    archive = cache / (item['name'] + '-' + item['revision'] + '.tar.gz')
    if not archive.exists():
        if os.environ.get('SS_OFFLINE') == '1':
            raise RuntimeError('离线缓存缺少 ' + str(archive))
        env = dict(os.environ)
        env.pop('LD_LIBRARY_PATH', None)
        subprocess.run(['curl', '-fsSL', '--retry', '3', '--max-time', '180',
                        item['url'], '-o', str(archive)], check=True, env=env)
    if archive.stat().st_size != item['bytes']:
        raise RuntimeError('Source archive size mismatch: ' + str(archive))
    target = sources / item['name']
    marker = target / '.storagestacked-source-revision'
    if target.exists():
        if not marker.exists() or marker.read_text().strip() != item['revision']:
            raise RuntimeError('Existing source cache differs from lock: ' + str(target))
        continue
    with tempfile.TemporaryDirectory(dir=sources) as temporary:
        with tarfile.open(archive) as source:
            source.extractall(temporary, filter='data')
        (Path(temporary) / (item['name'] + '-' + item['revision'])).rename(target)
    marker.write_text(item['revision'] + '\n')
    print('Prepared CMake source:', item['name'], item['revision'])
