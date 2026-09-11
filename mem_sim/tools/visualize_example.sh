#!/usr/bin/env bash
set -euo pipefail

vis_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
vis_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
vis_out=${HBM_SIM_VIS_OUT:-outputs/visualization/example}
mkdir -p "$vis_out"

"$vis_bin" \
  --config "$vis_source/configs/hbm.cfg" --standard hbm4 \
  --stats-view summary --stats-json "$vis_out/result.json" \
  --requests 128 \
  --read-ratio 50 \
  --cmd-trace "$vis_out/commands.csv" \
  --dfi-trace "$vis_out/dfi.csv" \
  --dump-thermal-map "$vis_out/thermal_map.txt" \
  --validate-cmd-trace \
  --validate-dfi-trace \
  | tee "$vis_out/stats.txt"

python3 "$vis_source/tools/performance_curve.py" \
  --binary "$vis_bin" \
  --standards hbm4 \
  --requests 256 \
  --read-ratios 100,50 \
  --csv-out "$vis_out/performance_curves.csv" \
  --json-out "$vis_out/performance_curves.json"

python3 "$vis_source/tools/visualize.py" \
  --command-trace "$vis_out/commands.csv" \
  --dfi-trace "$vis_out/dfi.csv" \
  --stats "$vis_out/result.json" \
  --performance-json "$vis_out/performance_curves.json" \
  --thermal-map "$vis_out/thermal_map.txt" \
  --out "$vis_out/dashboard.html" \
  --title "HBM4 validation dashboard"

echo "Open $vis_out/dashboard.html in a browser."
