#!/usr/bin/env bash
set -euo pipefail
bash "$(dirname -- "${BASH_SOURCE[0]}")/bootstrap.sh"
source "$(dirname -- "${BASH_SOURCE[0]}")/activate.sh"
mkdir -p "$SS_DEPS_ROOT/xpu-tools/bin" "$SS_DEPS_ROOT/xpu-downloads" "$SS_DEPS_ROOT/xpu-native"
"$AXI_PYTHON" - <<'PY'
import hashlib,json,os,subprocess
from pathlib import Path
root=Path(os.environ['SS_ROOT']);deps=Path(os.environ['SS_DEPS_ROOT'])
lock=json.loads((root/'env/xpu-artifacts.lock.json').read_text())
for name,rel in [('bazel-8.6.0-linux-x86_64','xpu-tools/bin/bazel'),('lz4-1.10.0.tar.gz','xpu-downloads/lz4-1.10.0.tar.gz')]:
    path=deps/rel;item=lock[name]
    if not path.exists():
        if os.environ.get('SS_OFFLINE')=='1':raise RuntimeError('离线缓存缺少 '+str(path))
        env=dict(os.environ);env.pop('LD_LIBRARY_PATH',None)
        subprocess.run(['curl','-fsSL','--max-time','180','--retry','3',item['url'],'-o',str(path)],check=True,env=env)
    if hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:raise RuntimeError('Checksum mismatch: '+str(path))
(deps/'xpu-tools/bin/bazel').chmod(0o755)
PY
if [[ ! -d "$SS_DEPS_ROOT/xpu-sysroot/conda-meta" ]]; then
    offline_args=()
    [[ ${SS_OFFLINE:-0} != 1 ]] || offline_args+=(--offline)
    "$SS_DEPS_ROOT/bootstrap/bin/micromamba" --no-rc create -y --root-prefix "$SS_DEPS_ROOT/mamba" \
        -p "$SS_DEPS_ROOT/xpu-sysroot" --file "$SS_ROOT/env/xpu-runtime-linux-64.lock" "${offline_args[@]}"
fi
"$AXI_PYTHON" - <<'PY'
import json,os
from pathlib import Path
root=Path(os.environ['SS_ROOT']);deps=Path(os.environ['SS_DEPS_ROOT'])
expected={s for s in (root/'env/xpu-runtime-linux-64.lock').read_text().splitlines() if s.startswith('https://')}
packages=[json.loads(p.read_text()) for p in (deps/'xpu-sysroot/conda-meta').glob('*.json')]
assert expected=={p['url']+'#'+p['md5'] for p in packages}, 'Private XPU runtime differs from lock'
PY
"$AXI_PYTHON" "$SS_ROOT/env/fetch_vortex_tools.py"
"$AXI_PYTHON" "$SS_ROOT/env/prepare_vortex_llvm.py"
"$AXI_PYTHON" "$SS_ROOT/env/prepare_cmake_sources.py"
cd "$SS_DEPS_ROOT/xpu-downloads"
[[ -d lz4-1.10.0 ]] || tar -xf lz4-1.10.0.tar.gz
make -C lz4-1.10.0/lib -j4 CC="$AXI_CC" PREFIX="$SS_DEPS_ROOT/xpu-native" install
printf 'XPU 工具环境已就绪：%s\n' "$SS_DEPS_ROOT"
