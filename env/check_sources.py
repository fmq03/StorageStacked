#!/usr/bin/env python3
"""Check pinned external dependencies and the monorepo source layout."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
expected = json.loads((root / "env/sources.lock.json").read_text())
for path, revision in expected.items():
    actual = subprocess.check_output(
        ["git", "-C", str(root / path), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != revision:
        raise SystemExit(f"{path}: expected {revision}, found {actual}; no checkout performed")
    print(f"{path}: {actual}")

for path in ("gem5_new", "axi2flit", "ucie-model", "mem_sim", "gem5_axi"):
    directory = root / path
    top = subprocess.check_output(["git", "-C", str(directory), "rev-parse", "--show-toplevel"], text=True).strip()
    if Path(top) != root or (directory / ".git").exists():
        raise SystemExit(f"{path}: expected ordinary source directory in {root}")
    print(f"{path}: monorepo source")
