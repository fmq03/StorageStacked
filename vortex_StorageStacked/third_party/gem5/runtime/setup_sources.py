#!/usr/bin/env python3
"""Verify the source snapshots stored directly in this repository."""
from storage_dependency import STORAGE_ROOT, MEMSIM_BUILD, storage_version
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
entrypoints = {"gem5": "SConstruct", "vortex": "configure", "coralnpu": "WORKSPACE"}
for name, info in json.loads((ROOT / "third_party/gem5/runtime/defaults/sources.json").read_text()).items():
    if name == "project":
        continue
    path = ROOT / "third_party" / name
    if not (path / entrypoints[name]).is_file():
        raise RuntimeError(f"Incomplete source checkout: {path}; restore it from this repository")
    if (path / ".git").exists() or (path / ".gitmodules").exists():
        raise RuntimeError(f"Source must be ordinary tracked files, without nested Git metadata: {path}")
    marker = path / ".source-version"
    if not marker.is_file() or marker.read_text().strip() != info["commit"]:
        raise RuntimeError(f"Source version marker differs from config/sources.json: {name}")
print("Vendored source snapshots are ready; no recursive Git checkout required")

print('Storage dependency:', storage_version())
