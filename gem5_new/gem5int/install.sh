#!/bin/bash
# 把本项目的 gem5 侧源码装进 gem5 源码树。
#
#   GEM5_HOME=$HOME/gem5 gem5int/install.sh
#
# 装三类东西：
#   1. src/ 下的项目目录，按原路径镜像过去：
#        src/dev/coralnpu/   CoralNPU 设备
#        src/hettrace/       统一内存侧 AXI4 monitor
#        src/mem/unified_timing/  功能执行使用的稀疏定延迟内存
#      Vortex 那条腿不在这里：它的 gem5 侧源码 source-of-truth 在 Vortex 树里，
#      由 $VORTEX_HOME/sim/simx/gem5/install.sh 安装（见 docs/04-integration.md）。
#   2. libhettrace 的头 -> $GEM5_HOME/src/hettrace/（与 monitor 同目录）。
#      gem5 的 SCons 把 src/ 当作 include 根（这就是 gem5 自己
#      #include "mem/packet.hh" 能成立的原因），所以放在这里之后
#      #include "hettrace/writer.h" 直接可用，gem5 的构建文件一行都不用改。
#      统一 monitor 需要它；CoralNPU 设备库也带一份独立副本。
#   3. configs/het/ -> $GEM5_HOME/configs/het/，异构系统配置脚本。
#
# 幂等：重复运行只是刷新文件。
# 卸载：gem5int/install.sh --revert
#
# 除新增目录外，唯一的上游增量是 DmaPort byte-enable 重载：真实 AXI WSTRB 必须
# 随 timing request 到达权威内存，不能在设备侧用额外读事务合成 RMW。补丁在应用
# 前做 dry-run，重复安装幂等，--revert 会反向还原。

set -euo pipefail

SELF_DIR=$(dirname "$(readlink -f "$0")")
PROJ_DIR=$(dirname "$SELF_DIR")
GEM5_HOME=${GEM5_HOME:-$HOME/gem5}

REVERT=0
if [ "${1:-}" = "--revert" ]; then REVERT=1; fi

if [ ! -d "$GEM5_HOME/src/dev" ] || [ ! -f "$GEM5_HOME/SConstruct" ]; then
    echo "错误: GEM5_HOME=$GEM5_HOME 看起来不是 gem5 源码树" >&2
    echo "      (期望存在 $GEM5_HOME/SConstruct 和 $GEM5_HOME/src/dev/)" >&2
    exit 1
fi

# 要镜像过去的 src/ 子目录。三个都是本项目新增的目录，gem5 自带的文件一个都不
# 碰 —— 所以列在这里而不是用 find，"哪些目录属于本项目"是显式的，--revert 才敢
# 直接 rm -rf。
SRC_SUBDIRS="dev/coralnpu hettrace mem/unified_timing"
DMA_BE_PATCH="$SELF_DIR/patches/dma_byte_enable.patch"

apply_dma_be_patch() {
    if patch -R -p1 -s -f --dry-run -d "$GEM5_HOME" \
            -i "$DMA_BE_PATCH" >/dev/null 2>&1; then
        echo "  DmaPort byte-enable: 补丁已在，跳过"
        return 0
    fi
    if ! patch -p1 -s -f --dry-run -d "$GEM5_HOME" \
            -i "$DMA_BE_PATCH" >/dev/null 2>&1; then
        echo "错误: DmaPort byte-enable 补丁无法应用；gem5 上游可能已变化" >&2
        return 1
    fi
    cp -n "$GEM5_HOME/src/dev/dma_device.hh" \
        "$GEM5_HOME/src/dev/dma_device.hh.pre-hettrace" 2>/dev/null || true
    cp -n "$GEM5_HOME/src/dev/dma_device.cc" \
        "$GEM5_HOME/src/dev/dma_device.cc.pre-hettrace" 2>/dev/null || true
    patch -p1 -s -d "$GEM5_HOME" -i "$DMA_BE_PATCH"
    echo "  DmaPort byte-enable: 已打补丁"
}

revert_dma_be_patch() {
    if patch -R -p1 -s -f --dry-run -d "$GEM5_HOME" \
            -i "$DMA_BE_PATCH" >/dev/null 2>&1; then
        patch -R -p1 -s -d "$GEM5_HOME" -i "$DMA_BE_PATCH"
        echo "  DmaPort byte-enable: 已还原"
    else
        echo "  DmaPort byte-enable: 未打过，跳过"
    fi
}

if [ "$REVERT" = "1" ]; then
    echo "从 $GEM5_HOME 卸载:"
    revert_dma_be_patch
    for sub in $SRC_SUBDIRS; do
        rm -rf "$GEM5_HOME/src/$sub"
        echo "  删除 src/$sub/"
    done
    rm -rf "$GEM5_HOME/configs/het"
    echo "  删除 configs/het/"
    echo "完成。记得重新编 gem5（残留的 build/ 里还有旧的 .o 和生成的 params 头）。"
    exit 0
fi

echo "安装 gem5 侧源码到 $GEM5_HOME"

apply_dma_be_patch

# ---- 1. hettrace 头 ---------------------------------------------------------
HET_DIR="$GEM5_HOME/src/hettrace"
mkdir -p "$HET_DIR"
install -m 0644 "$PROJ_DIR"/libhettrace/include/hettrace/*.h "$HET_DIR/"
echo "  hettrace 头 -> $HET_DIR"

# ---- 2. src/ 子目录 ---------------------------------------------------------
for sub in $SRC_SUBDIRS; do
    src="$SELF_DIR/src/$sub"
    dst="$GEM5_HOME/src/$sub"
    # 分阶段开发时允许某个可选子目录暂时为空。
    if [ ! -d "$src" ] || [ -z "$(ls -A "$src" 2>/dev/null)" ]; then
        echo "  src/$sub: 本项目树里为空，跳过"
        continue
    fi
    mkdir -p "$dst"
    for f in "$src"/*; do
        [ -f "$f" ] || continue
        install -m 0644 "$f" "$dst/"
    done
    echo "  src/$sub -> $dst"
done

# ---- 3. 配置脚本 ------------------------------------------------------------
if [ -d "$SELF_DIR/configs/het" ] && [ -n "$(ls -A "$SELF_DIR/configs/het" 2>/dev/null)" ]; then
    mkdir -p "$GEM5_HOME/configs/het"
    for f in "$SELF_DIR"/configs/het/*; do
        [ -f "$f" ] || continue
        install -m 0644 "$f" "$GEM5_HOME/configs/het/"
    done
    echo "  configs/het -> $GEM5_HOME/configs/het"
else
    echo "  configs/het: 本项目树里为空，跳过"
fi

cat <<EOF

重新编 gem5：
  cd $GEM5_HOME && .venv/bin/scons build/X86/gem5.opt -j\$(nproc)

gem5 只负责功能执行与统一 AXI4 HETTrace 导出；DRAM 时序离线交给外部
mem_sim/hbm_sim。Vortex 自身的 SimX 构建仍保留 third_party/ramulator 依赖。

检查 CoralNPU 设备是否真的进去了（比"编译通过"更靠得住）：
  nm -C $GEM5_HOME/build/X86/gem5.opt | grep -c 'gem5::CoralNPU::'
  grep -n trace_enable $GEM5_HOME/build/X86/params/CoralNPU.hh

CoralNPU 设备库（library 参数指向它）另外用 bazel 构建，见
coralnpuint/install.sh 和 docs/04-integration.md。
EOF
