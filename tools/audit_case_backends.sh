#!/usr/bin/env bash
# Run one configuration against all three memory backends with identical input.
set -euo pipefail

if (( $# < 4 || $# > 5 )); then
  echo "usage: bash tools/audit_case_backends.sh CONFIG STANDARD PRESET_OR_NONE OUT_DIR [HBM_SIM_BINARY]" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
case_cfg=$1
case_standard=$2
case_preset=$3
case_out=$4
sim_binary=${5:-"$repo_root/build-clang-debug/hbm_sim"}

if [[ $case_cfg != /* ]]; then
  case_cfg="$repo_root/$case_cfg"
fi
if [[ $case_out != /* ]]; then
  case_out="$repo_root/$case_out"
fi
if [[ $sim_binary != /* ]]; then
  sim_binary="$repo_root/$sim_binary"
fi

trace_file="$repo_root/examples/data_check.trace"
# 通用审计镜像不能带 HBM 专用 channel/SID 坐标，否则在单 channel LPDDR
# 配置中会正确地被 MemoryImage 拒绝。坐标由各标准的 AddressMapper 推导。
image_file="$repo_root/examples/backend_audit_image.txt"
for required in "$case_cfg" "$trace_file" "$image_file"; do
  if [[ ! -f $required ]]; then
    echo "error: required file not found: $required" >&2
    exit 2
  fi
done
if [[ ! -x $sim_binary ]]; then
  echo "error: simulator is missing or not executable: $sim_binary" >&2
  exit 2
fi

# File backends reopen an existing image by design. Refuse to mix a new audit
# with stale persistent state; the caller can choose another output directory.
for persistent in "$case_out/mmap/data.bin" "$case_out/mmap/data.bin.meta" \
                  "$case_out/chunk/data.bin" "$case_out/chunk/data.bin.meta"; do
  if [[ -e $persistent ]]; then
    echo "error: existing backend state would make the audit non-reproducible: $persistent" >&2
    echo "choose a new OUT_DIR or archive/remove that audit directory explicitly" >&2
    exit 2
  fi
done

mkdir -p "$case_out/sparse" "$case_out/mmap" "$case_out/chunk"

common=(--config "$case_cfg" --standard "$case_standard"
        --stats-view diagnostic --validate-cmd-trace --validate-dfi-trace
        --trace "$trace_file" --requests 0
        --memory-image "$image_file" --memory-capacity-bytes 1048576)
if [[ $case_preset != none && $case_preset != - ]]; then
  common+=(--preset "$case_preset")
fi

"$sim_binary" "${common[@]}" --memory-backend sparse \
  --stats-json "$case_out/sparse/result.json" \
  --dump-memory-image "$case_out/sparse/final.txt" \
  --dump-memory-csv "$case_out/sparse/final.csv" \
  --mismatch-report "$case_out/sparse/mismatch.txt" \
  | tee "$case_out/sparse/stats.txt" >/dev/null

"$sim_binary" "${common[@]}" --memory-backend mmap_sparse \
  --stats-json "$case_out/mmap/result.json" \
  --memory-data-file "$case_out/mmap/data.bin" \
  --dump-memory-image "$case_out/mmap/final.txt" \
  --dump-memory-csv "$case_out/mmap/final.csv" \
  --mismatch-report "$case_out/mmap/mismatch.txt" \
  --verify-golden "$case_out/sparse/final.txt" \
  | tee "$case_out/mmap/stats.txt" >/dev/null

"$sim_binary" "${common[@]}" --memory-backend chunk_file \
  --stats-json "$case_out/chunk/result.json" \
  --memory-data-file "$case_out/chunk/data.bin" \
  --memory-chunk-size 65536 --memory-chunk-cache-entries 4 \
  --dump-memory-image "$case_out/chunk/final.txt" \
  --dump-memory-csv "$case_out/chunk/final.csv" \
  --mismatch-report "$case_out/chunk/mismatch.txt" \
  --verify-golden "$case_out/sparse/final.txt" \
  | tee "$case_out/chunk/stats.txt" >/dev/null

diff -u "$case_out/sparse/final.txt" "$case_out/mmap/final.txt"
diff -u "$case_out/sparse/final.txt" "$case_out/chunk/final.txt"

python3 - "$repo_root" "$case_out" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
from result_io import count, read_result
for backend in ("sparse", "mmap", "chunk"):
    result = read_result(Path(sys.argv[2]) / backend / "result.json", require_completed=True)
    assert count(result, "data_checked_reads") > 0, "no independent payload checks"
    assert result["cmd_validation"] == result["dfi_validation"] == "pass"
    if backend != "sparse":
        assert count(result, "golden_verified") > 0
        assert count(result, "golden_mismatches") == 0
PY

python3 "$repo_root/tools/view_stats.py" \
  "$case_out/sparse/stats.txt" "$case_out/mmap/stats.txt" "$case_out/chunk/stats.txt" \
  --labels sparse,mmap_sparse,chunk_file \
  --keys standard,memory_backend,memory_backend_line_bytes,completed_reads,completed_writes,remaining_requests,hit_cycle_limit,data_mismatches,golden_verified,golden_mismatches,storage_lines_allocated,unique_written_lines

echo
echo "backend metadata magic (both must read HBMBACK\\0):"
od -An -tc -N8 "$case_out/mmap/data.bin.meta"
od -An -tc -N8 "$case_out/chunk/data.bin.meta"
echo
stat --format='%n logical_bytes=%s allocated_blocks=%b block_bytes=%B' \
  "$case_out"/mmap/data.bin* "$case_out"/chunk/data.bin*
echo
echo "PASS: all three final images are identical; artifacts: $case_out"
