#!/usr/bin/env bash
set -euo pipefail

sweep_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
sweep_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
sweep_tmp=$(mktemp -d)
trap 'rm -rf -- "$sweep_tmp"' EXIT

python3 "$sweep_source/experiments/architecture_sweep/run.py" \
  --binary "$sweep_bin" --standards hbm4,lpddr6 --requests 64 --inject-interval 2 \
  --out "$sweep_tmp" >/dev/null

test -s "$sweep_tmp/results.csv"
test -s "$sweep_tmp/checks.csv"
test -s "$sweep_tmp/summary.md"
test -s "$sweep_tmp/trends.html"
test "$(wc -l < "$sweep_tmp/results.csv")" -eq 17
grep -q 'bank,bg2_bank4' "$sweep_tmp/results.csv"
grep -q 'geometry,row131072_col2048' "$sweep_tmp/results.csv"
grep -q 'refresh,all_bank' "$sweep_tmp/results.csv"
grep -q 'PASS,bank scaling: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,geometry trend: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,bank scaling: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,geometry trend: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: LPDDR6' "$sweep_tmp/checks.csv"
if grep -q '^FAIL,' "$sweep_tmp/checks.csv"; then
  echo "architecture sweep contains a failed automated check" >&2
  exit 1
fi
test -s "$sweep_tmp/hbm4/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/hbm4/geometry_row32768_col512/workload.trace"
test -s "$sweep_tmp/lpddr6/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/lpddr6/geometry_row32768_col512/workload.trace"

# 第二个 bank case 请求应落在 bank=1,column=0：
# 64 columns * 32 B/transaction = 0x800。该断言防止把 64 B host line
# 错当成地址解码的 transaction 粒度，尤其覆盖 HBM3/LPDDR5。
python3 - "$sweep_source/experiments/architecture_sweep/run.py" "$sweep_tmp" <<'PY'
import pathlib
import runpy
import sys

module = runpy.run_path(sys.argv[1])
case = module["Case"]("bank", "unit", {"bank_groups": 2, "banks_per_group": 4})
root = pathlib.Path(sys.argv[2])
for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
    assert module["transaction_bytes"](standard) == 32
    trace = root / f"{standard}_address_unit.trace"
    module["write_trace"](trace, standard, case, 2, 2)
    assert trace.read_text().splitlines()[1] == "0 R 0x800"
PY

echo "architecture sweep smoke passed"
