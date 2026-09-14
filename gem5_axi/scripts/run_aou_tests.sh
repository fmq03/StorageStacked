#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
destination=${1:-"$AXI_PROJECT_DIR/results/aou"}
common_args=("${@:2}")
mkdir -p "$destination" "$AXI_PROJECT_DIR/build"
results=$(mktemp -d "$AXI_ENV_ROOT/aou-tests.XXXXXX")
echo "Working results: $results"
"${AXI_CC:-gcc}" -O2 -static -fno-pie -no-pie "$AXI_PROJECT_DIR/workloads/memory_check.c" -o "$AXI_PROJECT_DIR/build/memory_check"
run_case() {
    local name=$1
    shift
    mkdir -p "$results/$name"
    "$AXI_GEM5_BIN" --listener-mode=off -d "$results/$name" "$AXI_PROJECT_DIR/configs/run.py" --backend aou "${common_args[@]}" "$@" > "$results/$name/run.log" 2>&1
    tail -4 "$results/$name/run.log"
    local check_flags=()
    [[ $name == cpu || $name == id_wrap ]] || check_flags+=(--directed)
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check.py" "$results/$name" "${check_flags[@]}"
    local flags=()
    [[ $name != replay ]] || flags+=(--replay)
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check_aou.py" "$results/$name" "${flags[@]}"
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/inspect_link.py" "$results/$name"
    "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/trace_view.py" "$results/$name"
}
run_case directed --mode tester
run_case rp2 --mode tester --planes 2
run_case replay --mode tester --planes 2 --replay
run_case period_3ns --mode tester --period 3ns
run_case cpu --mode cpu --binary "$AXI_PROJECT_DIR/build/memory_check"
"${AXI_CC:-gcc}" -O2 -static -fno-pie -no-pie "$AXI_PROJECT_DIR/workloads/id_wrap_check.c" -o "$AXI_PROJECT_DIR/build/id_wrap_check"
run_case id_wrap --mode cpu --binary "$AXI_PROJECT_DIR/build/id_wrap_check" --planes 2 --max-ticks 20000000000000
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/audit_wave.py" "$results/directed" "$results/rp2" "$results/replay" "$results/period_3ns" "$results/cpu" "$results/id_wrap" --output "$results/wave_audit"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check_aou_negative.py" "$results/directed"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/check_link_negative.py" "$results/directed"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/summarize_aou.py" "$results"
cp -a "$results/." "$destination/"
echo "Verified AoU results: $destination"
