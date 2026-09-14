#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
mkdir -p "$AXI_ENV_ROOT" "$AXI_PROJECT_DIR/build"
cpu_results=$(mktemp -d "$AXI_ENV_ROOT/cpu-timing.XXXXXX")
echo "Working results: $cpu_results"
"${AXI_CC:-gcc}" -O2 -static -fno-pie -no-pie "$AXI_PROJECT_DIR/workloads/memory_check.c" -o "$AXI_PROJECT_DIR/build/memory_check"
for latency in 3 9; do
    case_dir="$cpu_results/cpu_l$latency"
    mkdir -p "$case_dir"
    "$AXI_GEM5_BIN" --listener-mode=off -d "$case_dir" "$AXI_PROJECT_DIR/configs/run.py" \
        --mode cpu --binary "$AXI_PROJECT_DIR/build/memory_check" \
        --no-stalls --latency "$latency" > "$case_dir/run.log" 2>&1
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$case_dir" --latency "$latency"
done
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/audit_wave.py" \
    "$cpu_results/cpu_l3" "$cpu_results/cpu_l9" --output "$cpu_results/wave_audit"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check_cpu_timing.py" "$cpu_results"
mkdir -p "$AXI_PROJECT_DIR/results/cpu_timing"
cp -a "$cpu_results/." "$AXI_PROJECT_DIR/results/cpu_timing/"
echo "Verified CPU timing results: $AXI_PROJECT_DIR/results/cpu_timing/comparison.json"
