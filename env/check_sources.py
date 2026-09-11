#!/usr/bin/env python3
"""Check source identities without requiring integration patches to be committed."""
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
