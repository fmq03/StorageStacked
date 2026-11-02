#!/usr/bin/env bash
set -euo pipefail

sweep_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
sweep_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
sweep_tmp=$(mktemp -d)
trap 'rm -rf -- "$sweep_tmp"' EXIT

# --density-requests 固定小值保持 smoke 快；密度组因此走“组织固定”与
# “行为不变”分支，真正的 bank 数递增由默认值下的 HBM4 覆盖。
python3 "$sweep_source/experiments/architecture_sweep/run.py" \
  --binary "$sweep_bin" --standards hbm4,lpddr6 --requests 64 \
  --density-requests 64 --inject-interval 2 --out "$sweep_tmp" >/dev/null

test -s "$sweep_tmp/results.csv"
test -s "$sweep_tmp/checks.csv"
test -s "$sweep_tmp/summary.md"
test -s "$sweep_tmp/trends.html"
# 每标准 9 个 case（3 密度 + 2 刷新 + 4 组织扰动），加表头。
test "$(wc -l < "$sweep_tmp/results.csv")" -eq 19
grep -q 'density,32gb_8h' "$sweep_tmp/results.csv"
grep -q 'density,24gb_12h' "$sweep_tmp/results.csv"
grep -q 'density,8gb_sc' "$sweep_tmp/results.csv"
grep -q 'refresh,all_bank' "$sweep_tmp/results.csv"
grep -q 'organization,bpg_double' "$sweep_tmp/results.csv"
grep -q 'organization,cols_double' "$sweep_tmp/results.csv"
grep -q 'organization,mixed_shift' "$sweep_tmp/results.csv"
grep -q 'PASS,density scaling: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,density scaling: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,refresh scope: LPDDR6' "$sweep_tmp/checks.csv"
# 容量核算是验收标准，必须进回归保护：Python 按容量公式算出的组织容量
# 要等于引擎报告的 aggregate_capacity_bytes。
grep -q 'PASS,capacity: HBM4 ' "$sweep_tmp/checks.csv"
grep -q 'PASS,capacity: LPDDR6 ' "$sweep_tmp/checks.csv"
grep -q 'PASS,organization drift: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,organization drift: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,baseline stability: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,baseline stability: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,density behavior invariance: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,density behavior invariance: LPDDR6' "$sweep_tmp/checks.csv"
grep -q 'PASS,address encoding: HBM4' "$sweep_tmp/checks.csv"
grep -q 'PASS,address encoding: LPDDR6' "$sweep_tmp/checks.csv"
if grep -q '^FAIL,' "$sweep_tmp/checks.csv"; then
  echo "architecture sweep contains a failed automated check" >&2
  exit 1
fi
test -s "$sweep_tmp/hbm4/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/hbm4/density_32gb_8h/workload.trace"
test -s "$sweep_tmp/hbm4/organization_bpg_double/resolved.cfg"
test -s "$sweep_tmp/hbm4/organization_mixed_shift/workload.trace"
test -s "$sweep_tmp/hbm4/baseline.cfg"
test -s "$sweep_tmp/lpddr6/refresh_all_bank/resolved.cfg"
test -s "$sweep_tmp/lpddr6/density_8gb_sc/workload.trace"
test -s "$sweep_tmp/lpddr6/organization_bpg_double/resolved.cfg"
test -s "$sweep_tmp/lpddr6/organization_mixed_shift/workload.trace"
test -s "$sweep_tmp/lpddr6/baseline.cfg"

# 密度组的第二个请求应落在 bank=1：columns × 32 B。HBM 的 columns=32 -> 0x400，
# LPDDR 的 columns=64 -> 0x800。这个按标准区分的结果本身就是“密度组尊重标准
# 列宽”的断言。fixture 必须收敛 lane，否则请求 #1 会落到另一条 lane 而不是
# 另一个 bank。
python3 - "$sweep_source/experiments/architecture_sweep/run.py" "$sweep_tmp" <<'PY'
import pathlib
import runpy
import sys

module = runpy.run_path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
expected = {"hbm3": "0 R 0x400", "hbm4": "0 R 0x400",
            "lpddr5": "0 R 0x800", "lpddr6": "0 R 0x800"}
for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
    assert module["transaction_bytes"](standard) == 32
    # 与组织无关的事务粒度断言：column 前进一格就是一条 32 B 事务。
    assert module["default_address"](
        row=0, column=1, bank=0, bank_group=0,
        org=module["STANDARD_ORGS"][standard],
        tx_bytes=module["transaction_bytes"](standard)) == 32
    case = module["Case"]("density", "unit", {
        "channels": 1, "pseudo_channels": 1, "sids": 1, "ranks": 1,
        "bank_groups": 2, "banks_per_group": 4})
    trace = root / f"{standard}_address_unit.trace"
    module["write_trace"](trace, standard, case, 2, 2)
    assert trace.read_text().splitlines()[1] == expected[standard], standard
PY

echo "architecture sweep smoke passed"
