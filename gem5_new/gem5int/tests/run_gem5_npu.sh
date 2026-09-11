#!/bin/bash
# gem5 里跑 CoralNPU 单设备，验收 SimObject 这一层。
#
#   GEM5_HOME=$HOME/gem5 CORALNPU_HOME=$HOME/coralnpu \
#       gem5int/tests/run_gem5_npu.sh
#
# 前提：gem5int/install.sh 跑过且 gem5.opt 编过，
#       coralnpuint/install.sh 跑过且 bazel build 过。
#
# 与 coralnpuint/tests/run_smoke.sh 的分工：那个在纯 C 里验设备库（时钟由测试
# 程序推、内存后端是测试程序里的数组），这个验的是真 gem5（时钟是事件队列、
# 内存后端是 gem5 timing DmaPort、时间戳是 curTick()）。两个都过才说明整条链没问题。
#
# 三步，第三步是重点：
#   1. 正向跑一遍，看内核跑完、trace 落盘。
#   2. hettrace validate，确认地址全落在 shared_buffer、时间戳单调。
#   3. **反向对照**：把 gem5 的 mem_ranges 挪开，让 shared_buffer 不再被任何
#      内存控制器覆盖。这时必须 fatal。如果它照样跑完，说明 AXI master 悄悄回落
#      到了库内私有 DDR 数组 —— 那"共享内存"就是假的，而正向那一遍完全看不出来。
#      这是本脚本存在的主要理由。

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
GEM5_HOME=${GEM5_HOME:-$HOME/gem5}
CORALNPU_HOME=${CORALNPU_HOME:-$HOME/coralnpu}
GEM5_BIN=${GEM5_BIN:-$GEM5_HOME/build/X86/gem5.opt}
CONFIG="$GEM5_HOME/configs/het/coralnpu_only.py"

fail() { echo "错误: $*" >&2; exit 1; }

[ -x "$GEM5_BIN" ] || fail "找不到 $GEM5_BIN，先 scons build/X86/gem5.opt"
[ -f "$CONFIG" ]   || fail "找不到 $CONFIG，先跑 gem5int/install.sh"

SO=$(readlink -f "$CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so" 2>/dev/null || true)
[ -n "$SO" ] && [ -f "$SO" ] || \
    fail "找不到 libcoralnpu-gem5.so，先 cd $CORALNPU_HOME && bazel build //gem5int:libcoralnpu-gem5.so"

# bazel-out 是符号链接，find 默认不跟进去。
BAZEL_OUT=$(readlink -f "$CORALNPU_HOME/bazel-out" 2>/dev/null || true)
ELF=$(find "$BAZEL_OUT" -path "*/gem5int/ddr_touch.elf" 2>/dev/null | head -1)
[ -n "$ELF" ] || fail "找不到 ddr_touch.elf，先 bazel build //gem5int:ddr_touch.elf"

echo "gem5:   $GEM5_BIN"
echo "库:     $SO"
echo "内核:   $ELF"

OUT=$(mktemp -d)
M5OUT=$(mktemp -d)
echo "HETTRACE_DIR=$OUT"

# ---- 1. 正向 ---------------------------------------------------------------
echo
echo "---- 1/3 正向跑 ----"
set +e
HETTRACE_DIR="$OUT" "$GEM5_BIN" --outdir="$M5OUT" "$CONFIG" \
    --library "$SO" --kernel "$ELF" 2>&1 | tail -6
RC=${PIPESTATUS[0]}
set -e
[ "$RC" = "0" ] || fail "gem5 返回 $RC（期望 0）"
[ -f "$OUT/coralnpu.hettrace" ] || fail "没有产出 coralnpu.hettrace"
[ -f "$OUT/coralnpu.hettrace.meta.json" ] || fail "没有产出 .meta.json 侧车文件"
echo "  ok   gem5 正常退出，trace 与侧车文件都在"

# ---- 2. 校验 ---------------------------------------------------------------
echo
echo "---- 2/3 hettrace validate ----"
export PYTHONPATH="$PROJ_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"
# 独立设备 bring-up 显式允许单源；文件内格式、时钟、地址、序号和 AXI 因果
# 仍按正式规则检查，只跳过跨源交接要求。
set +e
REPORT=$(python3 -m hettrace validate "$OUT" --allow-single-source 2>&1)
VRC=$?
set -e
echo "$REPORT" | sed -n '/每源统计/,/结论/p'
[ "$VRC" = "0" ] || fail "单源 validate 报了 ERROR"

# 侧车文件里的计数器是权威来源，比在报告文本里 grep 关键词可靠 —— 报告的措辞
# 会变，而且"报告里没提 non_monotonic"和"non_monotonic 为 0"是两件事。
python3 - "$OUT/coralnpu.hettrace.meta.json" <<'PY' || exit 1
import json, sys
m = json.load(open(sys.argv[1]))
bad = []
if m["emitted"] == 0:
    bad.append("emitted=0：一条记录都没有，tap 没接上或者内核没碰外部内存")
# 内核碰了地址映射里没有的区域 —— ddr_touch.cc 与 addrmap.json 脱钩了。
if m["unmapped"]:
    bad.append(f"unmapped={m['unmapped']}：内核地址与 addrmap.json 不一致")
# gem5 之外有人在推 CoralNPU 的时钟（设备库偷跑周期），时间基准就废了。
if m["non_monotonic"]:
    bad.append(f"non_monotonic={m['non_monotonic']}：时间戳回退，时钟不只由 gem5 推")
# 943 周期 × 2000 tick/周期。对不上说明 CoralNPU 的 clk_domain 没生效，
# 或者一次 gem5 事件推了不止一个 NPU 周期。
if m["clock_period_ticks"] != 2000:
    bad.append(f"clock_period_ticks={m['clock_period_ticks']}，期望 2000 (500MHz)")
if m["last_tick"] <= m["first_tick"]:
    bad.append("时间跨度为 0 —— 所有记录都盖在同一个 tick 上")
for b in bad:
    print(f"  FAIL {b}")
print(f"  ok   emitted={m['emitted']} unmapped=0 non_monotonic=0 "
      f"跨度={m['last_tick'] - m['first_tick']} tick" if not bad else "")
sys.exit(1 if bad else 0)
PY
echo "$REPORT" | grep -qE "shared_buffer +[0-9]+ +\(100\.0%\)" || \
    fail "记录没有 100% 落在 shared_buffer 里（详见上面的区域分布）"
echo "  ok   记录 100% 落在 shared_buffer"

# ---- 3. 反向对照 -----------------------------------------------------------
# 把 DRAM 挪到 npu_work，shared_buffer 就没有内存控制器覆盖了。
echo
echo "---- 3/3 反向对照：挪走 shared_buffer 的内存控制器 ----"
NEG=$(mktemp --suffix=.py)
sed 's/^DRAM_BASE = .*/DRAM_BASE = 0xB0000000/; s/^DRAM_SIZE = .*/DRAM_SIZE = 0x10000000/' \
    "$CONFIG" > "$NEG"
grep -q "DRAM_BASE = 0xB0000000" "$NEG" || fail "改写对照配置失败（$CONFIG 里的常量名变了？）"
set +e
NEG_LOG=$("$GEM5_BIN" --outdir="$M5OUT/neg" "$NEG" \
    --library "$SO" --kernel "$ELF" 2>&1)
NEG_RC=$?
set -e
rm -f "$NEG"
if [ "$NEG_RC" = "0" ]; then
    echo "$NEG_LOG" | tail -5
    fail "对照组竟然跑完了 —— AXI master 没走 gem5 内存，共享是假的"
fi
echo "$NEG_LOG" | grep -q "Unable to find destination" || {
    echo "$NEG_LOG" | tail -8
    fail "对照组失败了，但不是因为找不到内存 —— 换了个原因，得看一眼"
}
echo "  ok   shared_buffer 没有内存控制器时 gem5 fatal，说明访存真的走 gem5"

echo
echo "全部通过"
echo "trace 保留在 $OUT"
