"""Repository-relative configuration; absolute paths are temporary process state only."""
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'coralnpu/runtime'
CACHE = ROOT / 'coralnpu/.cache'
BUILD = CACHE / 'build'
SETTINGS = json.loads((RUNTIME / 'paths.json').read_text())
if Path(SETTINGS['storage']).is_absolute():
    raise ValueError('storage must be relative to the repository root')
STORAGE_ROOT = (ROOT / SETTINGS['storage']).resolve()
MEMSIM_BUILD = BUILD / 'memsim'

def relative(path, base=ROOT):
    return os.path.relpath(path, base)

def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')

import runpy
_STORAGE = runpy.run_path(str(STORAGE_ROOT/'scripts/storage_manifest.py'))['load'](STORAGE_ROOT)
STORAGE_SOURCE = _STORAGE['storage_source']
STORAGE_MEMSIM_SOURCE = _STORAGE['memsim_source']
STORAGE_MEMSIM_ONLINE_CHECK = _STORAGE['memsim_online_check']
STORAGE_CHECKS = _STORAGE['checks_dir']
STORAGE_PYTHON_PATH = _STORAGE['python_path']
STORAGE_INCLUDE_DIRS = _STORAGE['include_dirs']

STORAGE_RESULT_ROUTES = _STORAGE['result_routes']
