"""Rebuild only the native model selected by one user configuration."""
import argparse
import fcntl
from pathlib import Path
from paths import CACHE,ROOT
from configure import load_config
from rtl_build import build_native

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--config',required=True)
o=p.parse_args()
config=Path(o.config)
if config.is_absolute():raise ValueError('--config must be relative to the repository root')
config=(ROOT/config).resolve();config.relative_to(ROOT/'user')
with (CACHE/'build.lock').open('a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX)
    build_native(load_config(config))
