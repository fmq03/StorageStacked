#!/usr/bin/env bash
set -euo pipefail

# 四个 demo 共用同一条经过回归测试的运行路径；包装脚本只负责选择标准。
# 可通过 HBM_SIM_BIN、STACK_COUNT、OUTPUT_ROOT 覆盖二进制、Stack 数和输出根目录。
demo_standard=${1:?usage: run_demo.sh hbm3|hbm4|lpddr5|lpddr6}
case "$demo_standard" in
  hbm3|hbm4) demo_family=hbm ;;
  lpddr5|lpddr6) demo_family=lpddr ;;
  *) echo "unsupported demo standard: $demo_standard" >&2; exit 2 ;;
esac

demo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
demo_bin=${HBM_SIM_BIN:-$demo_root/build-clang-debug/hbm_sim}
demo_stacks=${STACK_COUNT:-2}
demo_output_root=${OUTPUT_ROOT:-$demo_root/outputs/demos}
demo_out=$demo_output_root/${demo_standard}_${demo_stacks}stack
demo_config=$demo_root/examples/configs/${demo_family}_nstacks.cfg
demo_trace=$demo_root/examples/multistack_demos/two_stack_payload.trace

if [[ ! -x "$demo_bin" ]]; then
  echo "hbm_sim binary not found: $demo_bin" >&2
  echo "build it first: cmake --build build-clang-debug -j" >&2
  exit 2
fi
if ! [[ "$demo_stacks" =~ ^[2-9][0-9]*$ ]]; then
  echo "STACK_COUNT must be an integer >= 2" >&2
  exit 2
fi

mkdir -p "$demo_out"
"$demo_bin" \
  --config "$demo_config" --standard "$demo_standard" \
  --stack-count "$demo_stacks" --trace "$demo_trace" --requests 0 \
  --dump-resolved-config "$demo_out/resolved.cfg" \
  --cmd-trace "$demo_out/commands.csv" \
  --response-trace "$demo_out/responses.csv" \
  --dfi-trace "$demo_out/dfi.csv" \
  --dfi-signal-trace "$demo_out/dfi_signals.csv" \
  --dump-timing-table "$demo_out/timing.csv" \
  --dump-thermal-map "$demo_out/thermal_{stack}.txt" \
  --validate-cmd-trace --validate-dfi-trace \
  --stats-view summary --stats-json "$demo_out/result.json" \
  | tee "$demo_out/stats.txt"

# Dashboard 同时合并所有 Stack 的热网格；命令轨迹本身已经带 stack_id。
thermal_args=()
for ((demo_stack=0; demo_stack<demo_stacks; ++demo_stack)); do
  thermal_args+=(--thermal-map "$demo_out/thermal_${demo_stack}.txt")
done
python3 "$demo_root/tools/visualize.py" \
  --command-trace "$demo_out/commands.csv" \
  --dfi-trace "$demo_out/dfi.csv" \
  --stats "$demo_out/result.json" \
  "${thermal_args[@]}" \
  --out "$demo_out/dashboard.html" \
  --title "${demo_standard^^} ${demo_stacks}-Stack validation dashboard"

echo "demo complete: $demo_out"
echo "open: $demo_out/dashboard.html"
