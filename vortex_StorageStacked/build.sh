#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
storage= jobs= test=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --storage|--jobs)
            [[ $# -ge 2 ]] || { echo "$1 缺少参数" >&2; exit 2; }
            if [[ $1 == --storage ]]; then storage=$2; else jobs=$2; fi
            shift 2 ;;
        --test) test=1; shift ;;
        -h|--help)
            echo '用法：./build.sh [--storage ../axi_StorageStacked] [--jobs 12] [--test]'
            echo '安装内部工具、构建 gem5/Vortex/存储平台和两个示例；随后在 user/ 执行 ./run.sh smoke 或 ./run.sh llm。'
            exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done
python3 - "$storage" "$jobs" <<'PY'
import json,sys
from pathlib import Path
p=Path('third_party/gem5/runtime/paths.json');c=json.loads(p.read_text())
if sys.argv[1]:
    if Path(sys.argv[1]).is_absolute():raise SystemExit('--storage 必须是相对于仓库根目录的路径')
    if not (Path(sys.argv[1])/'storage_axi/storage_config.hh').is_file():raise SystemExit('找不到公共 AXI 存储项目')
    c['storage']=sys.argv[1]
if sys.argv[2]:
    n=int(sys.argv[2])
    if not 1<=n<=256:raise SystemExit('--jobs 范围为 1..256')
    c['jobs']=n
p.write_text(json.dumps(c,indent=2)+'\n')
PY
mkdir -p third_party/.cache
exec 9>third_party/.cache/build.lock
flock -x 9
bash third_party/gem5/runtime/setup.sh
source third_party/gem5/runtime/environment.sh
"$AXI_PYTHON" third_party/gem5/runtime/build.py --config user/smoke/config.json --force
for project in smoke llm; do
    if [[ $project != smoke ]]; then
        "$AXI_PYTHON" third_party/gem5/runtime/build.py --config "user/$project/config.json" --refresh
    fi
    make -C "user/$project/src" CONFIG=../config.json OUT=../result/build
 done
flock -u 9
if [[ $test == 1 ]]; then exec "$AXI_PYTHON" third_party/gem5/runtime/test.py; fi
printf '构建完成。运行：cd user && ./run.sh smoke\n'
