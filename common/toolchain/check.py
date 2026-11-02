#!/usr/bin/env python3
"""Validate a project-owned Conda prefix against the explicit lock file."""
import argparse
import json
from pathlib import Path
import sys

def verify(prefix,lock):
    prefix=Path(prefix).resolve();lock=Path(lock)
    wanted={line.split('#',1)[0].strip() for line in lock.read_text().splitlines() if line.startswith('https:')}
    installed=set()
    for path in (prefix/'conda-meta').glob('*.json'):
        item=json.loads(path.read_text())
        url=item.get('url')
        if not url:raise ValueError('Package lacks original URL: '+str(path))
        installed.add(url.split('#',1)[0])
    if not wanted or installed!=wanted:
        raise ValueError('Toolchain differs from lock: missing='+str(sorted(wanted-installed))+' extra='+str(sorted(installed-wanted)))
    for name in ('python','cmake','x86_64-conda-linux-gnu-gcc','x86_64-conda-linux-gnu-g++'):
        if not (prefix/'bin'/name).is_file():raise ValueError('Missing tool '+name)
    return prefix

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('prefix',type=Path);p.add_argument('lock',type=Path);a=p.parse_args()
    try:verify(a.prefix,a.lock)
    except (OSError,ValueError) as error:print(str(error),file=sys.stderr);sys.exit(1)
