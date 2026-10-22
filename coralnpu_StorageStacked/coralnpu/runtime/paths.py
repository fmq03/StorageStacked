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
