#!/usr/bin/env bash
# 仅修改上游 SimX/ABI；gem5 设备源码在 gem5int/src/dev/vortex 直接维护。
set -euo pipefail
self=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project=$(dirname "$self")
: "${VORTEX_HOME:?请设置 VORTEX_HOME}"
patch_file="$self/patches/simx_online.patch"
if [[ ${1:-} == --revert ]]; then
    if patch -R -p1 -s -f --dry-run -d "$VORTEX_HOME" -i "$patch_file" >/dev/null 2>&1; then
        patch -R -p1 -s -d "$VORTEX_HOME" -i "$patch_file"
    fi
    exit 0
fi
if ! patch -R -p1 -s -f --dry-run -d "$VORTEX_HOME" -i "$patch_file" >/dev/null 2>&1; then
    patch -p1 -s -f --dry-run -d "$VORTEX_HOME" -i "$patch_file"
    patch -p1 -s -d "$VORTEX_HOME" -i "$patch_file"
fi
mkdir -p "$VORTEX_HOME/sim/simx/hettrace"
install -m 0644 "$project"/libhettrace/include/hettrace/*.h "$VORTEX_HOME/sim/simx/hettrace/"
install -m 0644 "$self/vortex_trace.h" "$VORTEX_HOME/sim/simx/gem5/"
echo 'Vortex SimX 在线访存补丁已准备好；gem5 设备使用系统内的源码。'
