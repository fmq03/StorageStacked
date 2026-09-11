#!/usr/bin/env bash
# Host↔Vortex functional and unified AXI HETTrace acceptance.

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
# shellcheck source=../../scripts/native_env.sh
. "$PROJ_DIR/scripts/native_env.sh"

GEM5_BIN=${GEM5_BIN:-$GEM5_HOME/build/X86/gem5.opt}
CONFIG=$GEM5_HOME/configs/het/het_system.py
VORTEX_SO=$VORTEX_BUILD/sim/simx/libvortex-gem5.so
RT_DIR=$VORTEX_BUILD/sw/runtime
TEST_DIR=$VORTEX_BUILD/tests/regression/vecadd
VXBIN=$TEST_DIR/kernel.vxbin
N=${N:-4}

fail() { printf '错误: %s\n' "$*" >&2; exit 1; }
require_file() { [ -f "$1" ] || fail "找不到 $2: $1"; }

[ -x "$GEM5_BIN" ] || fail "找不到 gem5.opt: $GEM5_BIN"
require_file "$CONFIG" "het_system.py（先运行 gem5int/install.sh）"
require_file "$GEM5_HOME/build/X86/params/HetAxiMonitor.hh" \
    "HetAxiMonitor 参数头（先重新构建 gem5）"
require_file "$VORTEX_SO" "Vortex gem5 library"
require_file "$RT_DIR/libvortex.so" "Vortex runtime"
require_file "$RT_DIR/libvortex-gem5-x86_64.so" "Vortex gem5 driver"
[ -x "$TEST_DIR/vecadd" ] || fail "找不到 Vortex vecadd: $TEST_DIR/vecadd"
require_file "$VXBIN" "Vortex vecadd kernel"

# Vortex SimX itself still depends on this upstream private copy.  It is not
# the project's DRAM backend and does not consume HETTrace.
VORTEX_RAMULATOR=$VORTEX_HOME/third_party/ramulator
require_file "$VORTEX_RAMULATOR/libramulator.so" \
    "Vortex third_party Ramulator dependency"
export LD_LIBRARY_PATH="$VORTEX_RAMULATOR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$PROJ_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"

OUT=${HET_VORTEX_SHARED_OUT:-$(mktemp -d /tmp/hettrace-vortex-shared.XXXXXX)}
M5OUT=${HET_VORTEX_SHARED_M5OUT:-$(mktemp -d /tmp/hettrace-vortex-shared-m5.XXXXXX)}
mkdir -p "$OUT" "$M5OUT"

echo "---- 1/3 host runtime 驱动 Vortex vecadd ----"
set +e
LOG=$(cd "$TEST_DIR" && HETTRACE_DIR="$OUT" \
    "$GEM5_BIN" --outdir="$M5OUT" "$CONFIG" \
    --cmd "$TEST_DIR/vecadd" --options="-n$N" \
    --vortex-library "$VORTEX_SO" --vortex-host-rt-dir "$RT_DIR" \
    --vortex-fast-forward "$@" 2>&1)
RC=$?
set -e
printf '%s\n' "$LOG" | grep -E \
    '^(PASSED|FAILED)|HetAxiMonitor: .*closed|^---- 结束' | sed 's/^/  /' || true
if [ "$RC" -ne 0 ]; then
    printf '%s\n' "$LOG" | tail -30 >&2
    fail "gem5 返回 $RC"
fi
printf '%s\n' "$LOG" | grep -q '^PASSED!' || fail "vecadd 自检没有通过"

echo
echo "---- 2/3 单一互连观察点、来源分类与共享地址 ----"
python3 - "$OUT" <<'PY'
import json
import pathlib
import sys

from hettrace import reader

root = pathlib.Path(sys.argv[1])
errors = []
records = {}
for name, period in (("host", 500), ("vortex", 1000)):
    trace = root / (name + ".hettrace")
    meta_path = root / (name + ".hettrace.meta.json")
    if not trace.is_file() or not meta_path.is_file():
        errors.append("%s trace/meta 缺失" % name)
        continue
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    expected = {
        "level": 3,
        "clock_period_ticks": period,
        "synth": True,
        "unmapped": 0,
        "non_monotonic": 0,
    }
    for field, wanted in expected.items():
        if meta.get(field) != wanted:
            errors.append("%s.%s=%r，期望 %r" %
                          (name, field, meta.get(field), wanted))
    records[name] = list(reader.read_records(str(trace)))
    print("  %-7s records=%-7d txns=%-6d data=%-7d" % (
        name, meta.get("emitted", 0), meta.get("transactions", 0),
        meta.get("data_records", 0)))

if (root / "coralnpu.hettrace").exists():
    errors.append("未启用 CoralNPU，却生成了 coralnpu.hettrace")

if "vortex" in records:
    DMA = 1 << 4
    data = [r for r in records["vortex"]
            if r.chan in (reader.CHAN_W, reader.CHAN_R)]
    dma = [r for r in data if r.flags & DMA]
    core = [r for r in data if not (r.flags & DMA)]
    bar = 0x100000000
    staging = bar + 0xfc000000
    buffer_limit = bar + 0x10000000
    if not dma:
        errors.append("Vortex CP DMA 没有被分类")
    if not core:
        errors.append("Vortex core 流量没有被分类")
    if not any(r.addr >= staging for r in dma):
        errors.append("CP DMA 未覆盖 host staging aperture")
    if not any(bar <= r.addr < buffer_limit for r in dma):
        errors.append("CP DMA 未覆盖 device buffer")

if "host" in records and "vortex" in records:
    def lines(items):
        return {r.addr >> 6 for r in items
                if r.chan in (reader.CHAN_W, reader.CHAN_R)}
    shared = lines(records["host"]) & lines(records["vortex"])
    if not shared:
        errors.append("host 与 Vortex 没有共享 cache line")
    else:
        print("  shared cache lines: %d" % len(shared))

# 这一用例显式启用 atomic fast-forward。sendAtomic() 只返回 delay，并不会在
# monitor 的当前调用栈里推进 curTick()；若把 B/R 直接记成请求 tick，绝大多数
# host 事务会退化为伪零延迟。反过来，若立即写 curTick()+delay，又会让下一笔请求
# 的时间戳倒退。最终实现应暂存完成事件：这里要求多数 host 事务保留非零延迟，
# non_monotonic=0 则已由上面的 meta 检查覆盖。
if "host" in records:
    starts = {}
    completions = {}
    for record in records["host"]:
        if record.chan in (reader.CHAN_AW, reader.CHAN_AR):
            starts[record.txn] = record.tick
        elif record.chan == reader.CHAN_B or (
                record.chan == reader.CHAN_R and
                record.flags & reader.FLAG_LAST):
            completions[record.txn] = record.tick
    latencies = [completions[txn] - tick for txn, tick in starts.items()
                 if txn in completions]
    positive = sum(delay > 0 for delay in latencies)
    if not latencies:
        errors.append("host 没有可配对的 AW/B 或 AR/R 事务")
    elif positive * 2 <= len(latencies):
        errors.append("host 非零响应延迟仅 %d/%d，atomic fast-forward 可能被伪装成零延迟" %
                      (positive, len(latencies)))
    else:
        print("  non-zero host response latency: %d/%d" %
              (positive, len(latencies)))

if errors:
    raise SystemExit("\n".join("FAIL " + error for error in errors))
print("  ok: host/Vortex 均来自 level=interconnect 的统一 tap")
print("  ok: core、CP DMA、staging 与 device buffer 交接均可追踪")
PY

echo
echo "---- 3/3 AXI 因果校验与外部 mem_sim 投影 ----"
python3 -m hettrace validate "$OUT"
python3 -m hettrace convert "$OUT" --preset memsim \
    --ticks-per-cycle "${MEMSIM_TICKS_PER_CYCLE:-1000}" \
    -o "$OUT/mem_sim.trace"
python3 - "$OUT" <<'PY'
import csv
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
expected = sum(json.loads(path.read_text(encoding="utf-8"))["data_records"]
               for path in root.glob("*.hettrace.meta.json"))
mapped = sum(1 for _ in csv.DictReader(
    (root / "mem_sim.trace.map.csv").open(encoding="utf-8")))
lines = sum(1 for _ in (root / "mem_sim.trace").open(encoding="utf-8"))
if expected != mapped or expected != lines:
    raise SystemExit("投影不守恒: data=%d trace=%d map=%d" %
                     (expected, lines, mapped))
print("  ok: %d 个 AXI data beat 投影为 %d 个可追溯请求" %
      (expected, mapped))
PY

echo "全部通过。trace: $OUT"
echo "m5out: $M5OUT"
