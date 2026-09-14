#!/usr/bin/env python3
"""Check pinned external dependencies and the monorepo source layout."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
expected = json.loads((root / "env/sources.lock.json").read_text())
for path, revision in expected.items():
    if not (root / path / ".git").exists():
        raise SystemExit(f"{path}: 子模块未初始化，请执行 git submodule update --init --recursive")
    actual = subprocess.check_output(
        ["git", "-C", str(root / path), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != revision:
        raise SystemExit(f"{path}: expected {revision}, found {actual}; no checkout performed")
    print(f"{path}: {actual}")

states = subprocess.check_output(
    ["git", "-C", str(root), "submodule", "status", "--recursive"], text=True
)
for state in states.splitlines():
    if state and state[0] != " ":
        raise SystemExit(
            "子模块未初始化、版本不匹配或存在冲突：" + state +
            "\n请核对本地修改后执行 git submodule update --init --recursive"
        )

for path in ("gem5_new", "axi2flit", "ucie-model", "mem_sim", "gem5_axi"):
    directory = root / path
    top = subprocess.check_output(["git", "-C", str(directory), "rev-parse", "--show-toplevel"], text=True).strip()
    if Path(top) != root or (directory / ".git").exists():
        raise SystemExit(f"{path}: expected ordinary source directory in {root}")
    print(f"{path}: monorepo source")
