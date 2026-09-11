#!/bin/bash
# 把 CoralNPU 侧的设备库与 trace tap 装进 CoralNPU 源码树。
#
#   CORALNPU_HOME=$HOME/coralnpu coralnpuint/install.sh
#
# 做两件事：
#   1. 新建一个独立 package $CORALNPU_HOME/gem5int/，把 ABI 实现、tap 和
#      libhettrace 的头全部放进去。独立目录不会与上游冲突。
#   2. 对三个 hw_sim 文件应用四个纯增量补丁：
#        core_mini_axi_wrapper.h  加 halted()/wfi() 非阻塞访问器和异步 AXI
#                                 request/response seam；
#        hw_primitives.h          让 AXI B/R 响应可以由后续 gem5 timing 事件
#                                 注入，RTL 会在真实 READY/VALID 上停等；
#        BUILD                    把 core_mini_axi_wrapper 的 visibility 放到
#                                 public，否则 //gem5int 依赖不到它。
#
# 幂等：重复运行会跳过已打过的补丁。
# 卸载：coralnpuint/install.sh --revert

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$SELF_DIR")
CORALNPU_HOME=${CORALNPU_HOME:-$HOME/coralnpu}

REVERT=0
if [ "${1:-}" = "--revert" ]; then REVERT=1; fi

GEM5INT_DIR="$CORALNPU_HOME/gem5int"
HW_SIM_DIR="$CORALNPU_HOME/hw_sim"

# "被打补丁的文件名:补丁文件名"。补丁都是 -p0 且只作用于单个文件。
PATCH_SPECS="hw_primitives.h:hw_primitives_async_axi.patch
BUILD:hw_sim_BUILD.patch"
CORE_FILE="core_mini_axi_wrapper.h"
CORE_BASE_PATCH="core_mini_axi_wrapper.h.patch"
CORE_ASYNC_PATCH="core_mini_axi_wrapper_async.patch"

if [ ! -f "$HW_SIM_DIR/core_mini_axi_wrapper.h" ]; then
    echo "错误: CORALNPU_HOME=$CORALNPU_HOME 看起来不是 CoralNPU 源码树" >&2
    echo "      (期望存在 $HW_SIM_DIR/core_mini_axi_wrapper.h)" >&2
    exit 1
fi

# 判断当前状态用 patch -R --dry-run：能反向应用说明已经打过了。
apply_patch() {
    local f="$1" p="$SELF_DIR/patches/$2"
    if patch -R -p0 -s -f --dry-run -i "$p" "$HW_SIM_DIR/$f" >/dev/null 2>&1; then
        echo "  hw_sim/$f: 补丁已在，跳过"
        return 0
    fi
    if ! patch -p0 -s -f --dry-run -i "$p" "$HW_SIM_DIR/$f" >/dev/null 2>&1; then
        echo "错误: hw_sim/$f 的补丁无法应用 —— 上游大概改过这个文件。" >&2
        echo "      需要重新生成 $p（见 docs/04-integration.md）。" >&2
        return 1
    fi
    cp -n "$HW_SIM_DIR/$f" "$HW_SIM_DIR/$f.pre-hettrace" 2>/dev/null || true
    patch -p0 -s -i "$p" "$HW_SIM_DIR/$f"
    echo "  hw_sim/$f: 已打补丁 (原文件备份为 $f.pre-hettrace)"
}

revert_patch() {
    local f="$1" p="$SELF_DIR/patches/$2"
    if patch -R -p0 -s -f --dry-run -i "$p" "$HW_SIM_DIR/$f" >/dev/null 2>&1; then
        patch -R -p0 -s -i "$p" "$HW_SIM_DIR/$f"
        echo "  hw_sim/$f: 已还原"
    else
        echo "  hw_sim/$f: 未打过补丁，跳过"
    fi
}

# 两个 core wrapper 补丁必须视为一个有顺序的 patch stack。async 补丁建立在
# base 补丁之上，会改变 base hunk 后面的上下文；若逐个做 base 的 reverse
# dry-run，patch 可能误判成“尚未安装”并把同一段代码再次插入。
apply_core_patch_stack() {
    local async="$SELF_DIR/patches/$CORE_ASYNC_PATCH"
    if patch -R -p0 -s -f --dry-run -i "$async" \
            "$HW_SIM_DIR/$CORE_FILE" >/dev/null 2>&1; then
        echo "  hw_sim/$CORE_FILE: base + async 补丁已在，跳过"
        return 0
    fi
    apply_patch "$CORE_FILE" "$CORE_BASE_PATCH"
    apply_patch "$CORE_FILE" "$CORE_ASYNC_PATCH"
}

revert_core_patch_stack() {
    # 严格逆序；async 去掉后 base 的原始上下文才恢复。
    revert_patch "$CORE_FILE" "$CORE_ASYNC_PATCH"
    revert_patch "$CORE_FILE" "$CORE_BASE_PATCH"
}

if [ "$REVERT" = "1" ]; then
    echo "还原 CoralNPU gem5 集成:"
    revert_patch "$CORE_FILE" "nonblocking_start.patch"
    revert_core_patch_stack
    for spec in $PATCH_SPECS; do
        revert_patch "${spec%%:*}" "${spec##*:}"
    done
    rm -rf "$GEM5INT_DIR"
    echo "  已删除 $GEM5INT_DIR"
    echo "完成。"
    exit 0
fi

# 上游 Bazel/Verilator 必须支持原生 C++，防止引入第二套 SystemC。
NATIVE_PATCH="$SELF_DIR/patches/native_cpp_rules.patch"
install -m 0644 "$SELF_DIR/patches/0018-Optional-SystemC-for-native-Cpp.patch" "$CORALNPU_HOME/third_party/rules_hdl/"
if ! patch -R -p1 -s -f --dry-run -d "$CORALNPU_HOME" -i "$NATIVE_PATCH" >/dev/null 2>&1; then
    patch -p1 -s -f --dry-run -d "$CORALNPU_HOME" -i "$NATIVE_PATCH"
    patch -p1 -s -d "$CORALNPU_HOME" -i "$NATIVE_PATCH"
fi

echo "安装 CoralNPU gem5 集成到 $CORALNPU_HOME"

mkdir -p "$GEM5INT_DIR/hettrace"
install -m 0644 "$PROJ_DIR"/libhettrace/include/hettrace/*.h "$GEM5INT_DIR/hettrace/"
install -m 0644 "$SELF_DIR/coralnpu_trace.h" "$GEM5INT_DIR/"
install -m 0644 "$SELF_DIR/coralnpu_gem5.h"  "$GEM5INT_DIR/"
install -m 0644 "$SELF_DIR/coralnpu_gem5.cc" "$GEM5INT_DIR/"
# 测试内核。BUILD 里的 coralnpu_v2_binary(name = "ddr_touch") 引用它，漏掉这个
# 文件的话 bazel 会在解析阶段就报 "missing input file"。
install -m 0644 "$SELF_DIR/ddr_touch.cc"     "$GEM5INT_DIR/"
# 链接器版本脚本。BUILD 里两个 cc_binary 的 additional_linker_inputs 引用它。
install -m 0644 "$SELF_DIR/coralnpu_gem5.map" "$GEM5INT_DIR/"
install -m 0644 "$SELF_DIR/BUILD.bazel"      "$GEM5INT_DIR/BUILD"
echo "  gem5int/ -> $GEM5INT_DIR"

apply_core_patch_stack
apply_patch "$CORE_FILE" "nonblocking_start.patch"
for spec in $PATCH_SPECS; do
    apply_patch "${spec%%:*}" "${spec##*:}"
done

# 把补丁栈幂等性变成安装时不变量，避免重复方法一直拖到 C++ 编译才暴露。
HALTED_COUNT=$(grep -c 'bool halted() const' "$HW_SIM_DIR/$CORE_FILE" || true)
ASYNC_READ_COUNT=$(grep -c 'void RegisterAsyncReadCallback' \
    "$HW_SIM_DIR/$CORE_FILE" || true)
if [ "$HALTED_COUNT" -ne 1 ] || [ "$ASYNC_READ_COUNT" -ne 1 ]; then
    echo "错误: hw_sim/$CORE_FILE 补丁栈重复或缺失" >&2
    echo "      halted=$HALTED_COUNT async_read=$ASYNC_READ_COUNT（都应为 1）" >&2
    exit 1
fi
echo "  core patch stack: halted=1 async_read=1（幂等性检查通过）"

cat <<EOF

接下来：

1) 构建设备库（首次会跑 Verilator，很慢；libcore_mini_axi_cc_library.a
   已经存在的话就快）：
     cd $CORALNPU_HOME && bazel build //gem5int:libcoralnpu-gem5.so
   产物 bazel-bin/gem5int/libcoralnpu-gem5.so 即 gem5 CoralNPU SimObject 的
   library 参数。要 RVV 就换 //gem5int:libcoralnpu-gem5-rvv.so。

2) gem5 侧：
     GEM5_HOME=\$GEM5_HOME $PROJ_DIR/gem5int/install.sh
     cd \$GEM5_HOME && .venv/bin/scons build/X86/gem5.opt -j\$(nproc)
EOF
