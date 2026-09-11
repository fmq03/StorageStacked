#!/bin/bash
# 异构系统验收：host + CoralNPU 在 gem5 里协同跑一遍，统一 memory-side
# monitor 分类出的两份 trace 必须能对齐。
#
#   GEM5_HOME=$HOME/gem5 CORALNPU_HOME=$HOME/coralnpu \
#       gem5int/tests/run_het.sh
#
# 前提：gem5int/install.sh 跑过且 gem5.opt 编过，
#       coralnpuint/install.sh 跑过且 bazel build 过。
#
# 与 run_gem5_npu.sh 的分工：那个是单设备验收（没有 CPU，验"gem5 能把 NPU 跑起来
# 并落 trace"）。这个多了 host 那条腿，验的是三件只有在异构系统里才成立的事：
#   1. host 与 NPU 看见的是同一段内存里的**同一批字节**（不只是同一批地址）；
#   2. 两份 trace 的时间戳来自同一个 curTick()，因此可归并；
#   3. 归并之后能量出真实的共享 —— 同一条 cache line 被两个源都碰过。
#
# 四步，最后一步是重点：
#   1. 正向跑，看 host 程序自检通过、两份 trace 都落盘。
#   2. 两份 meta.json 的层级、SYNTH 和计数器逐项过一遍。
#   3. validate + stats：两个源都要在 shared_buffer 里留下记录，且共享 line 数 > 0。
#   4. **反向对照**：--npu-no-share 让 NPU 的 AXI master 回落到库内私有数组。地址
#      不变、trace 照样产出、看起来一切正常，但 host 的校验和必须对不上。如果这一
#      步也"通过"，说明正向那一遍的共享是假的 —— 而正向单独看完全发现不了。

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
GEM5_HOME=${GEM5_HOME:-$HOME/gem5}
CORALNPU_HOME=${CORALNPU_HOME:-$HOME/coralnpu}
GEM5_BIN=${GEM5_BIN:-$GEM5_HOME/build/X86/gem5.opt}
CONFIG="$GEM5_HOME/configs/het/het_system.py"
HOST_BIN="$PROJ_DIR/workloads/shared_buffer/build/host_main"

fail() { echo "错误: $*" >&2; exit 1; }

[ -x "$GEM5_BIN" ] || fail "找不到 $GEM5_BIN，先 scons build/X86/gem5.opt"
[ -f "$CONFIG" ]   || fail "找不到 $CONFIG，先跑 gem5int/install.sh"
[ -f "$GEM5_HOME/build/X86/params/HetAxiMonitor.hh" ] || \
    fail "gem5 缺少 HetAxiMonitor，先重新安装 gem5int 并编译"

# host 程序每次都重编。它很小，而"改了 host_main.c 忘了 make"这种失败会表现成
# 莫名其妙的校验和不符，很难查。
make -s -C "$PROJ_DIR/workloads/shared_buffer" >/dev/null || fail "host_main 编不出来"
[ -x "$HOST_BIN" ] || fail "找不到 $HOST_BIN"

SO=$(readlink -f "$CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so" 2>/dev/null || true)
[ -n "$SO" ] && [ -f "$SO" ] || \
    fail "找不到 libcoralnpu-gem5.so，先 cd $CORALNPU_HOME && bazel build //gem5int:libcoralnpu-gem5.so"

# bazel-out 是符号链接，find 默认不跟进去。
BAZEL_OUT=$(readlink -f "$CORALNPU_HOME/bazel-out" 2>/dev/null || true)
ELF=$(find "$BAZEL_OUT" -path "*/gem5int/ddr_touch.elf" 2>/dev/null | head -1)
[ -n "$ELF" ] || fail "找不到 ddr_touch.elf，先 bazel build //gem5int:ddr_touch.elf"

echo "gem5:     $GEM5_BIN"
echo "host:     $HOST_BIN"
echo "NPU 库:   $SO"
echo "NPU 内核: $ELF"

OUT=$(mktemp -d)
M5OUT=$(mktemp -d)
echo "HETTRACE_DIR=$OUT"

run_het() {  # $1 = 额外传给配置脚本的参数
    HETTRACE_DIR="$1" "$GEM5_BIN" --outdir="$2" "$CONFIG" \
        --cmd "$HOST_BIN" --options npu \
        --npu-library "$SO" --npu-kernel "$ELF" "${@:3}"
}

# ---- 1. 正向 ---------------------------------------------------------------
echo
echo "---- 1/4 正向跑 host + CoralNPU ----"
set +e
LOG=$(run_het "$OUT" "$M5OUT" 2>&1)
RC=$?
set -e
echo "$LOG" | grep -E "^host:|kernel finished|已写|停了" || true
[ "$RC" = "0" ] || { echo "$LOG" | tail -15; fail "gem5 返回 $RC（期望 0）"; }
for f in host.hettrace coralnpu.hettrace; do
    [ -f "$OUT/$f" ] || fail "没有产出 $f"
    [ -f "$OUT/$f.meta.json" ] || fail "没有产出 $f.meta.json"
done
[ ! -e "$OUT/vortex.hettrace" ] || fail "未启用 Vortex，却生成了 vortex.hettrace"
echo "  ok   host 自检通过，两份 trace 与侧车文件都在"

# ---- 2. 侧车计数器 ---------------------------------------------------------
# 侧车文件里的计数器是权威来源，比在报告文本里 grep 关键词可靠 —— 报告措辞会变，
# 而且"报告里没提 non_monotonic"和"non_monotonic 为 0"是两件事。
echo
echo "---- 2/4 两份 meta.json 的计数器 ----"
python3 - "$OUT/host.hettrace.meta.json" "$OUT/coralnpu.hettrace.meta.json" <<'PY' || exit 1
import json, sys
host = json.load(open(sys.argv[1]))
npu  = json.load(open(sys.argv[2]))
bad = []
for m, name, period in ((host, "host", 500), (npu, "coralnpu", 2000)):
    if m["emitted"] == 0:
        bad.append(f"{name}: emitted=0，tap 没接上")
    if m["unmapped"]:
        bad.append(f"{name}: unmapped={m['unmapped']}，地址与 addrmap.json 不一致")
    if m["non_monotonic"]:
        bad.append(f"{name}: non_monotonic={m['non_monotonic']}，时间戳回退")
    # host 2GHz -> 500 tick/周期, NPU 500MHz -> 2000。对不上说明 clk_domain 没生效。
    if m["clock_period_ticks"] != period:
        bad.append(f"{name}: clock_period_ticks={m['clock_period_ticks']}，期望 {period}")
    if m.get("level") != 3:
        bad.append(f"{name}: level={m.get('level')}，期望 3 (interconnect)")
    if m.get("axi_data_bytes") != 16:
        bad.append(f"{name}: axi_data_bytes={m.get('axi_data_bytes')}，期望 16")
if host.get("synth") is not True:
    bad.append(f"host: synth={host.get('synth')}，packet 投影必须标 SYNTH")
if npu.get("synth") is not True:
    bad.append(f"coralnpu: synth={npu.get('synth')}，统一 packet monitor 重构的五通道必须标 SYNTH")
# 同一个时间基准的判据：NPU 是 host 写 REG_CTRL 之后才开始跑的，而 host 一直轮询到
# 它停下，所以 NPU 的整个区间必须套在 host 的区间**里面**。如果设备库自己数周期
# （从 0 开始）而不是用 gem5 的 curTick()，这一条立刻不成立。
if not (host["first_tick"] < npu["first_tick"] and npu["last_tick"] < host["last_tick"]):
    bad.append(f"NPU 区间 [{npu['first_tick']}, {npu['last_tick']}] 没套在 host 区间 "
               f"[{host['first_tick']}, {host['last_tick']}] 里 —— 两边时间基准不同")
for b in bad:
    print(f"  FAIL {b}")
if not bad:
    print(f"  ok   host {host['emitted']} 条 / coralnpu {npu['emitted']} 条，"
          f"level=interconnect, SYNTH=true, unmapped=0 non_monotonic=0")
    print(f"  ok   NPU 区间 [{npu['first_tick']}, {npu['last_tick']}] 落在 host 区间内"
          f"，两边同一个 curTick()")
sys.exit(1 if bad else 0)
PY

# ---- 3. validate + stats ---------------------------------------------------
echo
echo "---- 3/4 hettrace validate / stats ----"
export PYTHONPATH="$PROJ_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"
set +e
REPORT=$(python3 -m hettrace validate "$OUT" 2>&1)
VRC=$?
set -e
echo "$REPORT" | sed -n '/每源统计/,/结论/p'
[ "$VRC" = "0" ] || { echo "$REPORT" | tail -20; fail "validate 报了 ERROR"; }

# 两个源都必须在 shared_buffer 里留下记录。只有 host 有的话说明 NPU 压根没访存；
# 只有 NPU 有的话说明 host 的写被 cache 吃掉了（那 uncacheable 映射就没生效）。
for src in host coralnpu; do
    echo "$REPORT" | awk -v s="$src:" '
        $1 == s { inblk = 1; next }
        /^  [a-z]+:$/ { inblk = 0 }
        inblk && $1 == "shared_buffer" { found = 1 }
        END { exit(found ? 0 : 1) }' || \
        fail "$src 在 shared_buffer 里一条记录都没有（详见上面的区域分布）"
done
echo "  ok   host 与 coralnpu 都在 shared_buffer 留下了记录"

# 存成文件再交给 python 读，而不是把报告文本插进 heredoc —— 报告里有中文和引号，
# 插值进 python 源码是自找麻烦。
python3 -m hettrace stats "$OUT" > "$OUT/stats.txt" 2>&1
sed -n '/两两共享/,/^$/p' "$OUT/stats.txt"
# 共享 line 数是"协同真的发生了"的定量证据：同一条 64B line 被两个源都碰过。
# in[64] + out[64] 各 256 字节 = 各 4 条 line，所以期望值是 8；这里只要求 > 0，
# 具体条数取决于 host 侧 cache line 的对齐，不该写死。
python3 - "$OUT/stats.txt" <<'PY' || fail "共享 line 数为 0 —— 两个源碰的不是同一批 line"
import re, sys
n = 0
for line in open(sys.argv[1], encoding="utf-8"):
    if "<->" in line:
        m = re.search(r"(\d+)\s*$", line.strip())
        if m:
            n += int(m.group(1))
print(f"  ok   两两共享合计 {n} 条 line" if n else "  FAIL 共享 0 条 line")
sys.exit(0 if n else 1)
PY

# ---- 4. 反向对照 -----------------------------------------------------------
echo
echo "---- 4/4 反向对照：--npu-no-share ----"
NEG_OUT=$(mktemp -d)
set +e
NEG_LOG=$(run_het "$NEG_OUT" "$M5OUT/neg" --npu-no-share 2>&1)
NEG_RC=$?
set -e
if [ "$NEG_RC" = "0" ]; then
    echo "$NEG_LOG" | grep -E "^host:" || true
    fail "对照组竟然通过了 —— NPU 用私有数组也能算对，说明正向那遍的共享是假的"
fi
# 必须是 host 的自检失败（校验和/结果不符），而不是崩了或者别的原因。
echo "$NEG_LOG" | grep -qE "host: 错误 —— (NPU 算的校验和|out\[)" || {
    echo "$NEG_LOG" | tail -12
    fail "对照组失败了，但不是因为数据不共享 —— 换了个原因，得看一眼"
}
echo "  ok   NPU 用私有数组时 host 校验失败，说明正向的字节共享是真的"
echo "$NEG_LOG" | grep -E "^host: 错误" | head -3 | sed 's/^/       /'
rm -rf "$NEG_OUT"

echo
echo "全部通过"
echo "trace 保留在 $OUT"
