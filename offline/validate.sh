#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$here/validation"
validation=${STORAGE_VALIDATION_DIR:-$here/validation/offline-20261009-burst}
mkdir -p "$validation"
run_results="$here/results/check-$(date +%Y%m%d-%H%M%S)-$$"
export STORAGE_RESULTS_DIR="$run_results"
"$here/run.sh" check > "$validation/check.log" 2>&1
"$here/run.sh" test > "$validation/test.log" 2>&1
python3 - "$here" "$run_results" "$validation" <<'PY'
import json,sys
from pathlib import Path
here=Path(sys.argv[1]); result_root=Path(sys.argv[2]); validation=Path(sys.argv[3]); summaries=[]
for p in result_root.glob('*/summary.json'):
    s=json.loads(p.read_text())
    if s['command'] == 'test':
        assert s['passed'] is True,p
        summaries.append({'path':str(p.relative_to(here)),'command':s['command'],'cases':s['cases']})
assert len(summaries)==1
(validation/'acceptance.json').write_text(json.dumps({'passed':True,'network':'none','pull':'never','host_mounts':['results only'],'runs':summaries},ensure_ascii=False,indent=2)+'\n')
print('PASS：工具检查及一次完整回归；导出脚本另行验证归档首次导入与默认入口。')
PY
