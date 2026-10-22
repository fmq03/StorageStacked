#!/usr/bin/env bash
set -euo pipefail
storage= jobs= test=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --storage) storage=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --test) test=1; shift ;;
        --help|-h) echo '用法: ./build.sh [--storage ../axi_StorageStacked] [--jobs 12] [--test]'; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done
python3 - "$storage" "$jobs" <<'PY'
from pathlib import Path
import json,sys,shutil
p=Path('coralnpu/runtime/paths.json');c=json.loads(p.read_text());old_storage=c['storage']
if sys.argv[1]:
    if Path(sys.argv[1]).is_absolute():raise SystemExit('--storage 必须是相对于仓库根目录的路径')
    c['storage']=sys.argv[1]
if sys.argv[2]:
    c['jobs']=int(sys.argv[2])
    if not 1<=c['jobs']<=128:raise SystemExit('--jobs 必须在 1..128')
if not (Path(c['storage'])/'storage_axi/storage_config.hh').is_file():raise SystemExit('公共存储项目不存在，请检查 --storage 相对路径')
p.write_text(json.dumps(c,indent=2)+'\n')
Path('docs').mkdir(exist_ok=True)
cache=Path('coralnpu/.cache');cache.mkdir(exist_ok=True)
if old_storage!=c['storage'] and (cache/'build').exists():shutil.rmtree(cache/'build')
location=cache/'location'
# Tool-generated caches contain absolute paths; only these disposable caches
# are invalidated when the checkout is physically relocated.
if location.exists() and location.read_text()!=str(Path.cwd()):
    for name in ('build','tools/toolchain'):
        target=cache/name
        if target.exists():shutil.rmtree(target)
location.write_text(str(Path.cwd()))
PY
bash coralnpu/runtime/bootstrap.sh
source coralnpu/runtime/environment.sh
exec "$AXI_PYTHON" coralnpu/runtime/build.py "$test"
