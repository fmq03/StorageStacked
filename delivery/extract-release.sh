#!/usr/bin/env bash
# Restore the complete offline delivery from ordered, size-checked parts.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
manifest=$here/PARTS.tsv
output=${1:-$here}
[[ $# -le 1 ]] || { echo '用法：bash extract.sh [解压位置]' >&2; exit 2; }
[[ -f $manifest ]] || { echo "缺少分块清单：$manifest" >&2; exit 1; }
patch_files=(run.sh docker-compat-entrypoint.py run-seccomp-check.sh DOCKER_COMPATIBILITY.md)
for name in "${patch_files[@]}"; do
    [[ -f $here/$name ]] || { echo "缺少启动补丁：$here/$name；请使用最新版 download.sh 下载，或从 Release 下载这四个文件。" >&2; exit 1; }
done
IFS=$'\t' read -r first_name _ < "$manifest" || { echo '分块清单为空。' >&2; exit 1; }
bundle=${first_name%.tar.part-0000}
[[ $bundle =~ ^zhongxing-storagestacked-offline-[0-9]{8}(-[A-Za-z0-9._-]+)?$ && $first_name == "$bundle.tar.part-0000" ]] || {
    echo '分块清单的版本或首块名称无效。' >&2; exit 1;
}
pieces=()
total=0
while IFS=$'\t' read -r name expected extra; do
    [[ $expected =~ ^[0-9]+$ && -z $extra ]] || {
        echo '分块清单格式错误。' >&2; exit 1;
    }
    printf -v ordinal '%04d' "${#pieces[@]}"
    [[ $name == "$bundle.tar.part-$ordinal" ]] || { echo '分块编号缺失或顺序错误。' >&2; exit 1; }
    [[ -f $here/$name ]] || { echo "缺少分块：$name" >&2; exit 1; }
    actual=$(stat -c '%s' -- "$here/$name")
    [[ $actual == "$expected" && $expected -gt 0 && $expected -le 25000000 ]] || {
        echo "分块大小不符：$name，应为 $expected 字节，实际 $actual 字节。" >&2; exit 1;
    }
    pieces+=("$here/$name")
    total=$((total+expected))
done < "$manifest"
[[ ${#pieces[@]} -gt 0 ]] || { echo '分块清单为空。' >&2; exit 1; }
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
[[ ! -e $output/$bundle && ! -L $output/$bundle ]] || {
    echo "输出目录已存在：$output/$bundle；请指定新的解压位置。" >&2; exit 1;
}
stage=$(mktemp -d "$output/.storagestacked-extract.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
printf '合并并解包 %s 块，共 %s 字节……\n' "${#pieces[@]}" "$total"
cat -- "${pieces[@]}" | tar -xf - -C "$stage"
restored=$stage/$bundle
for name in run.sh 使用说明.md SOURCE_VERSIONS.json DELIVERY.json ACCEPTANCE.json VALIDATION.json zhongxing-storagestacked-offline.tar.gz; do
    [[ -f $restored/$name ]] || { echo "解包后缺少文件：$name" >&2; exit 1; }
done
cp -- "$restored/run.sh" "$restored/run-original.sh"
for extra in "${patch_files[@]}"; do
    cp -- "$here/$extra" "$restored/$extra"
done
mv -- "$restored" "$output/$bundle"
printf '解包完成：%s\n运行：cd "%s" && bash run.sh\n' "$output/$bundle" "$output/$bundle"
