#!/bin/bash
# 编译并运行 libcoralnpu-gem5.so 的 dlopen 冒烟测试。
#
#   CORALNPU_HOME=$HOME/coralnpu coralnpuint/tests/run_smoke.sh
#
# 前提：coralnpuint/install.sh 已跑过，且
#   bazel build //gem5int:libcoralnpu-gem5.so //gem5int:ddr_touch.elf
# 已完成。
#
# 会把 trace 写到一个临时 HETTRACE_DIR 并用 tools/hettrace 校验，所以这个脚本
# 同时覆盖了"设备库 -> .trace 文件 -> 解析工具"整条链路。

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$(dirname "$SELF_DIR")")
CORALNPU_HOME=${CORALNPU_HOME:-$HOME/coralnpu}

SO=$(readlink -f "$CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so" 2>/dev/null || true)
if [ -z "$SO" ] || [ ! -f "$SO" ]; then
    echo "错误: 找不到 libcoralnpu-gem5.so" >&2
    echo "      先跑: cd $CORALNPU_HOME && bazel build //gem5int:libcoralnpu-gem5.so" >&2
    exit 1
fi

# bazel-out 是个符号链接，find 默认不跟进去，所以先解析真实路径。
#
# 用 gem5int:ddr_touch.elf 而不是 tests/cocotb 里现成的内核：后者全在 TCM 里跑
# 完，一次 AXI master 访问都没有，tap 会输出 0 条记录 —— 那种"通过"什么都没验证。
BAZEL_OUT=$(readlink -f "$CORALNPU_HOME/bazel-out" 2>/dev/null || true)
ELF=$(find "$BAZEL_OUT" -path "*/gem5int/ddr_touch.elf" 2>/dev/null | head -1)
if [ -z "$ELF" ]; then
    echo "错误: 找不到测试内核 ddr_touch.elf" >&2
    echo "      先跑: cd $CORALNPU_HOME && bazel build //gem5int:ddr_touch.elf" >&2
    exit 1
fi
echo "内核: $ELF"

BIN=$(mktemp -d)/smoke
cc -std=c11 -Wall -Wextra -Werror -o "$BIN" "$SELF_DIR/smoke_dlopen.c" -ldl
echo "编译通过（纯 C，-Werror）—— ABI 确实是 C ABI"

OUT=$(mktemp -d)
echo "HETTRACE_DIR=$OUT"
echo

# set -e 会让失败的测试直接中断脚本，那样后面的 trace 诊断就看不到了 ——
# 而测试失败的时候恰恰最需要那些诊断。
set +e
HETTRACE_DIR="$OUT" "$BIN" "$SO" "$ELF"
RC=$?
set -e

echo
echo "---- trace 产物 ----"
ls -la "$OUT"
# 工具吃的是整个 trace 目录（要同时看 .hettrace 和 .meta.json 侧车文件），
# 不是单个文件。
if compgen -G "$OUT/*.hettrace" > /dev/null; then
    echo
    echo "---- hettrace validate ----"
    export PYTHONPATH="$PROJ_DIR/tools${PYTHONPATH:+:$PYTHONPATH}"
    set +e
    VALIDATE_LOG=$(python3 -m hettrace validate "$OUT" --allow-single-source 2>&1)
    VALIDATE_RC=$?
    set -e
    printf '%s\n' "$VALIDATE_LOG" | sed -n '1,30p'
    if [ "$VALIDATE_RC" -ne 0 ]; then
        echo "错误: 单源 trace 校验失败" >&2
        RC=$VALIDATE_RC
    fi
    echo
    echo "---- hettrace dump (前 6 条) ----"
    # dump 吃的是单个文件（它只解析记录，不看侧车），validate 吃的是目录。
    python3 -m hettrace dump "$OUT"/coralnpu.hettrace -n 6 2>&1 | head -12 || true
fi

rm -rf "$(dirname "$BIN")"
echo
echo "trace 保留在 $OUT"
exit $RC
