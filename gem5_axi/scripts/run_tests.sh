#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
destination=${1:-"$AXI_PROJECT_DIR/results"}
mkdir -p "$destination" "$AXI_PROJECT_DIR/build" "$AXI_ENV_ROOT"
destination=$(cd "$destination" && pwd)
# Native SystemC VCD writes very frequently. Run on the Linux filesystem,
# then publish the verified artifacts to the project (which may be on /mnt/d).
results=$(mktemp -d "$AXI_ENV_ROOT/axi-tests.XXXXXX")
echo "Working results: $results"
"${AXI_CC:-gcc}" -O2 -static -fno-pie -no-pie "$AXI_PROJECT_DIR/workloads/memory_check.c" -o "$AXI_PROJECT_DIR/build/memory_check"
run_case() {
    local name=$1
    shift
    mkdir -p "$results/$name"
    "$AXI_GEM5_BIN" --listener-mode=off -d "$results/$name" "$AXI_PROJECT_DIR/configs/run.py" "$@" > "$results/$name/run.log" 2>&1
    tail -4 "$results/$name/run.log"
}
run_case directed --mode tester
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$results/directed" --directed
run_case serial_l3 --mode tester --slots 1 --no-stalls --response-hold 0ns --latency 3
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$results/serial_l3" --directed --latency 3
run_case serial_l9 --mode tester --slots 1 --no-stalls --response-hold 0ns --latency 9
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$results/serial_l9" --directed --latency 9
run_case cpu --mode cpu --binary "$AXI_PROJECT_DIR/build/memory_check"
run_case period_3ns --mode tester --period 3ns
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$results/period_3ns" --directed
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$results/cpu"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/audit_wave.py" \
    "$results/directed" "$results/serial_l3" "$results/serial_l9" \
    "$results/period_3ns" "$results/cpu" --output "$results/wave_audit"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/report.py" "$results"
cp -a "$results/." "$destination/"
echo "Verified results: $destination/report.html"
