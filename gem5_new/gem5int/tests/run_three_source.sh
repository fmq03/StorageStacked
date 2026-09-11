#!/usr/bin/env bash
# Full functional acceptance for the unified memory-side HETTrace v2 tap.

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
# shellcheck source=../../scripts/native_env.sh
. "$PROJ_DIR/scripts/native_env.sh"

GEM5_BIN=${GEM5_BIN:-$GEM5_HOME/build/X86/gem5.opt}
CONFIG=$GEM5_HOME/configs/het/het_system.py
HOST_BIN=$PROJ_DIR/workloads/three_source/build/host_main
VORTEX_SO=$VORTEX_BUILD/sim/simx/libvortex-gem5.so
RT_DIR=$VORTEX_BUILD/sw/runtime
VXBIN=$VORTEX_BUILD/tests/regression/vecadd/kernel.vxbin
TICKS_PER_CYCLE=${MEMSIM_TICKS_PER_CYCLE:-1000}

fail() { printf '错误: %s\n' "$*" >&2; exit 1; }
require_file() { [ -f "$1" ] || fail "找不到 $2: $1"; }

[ -x "$GEM5_BIN" ] || fail "找不到 gem5.opt: $GEM5_BIN"
require_file "$CONFIG" "het_system.py（先运行 gem5int/install.sh）"
require_file "$GEM5_HOME/build/X86/params/HetAxiMonitor.hh" \
    "HetAxiMonitor 参数头（先重新构建 gem5）"
require_file "$VORTEX_SO" "Vortex gem5 library"
require_file "$RT_DIR/libvortex.so" "Vortex runtime"
require_file "$RT_DIR/libvortex-gem5-x86_64.so" "Vortex gem5 driver"
require_file "$VXBIN" "Vortex vecadd kernel"

# host 很小，测试自己构建，干净源码树无需保留二进制产物。
make -s -C "$PROJ_DIR/workloads/three_source" \
    VORTEX_HOME="$VORTEX_HOME" VORTEX_BUILD="$VORTEX_BUILD" >/dev/null || \
    fail "三源 host workload 构建失败"
[ -x "$HOST_BIN" ] || fail "找不到三源 host workload: $HOST_BIN"

NPU_SO=$(readlink -f "$CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so" \
    2>/dev/null || true)
require_file "$NPU_SO" "CoralNPU gem5 library"
BAZEL_OUT=$(readlink -f "$CORALNPU_HOME/bazel-out" 2>/dev/null || true)
NPU_ELF=$(find "$BAZEL_OUT" -path '*/gem5int/ddr_touch.elf' -print -quit \
    2>/dev/null || true)
require_file "$NPU_ELF" "CoralNPU ddr_touch.elf"

# This Ramulator is an existing Vortex SimX runtime dependency.  It is not the
# project memory backend and does not consume HETTrace.
VORTEX_RAMULATOR=$VORTEX_HOME/third_party/ramulator
require_file "$VORTEX_RAMULATOR/libramulator.so" \
    "Vortex third_party Ramulator dependency"
export LD_LIBRARY_PATH="$VORTEX_RAMULATOR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$PROJ_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"

OUT=${HET_THREE_SOURCE_OUT:-$(mktemp -d /tmp/hettrace-three-source.XXXXXX)}
M5OUT=${HET_THREE_SOURCE_M5OUT:-$(mktemp -d /tmp/hettrace-three-source-m5.XXXXXX)}
mkdir -p "$OUT" "$M5OUT"

echo "---- 1/4 gem5 功能执行：host + CoralNPU + Vortex ----"
set +e
LOG=$(HETTRACE_DIR="$OUT" "$GEM5_BIN" --outdir="$M5OUT" "$CONFIG" \
    --cmd "$HOST_BIN" --options="-k $VXBIN" \
    --vortex-library "$VORTEX_SO" --vortex-host-rt-dir "$RT_DIR" \
    --vortex-fast-forward \
    --npu-library "$NPU_SO" --npu-kernel "$NPU_ELF" "$@" 2>&1)
RC=$?
set -e
printf '%s\n' "$LOG" | grep -E \
    '^host: |CoralNPU: kernel finished|HetAxiMonitor: .*closed|^---- 结束' \
    | sed 's/^/  /' || true
if [ "$RC" -ne 0 ]; then
    printf '%s\n' "$LOG" | tail -30 >&2
    fail "gem5 返回 $RC"
fi
printf '%s\n' "$LOG" | grep -q '^host: 全部通过' || \
    fail "host/NPU/Vortex 功能自检未通过"

echo
echo "---- 2/4 单观察点与 HETTrace v2 元数据 ----"
python3 - "$OUT" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
expected = {
    "host": (500, True),
    "vortex": (1000, True),
    "coralnpu": (2000, True),
}
errors = []
for name, (period, synth) in expected.items():
    trace = root / (name + ".hettrace")
    meta_path = root / (name + ".hettrace.meta.json")
    if not trace.is_file() or not meta_path.is_file():
        errors.append("%s trace/meta 缺失" % name)
        continue
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    checks = {
        "level": (meta.get("level"), 3),
        "clock_period_ticks": (meta.get("clock_period_ticks"), period),
        "synth": (meta.get("synth"), synth),
        "axi_data_bytes": (meta.get("axi_data_bytes"), 16),
        "unmapped": (meta.get("unmapped"), 0),
        "non_monotonic": (meta.get("non_monotonic"), 0),
    }
    for field, (actual, wanted) in checks.items():
        if actual != wanted:
            errors.append("%s.%s=%r, 期望 %r" % (name, field, actual, wanted))
    if meta.get("emitted", 0) <= 0 or meta.get("transactions", 0) <= 0:
        errors.append("%s 没有 AXI 事件/事务" % name)
    print("  %-9s records=%-7d txns=%-6d data=%-7d synth=%s" % (
        name, meta.get("emitted", 0), meta.get("transactions", 0),
        meta.get("data_records", 0), meta.get("synth")))
if errors:
    raise SystemExit("\n".join("FAIL " + error for error in errors))
PY

echo
echo "---- 3/4 协议因果、来源分类与共享区 ----"
python3 -m hettrace validate "$OUT"
python3 - "$OUT" <<'PY'
import pathlib
import sys

from hettrace import reader
from hettrace import addrmap

root = pathlib.Path(sys.argv[1])
records = {
    name: list(reader.read_records(str(root / (name + ".hettrace"))))
    for name in ("host", "vortex", "coralnpu")
}
SYNTH = 1 << 6
DMA = 1 << 4
W = reader.CHAN_W
R = reader.CHAN_R
errors = []
if any(not (record.flags & SYNTH) for name in ("host", "vortex", "coralnpu")
       for record in records[name]):
    errors.append("统一 monitor 存在未标 SYNTH 的 packet 投影记录")
vortex_data = [r for r in records["vortex"] if r.chan in (W, R)]
if not any(r.flags & DMA for r in vortex_data):
    errors.append("Vortex CP DMA 未被分类")
if not any(not (r.flags & DMA) for r in vortex_data):
    errors.append("Vortex core 流量未被分类")
npu_channels = {channel: 0 for channel in range(5)}
for record in records["coralnpu"]:
    npu_channels[record.chan] += 1
if [npu_channels[i] for i in range(5)] != [64, 64, 64, 64, 64]:
    errors.append("CoralNPU 五通道计数不是各 64: %r" % npu_channels)
if not any(r.chan == W and r.strb != (1 << r.size) - 1
           for r in records["coralnpu"]):
    errors.append("CoralNPU 的部分 WSTRB 没有保留到统一 tap")

def touches(name, region):
    base, size, _kind, _accessors = addrmap.REGIONS[region]
    return any(base <= r.addr < base + size and r.chan in (W, R)
               for r in records[name])

for source, region in (("host", "shared_buffer"),
                       ("coralnpu", "shared_buffer"),
                       ("host", "vortex_bar"),
                       ("vortex", "vortex_bar")):
    if not touches(source, region):
        errors.append("%s 未触及 %s" % (source, region))
v = records["vortex"]
n = records["coralnpu"]
if max(v[0].tick, n[0].tick) >= min(v[-1].tick, n[-1].tick):
    errors.append("Vortex 与 CoralNPU 活动区间不重叠")
if errors:
    raise SystemExit("\n".join("FAIL " + error for error in errors))
print("  ok: 三源五通道均标 SYNTH；Vortex core/CP DMA 分离")
print("  ok: NPU seam 的地址/ID/部分 WSTRB 保留到统一 packet 投影")
print("  ok: host↔NPU shared_buffer、host↔Vortex BAR 均有数据通道证据")
PY

echo
echo "---- 4/4 投影为外部 mem_sim 请求流 ----"
python3 -m hettrace convert "$OUT" --preset memsim \
    --ticks-per-cycle "$TICKS_PER_CYCLE" -o "$OUT/mem_sim.trace"
python3 - "$OUT" <<'PY'
import csv
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
mapped = sum(1 for _ in csv.DictReader(
    (root / "mem_sim.trace.map.csv").open(encoding="utf-8")))
expected = sum(json.loads(path.read_text(encoding="utf-8"))["data_records"]
               for path in root.glob("*.hettrace.meta.json"))
lines = sum(1 for _ in (root / "mem_sim.trace").open(encoding="utf-8"))
if mapped != expected or lines != expected:
    raise SystemExit("投影不守恒: data=%d trace=%d map=%d" %
                     (expected, lines, mapped))
print("  ok: %d 个 AXI data beat -> %d 个 mem_sim 请求，sidecar 逐行可追溯" %
      (expected, mapped))
PY

echo
echo "全部通过。gem5 是功能真值；最终 DRAM 时序请把 $OUT/mem_sim.trace 交给 hbm_sim。"
echo "trace: $OUT"
echo "m5out: $M5OUT"
