"""Reproduce the compiler failure and verify fallback without loosening seccomp."""
import json
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
image = 'zhongxing-storagestacked:offline-20261009-burst'
probe = '''set -euo pipefail
source "$STORAGE_WORKSPACE/vortex_StorageStacked/third_party/gem5/runtime/environment.sh"
printf '.text\\n.globl _start\\n_start:\\n  nop\\n' >/tmp/probe.S
"$SS_DEPS_ROOT/xpu-toolchains/llvm-vortex/bin/clang++" --target=riscv32 -march=rv32imf -mabi=ilp32f -c /tmp/probe.S -o /tmp/probe.o
test -s /tmp/probe.o
'''
denial = '''import ctypes, errno
libc = ctypes.CDLL(None, use_errno=True)
assert libc.syscall(435, 0, 0) == -1
assert ctypes.get_errno() == errno.ENOSYS
ctypes.set_errno(0)
assert libc.getpriority(0, 0) == -1
assert ctypes.get_errno() == errno.EPERM
print('clone3 ENOSYS; unrelated getpriority EPERM preserved')
'''

with tempfile.TemporaryDirectory(prefix='docker-compat-') as tmp:
    root = Path(tmp)
    profile = root/'policy.json'
    profile.write_text(json.dumps({'defaultAction': 'SCMP_ACT_ALLOW', 'syscalls': [
        {'names': ['clone3', 'getpriority'], 'action': 'SCMP_ACT_ERRNO', 'errnoRet': 1}]}))
    (root/'probe.sh').write_text(probe)
    (root/'denial.py').write_text(denial)
    base = ['docker', 'run', '--rm', '--pull=never', '--network=none',
            '--mount', f'type=bind,source={root},target=/probe,readonly']
    old = ['--security-opt', f'seccomp={profile}']
    original = ['--entrypoint', '/bin/bash', image, '/probe/probe.sh']
    fixed = ['--mount', f'type=bind,source={here}/docker-compat-entrypoint.py,target=/compat.py,readonly',
             '--entrypoint', '/usr/local/bin/python3', image, '/compat.py', 'shell']
    cases = [
        ('original_default', base + original, 0),
        ('original_clone3_eperm', base + old + original, 1),
        ('fixed_clone3_eperm', base + old + fixed + ['/probe/probe.sh'], 0),
        ('fixed_default', base + fixed + ['/probe/probe.sh'], 0),
        ('previous_denials_preserved', base + old + fixed + ['-c',
         '/usr/local/bin/python3 /probe/denial.py'], 0),
    ]
    results = []
    for name, command, expected in cases:
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        output = result.stdout + result.stderr
        assert result.returncode == expected, (name, result.returncode, output)
        if name == 'original_clone3_eperm':
            assert 'posix_spawn failed: Operation not permitted' in output, output
        results.append({'case': name, 'exit_code': result.returncode, 'output': output})
    print(json.dumps({'passed': True, 'image': image, 'cases': results}, indent=2))
