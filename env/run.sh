#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/activate.sh"
[[ -x "$AXI_GEM5_BIN" ]] || { echo '请先执行 bash env/build.sh' >&2; exit 1; }
destination=${1:-"$SS_ROOT/results/unified-$(date -u +%Y%m%dT%H%M%SZ)"}
if [[ -e "$destination" ]]; then
    echo "结果目录已存在，请使用新目录：$destination" >&2
    exit 1
fi
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
"$AXI_PYTHON" "$SS_ROOT/env/check_sources.py"
"$AXI_PYTHON" "$SS_ROOT/env/record.py" "$destination/environment"
bash "$AXI_PROJECT_DIR/scripts/run_aou_tests.sh" "$destination/aou" --het-trace
bash "$AXI_PROJECT_DIR/scripts/run_tests.sh" "$destination/ram"
for latency in 3 9; do
    case_dir="$destination/feedback/cpu_l$latency"
    mkdir -p "$case_dir"
    "$AXI_GEM5_BIN" --listener-mode=off -d "$case_dir" "$AXI_PROJECT_DIR/configs/run.py" \
        --backend aou --het-trace --mode cpu --binary "$AXI_PROJECT_DIR/build/memory_check" \
        --no-stalls --latency "$latency" > "$case_dir/run.log" 2>&1
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$case_dir"
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check_aou.py" "$case_dir"
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/inspect_link.py" "$case_dir"
done
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/audit_wave.py" \
    "$destination/feedback/cpu_l3" "$destination/feedback/cpu_l9" \
    --output "$destination/feedback/wave_audit"
mkdir -p "$destination/observer_off"
"$AXI_GEM5_BIN" --listener-mode=off -d "$destination/observer_off" "$AXI_PROJECT_DIR/configs/run.py" \
    --backend aou --mode tester > "$destination/observer_off/run.log" 2>&1
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$destination/observer_off" --directed
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check_aou.py" "$destination/observer_off"
"$AXI_PYTHON" "$SS_ROOT/env/verify_run.py" "$destination"
echo "全流程校验通过：$destination/summary.json"
