#!/usr/bin/env bash
set -euo pipefail
self=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${GEM5_HOME:?请设置 GEM5_HOME}"

case "${1:-}" in
    '') revert=0 ;;
    --revert) revert=1 ;;
    *) echo "用法: GEM5_HOME=<gem5> $0 [--revert]" >&2; exit 2 ;;
esac
if [[ ! -d "$GEM5_HOME/src/dev" || ! -f "$GEM5_HOME/SConstruct" ]]; then
    echo "错误: GEM5_HOME=$GEM5_HOME 看起来不是 gem5 源码树" >&2
    exit 1
fi

patch_file="$self/patches/dma_byte_enable.patch"
components=(dev/coralnpu dev/vortex mem/unified_timing)

apply_patch() {
    if patch -R -p1 -s -f --dry-run -d "$GEM5_HOME" -i "$patch_file" >/dev/null 2>&1; then
        echo "DmaPort byte-enable: 补丁已在，跳过"
        return
    fi
    patch -p1 -s -f --dry-run -d "$GEM5_HOME" -i "$patch_file"
    patch -p1 -s -d "$GEM5_HOME" -i "$patch_file"
    echo "DmaPort byte-enable: 已打补丁"
}

revert_patch() {
    if patch -R -p1 -s -f --dry-run -d "$GEM5_HOME" -i "$patch_file" >/dev/null 2>&1; then
        patch -R -p1 -s -d "$GEM5_HOME" -i "$patch_file"
        echo "DmaPort byte-enable: 已还原"
    else
        echo "DmaPort byte-enable: 未打过，跳过"
    fi
}

mirror_component() {
    local component=$1 source="$self/src/$1" destination="$GEM5_HOME/src/$1"
    mkdir -p "$destination"
    # These directories are owned by this project. Remove stale files first so
    # a deleted source cannot remain compiled in gem5 on a later rebuild.
    while IFS= read -r -d '' file; do
        local relative=${file#"$destination/"}
        [[ -f "$source/$relative" ]] || rm -f -- "$file"
    done < <(find "$destination" -type f -print0)
    while IFS= read -r -d '' link; do
        local relative=${link#"$destination/"}
        [[ -L "$source/$relative" ]] || rm -f -- "$link"
    done < <(find "$destination" -type l -print0)
    find "$destination" -depth -type d -empty -delete
    while IFS= read -r -d '' file; do
        local relative=${file#"$source/"}
        install -D -m 0644 "$file" "$destination/$relative"
    done < <(find "$source" -type f -print0)
    echo "镜像 src/$component"
}

if [[ $revert -eq 1 ]]; then
    echo "从 $GEM5_HOME 撤销统一设备安装:"
    revert_patch
    for component in "${components[@]}"; do
        rm -rf -- "$GEM5_HOME/src/$component"
        echo "删除 src/$component/"
    done
    echo "完成。统一在线流需要时请重新运行 env/build.sh 或 gem5int/install_devices.sh。"
    exit 0
fi

apply_patch
for component in "${components[@]}"; do
    mirror_component "$component"
done
