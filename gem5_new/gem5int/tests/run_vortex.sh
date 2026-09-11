#!/bin/bash
# Vortex 侧 tap 验收：让 gem5 把 Vortex SimX 跑起来，确认 tap 落出带 curTick()
# 时间戳的记录。
#
#   GEM5_HOME=$HOME/gem5 VORTEX_HOME=$HOME/vortex-gpu/vortex \
#       gem5int/tests/run_vortex.sh
#
# VORTEX_BUILD 默认是 $VORTEX_HOME/../vxbuild。**别把它放在 /tmp 下面** ——
# Ubuntu 开机会清 /tmp，重启之后 libvortex-gem5.so 就没了，而这个脚本报的是
# "找不到 .so"，看不出是被系统删的。
#
# 前提：
#   1. vortexint/install.sh 跑过（观测补丁和 timing-feedback 组合补丁在位）；
#   2. $VORTEX_HOME/sim/simx/gem5/install.sh 跑过且 gem5.opt 编过（Vortex 的
#      gem5 SimObject 源码 source-of-truth 在 Vortex 树里，不在本项目）；
#   3. Vortex 的 third_party 建过，且 libvortex-gem5.so 用 USE_GEM5=1 编过：
#        make -C $VORTEX_HOME/third_party
#        env -u DEBUG make -C $VORTEX_BUILD/sim/simx USE_GEM5=1 libvortex-gem5
#
# ---- 这个测试验什么、不验什么 ----
#
# 验：库能被 dlopen、SimObject 能构造、gem5 事件队列能推 Vortex 的 cycle()、内核
# 能跑完并终止、tap 能落出记录、时间戳与时钟周期都对。
#
# 不验：host 与 Vortex 之间的字节共享。那要走 CP 的 mem_upload 路径，需要真正的
# .vxbin，而 .vxbin 要 Vortex 的 LLVM 工具链。见 docs/03-limitations.md。
# 换句话说这是 run_gem5_npu.sh 的对应物（单设备验收），不是 run_het.sh 的。

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
GEM5_HOME=${GEM5_HOME:-$HOME/gem5}
VORTEX_HOME=${VORTEX_HOME:-$HOME/vortex-gpu/vortex}
VORTEX_BUILD=${VORTEX_BUILD:-$(dirname "$VORTEX_HOME")/vxbuild}
GEM5_BIN=${GEM5_BIN:-$GEM5_HOME/build/X86/gem5.opt}
CONFIG="$GEM5_HOME/configs/het/vortex_only.py"
SO="$VORTEX_BUILD/sim/simx/libvortex-gem5.so"
KERNEL="$PROJ_DIR/workloads/vortex_smoke/build/kernel.bin"

fail() { echo "错误: $*" >&2; exit 1; }

[ -x "$GEM5_BIN" ] || fail "找不到 $GEM5_BIN，先 scons build/X86/gem5.opt"
[ -f "$CONFIG" ]   || fail "找不到 $CONFIG，先跑 gem5int/install.sh"
[ -f "$SO" ] || fail "找不到 $SO
      先 env -u DEBUG make -C $VORTEX_BUILD/sim/simx USE_GEM5=1 libvortex-gem5
      （USE_GEM5=1 不能省 —— 默认只编 simx 可执行文件，不编这个库）"

# gem5 里有没有 VortexGPGPU 这个 SimObject，比"库在不在"更容易漏检查：库是
# Vortex 树里的产物，SimObject 是 gem5 树里的，两边分别安装、分别构建。
grep -q trace_enable "$GEM5_HOME/build/X86/params/VortexGPGPU.hh" 2>/dev/null || \
    fail "gem5 里的 VortexGPGPU 没有 trace_enable 参数
      说明 gem5 是用打补丁前的 VortexGPGPU.py 编的。依次跑：
        VORTEX_HOME=$VORTEX_HOME $PROJ_DIR/vortexint/install.sh
        GEM5_HOME=$GEM5_HOME $VORTEX_HOME/sim/simx/gem5/install.sh
        cd $GEM5_HOME && .venv/bin/scons build/X86/gem5.opt -j\$(nproc)"

# 内核每次都重建。它只有 88 字节，重建的代价可以忽略，而"改了 kernel.S 忘了
# make"会表现成记录数莫名变化，很难查。
make -s -C "$PROJ_DIR/workloads/vortex_smoke" >/dev/null || fail "kernel.bin 编不出来"
[ -f "$KERNEL" ] || fail "找不到 $KERNEL"

# libvortex-gem5.so 链的是 Vortex 自带的 third_party/ramulator。这一行不是保险
# 措施：如果机器上别处还装了一个 ramulator2（很常见，它是个独立项目），那份会
# 通过 LD_LIBRARY_PATH 抢在 .so 自己的 RUNPATH 之前被加载，于是 gem5 在
# dlopen 时死在 "undefined symbol: _ZN9Ramulator7Logging19_create_base_loggerEv"
# —— 一个完全看不出与 ramulator 版本有关的报错。
RAMULATOR_DIR="$VORTEX_HOME/third_party/ramulator"
[ -f "$RAMULATOR_DIR/libramulator.so" ] || \
    fail "找不到 $RAMULATOR_DIR/libramulator.so，先 make -C $VORTEX_HOME/third_party"
export LD_LIBRARY_PATH="$RAMULATOR_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "gem5:       $GEM5_BIN"
echo "Vortex 库:  $SO"
echo "内核:       $KERNEL ($(stat -c%s "$KERNEL") 字节)"

OUT=$(mktemp -d)
M5OUT=$(mktemp -d)
echo "HETTRACE_DIR=$OUT"

# ---- 1. 跑 ------------------------------------------------------------------
echo
echo "---- 1/3 gem5 驱动 Vortex 跑内核 ----"
set +e
LOG=$(HETTRACE_DIR="$OUT" "$GEM5_BIN" --outdir="$M5OUT" "$CONFIG" \
        --library "$SO" --kernel "$KERNEL" 2>&1)
RC=$?
set -e
echo "$LOG" | grep -E "VortexGPGPU: (vortex-gem5|memory trace|standalone kernel)|^---- 结束" \
    | sed 's/^.*info: /  /' || true
[ "$RC" = "0" ] || { echo "$LOG" | tail -20; fail "gem5 返回 $RC（期望 0）"; }
[ -f "$OUT/vortex.hettrace" ] || fail "没有产出 vortex.hettrace"
echo "  ok   内核跑完并终止，trace 落盘"

# ---- 2. 侧车计数器 ---------------------------------------------------------
echo
echo "---- 2/3 meta.json 的计数器 ----"
python3 - "$OUT/vortex.hettrace.meta.json" <<'PY' || exit 1
import json, sys
m = json.load(open(sys.argv[1]))
bad = []
if m["emitted"] == 0:
    bad.append("emitted=0 —— tap 没接上，或者内核的访存全被 Vortex 的 cache 吃了")
if m["unmapped"]:
    bad.append("unmapped=%d，地址与 addrmap.json 不一致" % m["unmapped"])
if m["non_monotonic"]:
    bad.append("non_monotonic=%d，时间戳回退" % m["non_monotonic"])
# 1GHz -> 1000 tick/周期。对不上说明 clk_domain 没生效。
if m["clock_period_ticks"] != 1000:
    bad.append("clock_period_ticks=%d，期望 1000" % m["clock_period_ticks"])
# level 0 = kLevelPostLlc。写死这个数是为了在有人改了 tap 挂点却忘了改 level
# 时能发现 —— level 是下游判断"这份 trace 能和谁比"的唯一依据。
if m["level"] != 0:
    bad.append("level=%d，期望 0 (kLevelPostLlc)" % m["level"])
# first_tick 必须是真的 gem5 tick：Vortex 自己数周期的话会从 0 或很小的数开始，
# 而设备是 1GHz、第一次访存至少要过几个周期，所以 >= 1000 是个宽松但有效的下界。
if m["first_tick"] < 1000:
    bad.append("first_tick=%d 太小 —— 时间戳可能不是 curTick()" % m["first_tick"])
for b in bad:
    print("  FAIL " + b)
if not bad:
    print("  ok   emitted=%d unmapped=0 non_monotonic=0 clock_period_ticks=1000"
          % m["emitted"])
    print("  ok   区间 [%d, %d]，起点远大于 0，时间戳来自 gem5"
          % (m["first_tick"], m["last_tick"]))
sys.exit(1 if bad else 0)
PY

# ---- 3. 区域分布 -----------------------------------------------------------
# 显式单源模式仍会做全部文件内检查，只跳过跨源交接要求。
echo
echo "---- 3/3 记录落在 vortex_vram ----"
export PYTHONPATH="$PROJ_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"
python3 -m hettrace validate "$OUT" --allow-single-source
python3 -m hettrace dump "$OUT/vortex.hettrace" -n 0 > "$OUT/dump.txt"
python3 - "$OUT/dump.txt" <<'PY' || exit 1
import sys
from collections import Counter
c = Counter()
for line in open(sys.argv[1], encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    c[line.split()[-1]] += 1
total = sum(c.values())
for reg, n in c.most_common():
    print("       %-16s %4d (%.1f%%)" % (reg, n, 100.0 * n / total))
bad = []
if not c["vortex_vram"]:
    bad.append("vortex_vram 里一条记录都没有 —— 内核的数据访存没有出核")
# 取指落在 host_heap 是本 smoke test 的已知产物：flat-image 的装载地址在设备库
# 里写死成 0x80000000。数量必须很小（88 字节代码 = 2 条 line）；若变多，说明
# 内核跑飞了在乱取指，那才是真问题。
if c["host_heap"] > 4:
    bad.append("host_heap 里有 %d 条记录（期望 <= 4 条取指）—— 内核可能跑飞了"
               % c["host_heap"])
for k in c:
    if k not in ("vortex_vram", "host_heap"):
        bad.append("意外的区域 %s，共 %d 条" % (k, c[k]))
for b in bad:
    print("  FAIL " + b)
if not bad:
    print("  ok   %d 条落在 vortex_vram，%d 条取指落在 host_heap（已知产物）"
          % (c["vortex_vram"], c["host_heap"]))
sys.exit(1 if bad else 0)
PY

# 全是读、一条写都没有，是 Vortex 的 dcache 是 writeback 且内核结束前没有 flush
# 的结果：写命中留在 cache 里，脏行没被换出就没到 DRAM；而 write-allocate 让每
# 次写 miss 先产生一次读。这是"写记录只在换出时出现"的教科书例子，不是 bug，
# 见 docs/03-limitations.md。这里只提示，不作为失败条件。
if ! grep -qE "^\s+[0-9]+\s+W\s" "$OUT/dump.txt"; then
    echo "  note 全部记录都是读 —— dcache 是 writeback，脏行未换出（详见 03-limitations.md）"
fi

echo
echo "全部通过"
echo "trace 保留在 $OUT"
