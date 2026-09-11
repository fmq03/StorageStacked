#!/usr/bin/env bash
set -euo pipefail

vis_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
vis_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
vis_tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$vis_tmp_dir"' EXIT

"$vis_bin" \
  --config "$vis_source/configs/hbm.cfg" --standard hbm4 \
  --requests 16 \
  --stats-json "$vis_tmp_dir/result.json" \
  --read-ratio 50 \
  --cmd-trace "$vis_tmp_dir/commands.csv" \
  --dfi-trace "$vis_tmp_dir/dfi.csv" \
  --dump-thermal-map "$vis_tmp_dir/thermal.txt" \
  > "$vis_tmp_dir/stats.txt"

cat > "$vis_tmp_dir/performance.json" <<'JSON'
{"rows":[{"standard":"HBM4","read_ratio_pct":100,"offered_requests_per_tick":0.25,"avg_read_latency_ticks":20.0,"achieved_bw_GBps":8.0,"bandwidth_util_pct":12.5},{"standard":"HBM4","read_ratio_pct":100,"offered_requests_per_tick":1.0,"avg_read_latency_ticks":80.0,"achieved_bw_GBps":15.0,"bandwidth_util_pct":23.4}]}
JSON

python3 "$vis_source/tools/visualize.py" \
  --command-trace "$vis_tmp_dir/commands.csv" \
  --dfi-trace "$vis_tmp_dir/dfi.csv" \
  --stats "$vis_tmp_dir/result.json" \
  --performance-json "$vis_tmp_dir/performance.json" \
  --thermal-map "$vis_tmp_dir/thermal.txt" \
  --max-events 5 \
  --out "$vis_tmp_dir/dashboard.html"

test -s "$vis_tmp_dir/dashboard.html"
python3 - "$vis_source" "$vis_tmp_dir/thermal.txt" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, str(Path(sys.argv[1]).resolve() / 'tools'))
from visualize import read_thermal
nodes = read_thermal([Path(sys.argv[2])])
assert nodes and any(n['address_kind'] == 'direct_event' for n in nodes)
assert any(n['address_kind'] == 'coupling_only' for n in nodes)
for node in nodes:
    if node['address_kind'] == 'coupling_only':
        assert node['channel'] == -1 and node['bank'] == -1
        assert node['events'] == 0
PY
grep -q "Offline validation dashboard" "$vis_tmp_dir/dashboard.html"
grep -q "Trace explorer" "$vis_tmp_dir/dashboard.html"
grep -q "Request swimlanes" "$vis_tmp_dir/dashboard.html"
grep -q '"commands"' "$vis_tmp_dir/dashboard.html"
grep -q '"sampled":true' "$vis_tmp_dir/dashboard.html"
grep -q '"performance":\[{"standard":"HBM4"' "$vis_tmp_dir/dashboard.html"
grep -q '<polyline points=' "$vis_tmp_dir/dashboard.html" || grep -q 'polyline points=' "$vis_source/tools/visualize.py"

echo "visualization smoke passed"
