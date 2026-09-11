#!/usr/bin/env bash
# Generate a synthetic decoder-LLM HETTrace, project it to mem_sim, and run hbm_sim.

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
# shellcheck source=../../scripts/native_env.sh
. "$PROJ_DIR/scripts/native_env.sh"

OUT_DIR=${LLM_BENCH_OUT:-$PROJ_DIR/build/llm_memory}
TRACE_DIR=$OUT_DIR/traces
MEM_SIM_TRACE=$OUT_DIR/mem_sim.trace
MEM_SIM_MAP=$OUT_DIR/mem_sim.map.csv
MEM_SIM_RESPONSES=$OUT_DIR/hbm_sim.responses.csv

HET_PYTHON=${HET_PYTHON:-python3}
MEMSIM_CONFIG=${MEMSIM_CONFIG:-$MEMSIM_HOME/configs/hbm.cfg}
MEMSIM_STANDARD=${MEMSIM_STANDARD:-hbm4}
MEMSIM_TICKS_PER_CYCLE=${MEMSIM_TICKS_PER_CYCLE:-1000}
MEMSIM_MAX_CYCLES=${MEMSIM_MAX_CYCLES:-100000000}
MEMSIM_PROGRESS_INTERVAL=${MEMSIM_PROGRESS_INTERVAL:-0}
MEMSIM_STATS_VIEW=${MEMSIM_STATS_VIEW:-summary}

require_file() {
    if [ ! -f "$1" ]; then
        printf 'missing %s: %s\n' "$2" "$1" >&2
        exit 2
    fi
}

require_file "$MEMSIM_CONFIG" "mem_sim config"
if [ ! -x "$MEMSIM_BIN" ]; then
    printf 'missing external hbm_sim executable: %s\n' "$MEMSIM_BIN" >&2
    printf 'build mem_sim first, or set MEMSIM_HOME/MEMSIM_BUILD/MEMSIM_BIN\n' >&2
    exit 2
fi

case "$MEMSIM_TICKS_PER_CYCLE" in
    ''|*[!0-9]*|0)
        printf 'MEMSIM_TICKS_PER_CYCLE must be a positive integer\n' >&2
        exit 2
        ;;
esac
case "$MEMSIM_MAX_CYCLES" in
    ''|*[!0-9]*|0)
        printf 'MEMSIM_MAX_CYCLES must be a positive integer\n' >&2
        exit 2
        ;;
esac
case "$MEMSIM_PROGRESS_INTERVAL" in
    ''|*[!0-9]*)
        printf 'MEMSIM_PROGRESS_INTERVAL must be a non-negative integer\n' >&2
        exit 2
        ;;
esac
case "$MEMSIM_STATS_VIEW" in
    summary|full) ;;
    *)
        printf 'MEMSIM_STATS_VIEW must be summary or full\n' >&2
        exit 2
        ;;
esac

mkdir -p "$TRACE_DIR"

"$HET_PYTHON" "$SELF_DIR/generate_trace.py" --output "$TRACE_DIR" "$@"

PYTHONPATH="$PROJ_DIR/tools" "$HET_PYTHON" -m hettrace validate "$TRACE_DIR" \
    | tee "$OUT_DIR/validate.txt"
PYTHONPATH="$PROJ_DIR/tools" "$HET_PYTHON" -m hettrace stats "$TRACE_DIR" \
    --window 1000000 | tee "$OUT_DIR/stats.txt"

PYTHONPATH="$PROJ_DIR/tools" "$HET_PYTHON" -m hettrace convert "$TRACE_DIR" \
    --preset memsim --ticks-per-cycle "$MEMSIM_TICKS_PER_CYCLE" \
    --map-output "$MEM_SIM_MAP" -o "$MEM_SIM_TRACE"

"$MEMSIM_BIN" \
    --config "$MEMSIM_CONFIG" \
    --standard "$MEMSIM_STANDARD" \
    --trace "$MEM_SIM_TRACE" \
    --requests 0 \
    --max-cycles "$MEMSIM_MAX_CYCLES" \
    --progress-interval "$MEMSIM_PROGRESS_INTERVAL" \
    --stats-view "$MEMSIM_STATS_VIEW" \
    --response-delivery-mode host \
    --response-trace "$MEM_SIM_RESPONSES" \
    | tee "$OUT_DIR/hbm_sim.txt"

"$HET_PYTHON" "$SELF_DIR/compare_results.py" \
    "$TRACE_DIR/benchmark.json" \
    "$MEM_SIM_MAP" "$MEM_SIM_RESPONSES" \
    | tee "$OUT_DIR/summary.md"

echo "LLM memory benchmark outputs: $OUT_DIR"
