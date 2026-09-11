#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/activate.sh"
export LD_LIBRARY_PATH="$SS_DEPS_ROOT/xpu-native/lib:$VORTEX_HOME/third_party/ramulator:$LD_LIBRARY_PATH"
destination=${1:-"$SS_ROOT/results/xpu-$(date -u +%Y%m%dT%H%M%SZ)"}
[[ ! -e "$destination" ]] || { echo "结果目录已存在：$destination" >&2; exit 1; }
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
"$AXI_PYTHON" "$SS_ROOT/env/check_sources.py"
"$AXI_PYTHON" "$SS_ROOT/env/record.py" "$destination/environment"
"$AXI_PYTHON" "$SS_ROOT/env/record_xpu.py" "$destination/environment"
ctest --test-dir "$MEMSIM_BUILD" --show-only=json-v1 > "$destination/native-test-plan.json"
ctest --test-dir "$MEMSIM_BUILD" --output-on-failure > "$destination/native-tests.log" 2>&1
"$AXI_PYTHON" "$MEMSIM_HOME/integration/check_online.py" "$MEMSIM_BUILD/libstoragestacked_memsim.so" "$destination/api"
gpu=(--vortex-library "$VORTEX_BUILD/sim/simx/libvortex-gem5.so" --vortex-host-rt-dir "$VORTEX_BUILD/sw/runtime")
npu=(--npu-library "$CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so" --npu-kernel "$SS_ROOT/build/xpu/ddr_touch.elf")
run_case() {
    local name=$1
    shift
    local dir="$destination/$name"
    mkdir -p "$dir"
    echo "运行 $name"
    "$AXI_GEM5_BIN" -d "$dir" "$AXI_PROJECT_DIR/configs/run_xpu.py" "$@" > "$dir/run.log" 2>&1
    for checker in check check_aou inspect_link check_memsim trace_view memsim_view; do
        "$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/$checker.py" "$dir" >> "$dir/verification.log" 2>&1
    done
    "$AXI_PYTHON" -m hettrace validate "$dir/hettrace" --ticks-per-second 1000000000000000 > "$dir/hettrace/validation.txt"
    echo "$name 计算与链路校验通过"
}
run_case npu --cmd "$HET_PROJECT_ROOT/workloads/shared_buffer/build/host_main" --options=npu "${npu[@]}"
run_case gpu --cmd "$VORTEX_BUILD/tests/regression/vecadd/vecadd" --options="-n 4 -k $VORTEX_BUILD/tests/regression/vecadd/kernel.vxbin" "${gpu[@]}"
run_case three --cmd "$HET_PROJECT_ROOT/workloads/three_source/build/host_main" --options="-k $VORTEX_BUILD/tests/regression/vecadd/kernel.vxbin" "${gpu[@]}" "${npu[@]}"
run_case three_slow --cmd "$HET_PROJECT_ROOT/workloads/three_source/build/host_main" --options="-k $VORTEX_BUILD/tests/regression/vecadd/kernel.vxbin" "${gpu[@]}" "${npu[@]}" --memsim-scale 4
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/audit_wave.py" "$destination/npu" "$destination/gpu" "$destination/three" "$destination/three_slow" --output "$destination/wave_audit"
"$AXI_PYTHON" "$SS_ROOT/env/verify_xpu.py" "$destination"
echo "三源全链路验收通过：$destination/summary.json"
