#!/usr/bin/env bash
set -euo pipefail

demo_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
demo_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
demo_tmp=$(mktemp -d)
trap 'rm -rf -- "$demo_tmp"' EXIT

for demo_standard in hbm3 hbm4 lpddr5 lpddr6; do
  HBM_SIM_BIN="$demo_bin" OUTPUT_ROOT="$demo_tmp" \
    bash "$demo_source/examples/multistack_demos/${demo_standard}_nstack.sh" \
    >"$demo_tmp/${demo_standard}.log"
  python3 - "$demo_source" "$demo_tmp/${demo_standard}_2stack/result.json" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / 'tools'))
from result_io import read_result, count
r = read_result(Path(sys.argv[2]), require_completed=True)
assert count(r, 'stack_count') == 2
assert count(r, 'data_mismatches') == 0
for i in range(2):
    assert count(r, f'stack_{i}_reads') + count(r, f'stack_{i}_writes') > 0
PY
  test -s "$demo_tmp/${demo_standard}_2stack/resolved.cfg"
  test -s "$demo_tmp/${demo_standard}_2stack/dashboard.html"
  grep -F '"stack":1' "$demo_tmp/${demo_standard}_2stack/dashboard.html" >/dev/null
  grep -F 'id="thermalStack"' "$demo_tmp/${demo_standard}_2stack/dashboard.html" >/dev/null
done

echo "four multistack demos passed"
