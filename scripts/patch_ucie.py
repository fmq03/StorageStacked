#!/usr/bin/env python3
"""Apply the AXI2Flit project's required AoU patch, without staging or committing."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
repo = root / 'ucie-model'
patch = root / 'axi2flit/systemc/integration/ucie-model-aou.patch'
def git(*args):
    return subprocess.run(['git', '-C', str(repo), 'apply', *args, str(patch)], capture_output=True, text=True)
if git('--reverse', '--check').returncode == 0:
    print('UCIe AoU patch already present')
else:
    check = git('--check')
    if check.returncode:
        raise SystemExit('UCIe patch conflicts with working tree; no files changed:\n' + check.stderr)
    result = git()
    if result.returncode:
        raise SystemExit(result.stderr)
    print('Applied UCIe AoU patch (uncommitted)')

patch = root / 'gem5_axi/patches/ucie_observer.patch'
if git('--reverse', '--check').returncode == 0:
    print('UCIe passive observer patch already present')
else:
    check = git('--check')
    if check.returncode:
        raise SystemExit('Observer patch conflicts; no files changed:\n' + check.stderr)
    result = git()
    if result.returncode:
        raise SystemExit(result.stderr)
    print('Applied UCIe passive observer patch (uncommitted)')
