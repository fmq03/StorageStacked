#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
base=${STORAGE_RELEASE_BASE:-https://github.com/hy2581/zhongxing-storagestacked-offline/releases/download/${STORAGE_RELEASE_TAG:-offline-20261009-burst}}
destination=${1:-$here/downloads}
jobs=${STORAGE_DOWNLOAD_JOBS:-1}
stall_seconds=${STORAGE_STALL_SECONDS:-60}
request_seconds=${STORAGE_REQUEST_SECONDS:-900}
[[ $jobs =~ ^[1-9][0-9]*$ && $jobs -le 16 ]] || { echo 'STORAGE_DOWNLOAD_JOBS 必须为 1..16。' >&2; exit 2; }
for seconds in "$stall_seconds" "$request_seconds"; do
    [[ $seconds =~ ^[1-9][0-9]*$ ]] || { echo '超时秒数必须是正整数。' >&2; exit 2; }
done
[[ $# -le 1 ]] || { echo '用法：bash download.sh [下载目录]' >&2; exit 2; }
command -v curl >/dev/null || { echo '需要 curl 下载文件。' >&2; exit 1; }
# Older curl already uses HTTP/1.1 and does not know this explicit option.
http11=0
if curl --http1.1 --version >/dev/null 2>&1; then http11=1; fi
mkdir -p -- "$destination"
destination=$(cd -- "$destination" && pwd)
status_file=
cleanup() {
    local pid
    trap - EXIT INT TERM
    # Only signal jobs belonging to this shell; completed jobs are excluded.
    for pid in $(jobs -pr); do kill -TERM "$pid" 2>/dev/null || :; done
    wait 2>/dev/null || :
    [[ -z $status_file ]] || rm -f -- "$status_file"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
progress() {
    local target=$1 expected=$2 current
    while sleep 5; do
        current=0
        [[ ! -f $target ]] || current=$(stat -c '%s' -- "$target")
        printf '进度：%s %s/%s 字节（%s%%）\n' "${target##*/}" "$current" "$expected" "$((current*100/expected))" >&2
    done
}
request() {
    local url=$1 target=$2 resume=${3:-0} expected=${4:-0} attempt actual status code transfer_pid progress_pid
    local options
    for attempt in 1 2 3 4 5 6; do
        actual=0
        [[ ! -f $target ]] || actual=$(stat -c '%s' -- "$target")
        if [[ $expected -gt 0 && $actual -eq $expected ]]; then return 0; fi
        if [[ $expected -gt 0 && $actual -gt $expected ]]; then
            printf '临时文件过大（%s/%s 字节），重新下载：%s\n' "$actual" "$expected" "${target##*/}" >&2
            rm -f -- "$target"
            actual=0
        fi
        options=(--fail --location --silent --show-error --connect-timeout 30
                 --speed-limit 1 --speed-time "$stall_seconds" --max-time "$request_seconds")
        [[ $http11 == 0 ]] || options=(--http1.1 "${options[@]}")
        [[ $resume == 0 || $actual == 0 ]] || options+=(--continue-at -)
        printf '连接：%s，尝试 %s/6，当前 %s 字节\n' "${target##*/}" "$attempt" "$actual" >&2
        status_file=$(mktemp "$destination/.http-status.XXXXXXXX")
        curl "${options[@]}" "$url" -o "$target" --write-out '%{http_code}' > "$status_file" &
        transfer_pid=$!
        progress_pid=
        if [[ $expected -gt 0 ]]; then progress "$target" "$expected" & progress_pid=$!; fi
        if wait "$transfer_pid"; then code=0; else code=$?; fi
        if [[ -n $progress_pid ]]; then kill -TERM "$progress_pid" 2>/dev/null || :; wait "$progress_pid" 2>/dev/null || :; fi
        status=$(cat "$status_file")
        rm -f -- "$status_file"
        status_file=
        actual=0
        [[ ! -f $target ]] || actual=$(stat -c '%s' -- "$target")
        if [[ $expected -gt 0 && $actual -eq $expected ]]; then return 0; fi
        if [[ $code == 0 && $expected == 0 ]]; then return 0; fi
        if [[ $resume != 0 && ( $status == 416 || $code == 33 ) ]]; then
            echo '服务端拒绝续传，改为从头下载当前分块。' >&2
            rm -f -- "$target"
        elif [[ $expected -gt 0 && ( $actual -gt $expected || ( $code == 0 && $status == 200 ) ) ]]; then
            printf '返回大小不符（%s/%s 字节，HTTP %s），重新下载当前分块。\n' "$actual" "$expected" "$status" >&2
            rm -f -- "$target"
        fi
        [[ $attempt == 6 ]] && return 1
        printf '下载未完成，准备第 %s 次重试（curl=%s，HTTP=%s，%s/%s 字节）……\n' "$attempt" "$code" "$status" "$actual" "$expected" >&2
        sleep "$((attempt*5))"
    done
}
for name in PARTS.tsv extract.sh PACKAGE.json SPLIT_ACCEPTANCE.json run.sh docker-compat-entrypoint.py run-seccomp-check.sh DOCKER_COMPATIBILITY.md; do
    request "$base/$name" "$destination/$name.downloading"
    mv -- "$destination/$name.downloading" "$destination/$name"
done
IFS=$'\t' read -r first_name _ < "$destination/PARTS.tsv" || { echo '分块清单为空。' >&2; exit 1; }
bundle=${first_name%.tar.part-0000}
[[ $bundle =~ ^zhongxing-storagestacked-offline-[0-9]{8}(-[A-Za-z0-9._-]+)?$ && $first_name == "$bundle.tar.part-0000" ]] || {
    echo '分块清单的版本或首块名称无效。' >&2; exit 1;
}
fetch_part() {
    local name=$1 expected=$2 actual
    status_file=
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if [[ -f $destination/$name ]] && [[ $(stat -c '%s' -- "$destination/$name") == "$expected" ]]; then
        return
    fi
    printf '下载：%s\n' "$name"
    request "$base/$name" "$destination/$name.downloading" 1 "$expected"
    actual=$(stat -c '%s' -- "$destination/$name.downloading")
    [[ $actual == "$expected" ]] || { echo "下载大小不符：$name" >&2; return 1; }
    mv -- "$destination/$name.downloading" "$destination/$name"
    printf '完成：%s（%s 字节）\n' "$name" "$actual"
}
workers=()
failed=0
while IFS=$'\t' read -r name bytes extra; do
    [[ $name == "$bundle.tar.part-"[0-9][0-9][0-9][0-9] && $bytes =~ ^[0-9]+$ && -z $extra ]] || {
        echo '分块清单格式错误。' >&2; exit 1;
    }
    fetch_part "$name" "$bytes" &
    workers+=("$!")
    if [[ ${#workers[@]} == "$jobs" ]]; then
        for worker in "${workers[@]}"; do wait "$worker" || failed=1; done
        [[ $failed == 0 ]] || { echo '下载未完成；再次运行可继续下载。' >&2; exit 1; }
        workers=()
    fi
done < "$destination/PARTS.tsv"
# Bash 4.2/4.3 treat an empty array as unset with `set -u`.
if [[ ${#workers[@]} -gt 0 ]]; then
    for worker in "${workers[@]}"; do wait "$worker" || failed=1; done
fi
[[ $failed == 0 ]] || { echo '下载未完成；再次运行可继续下载。' >&2; exit 1; }
echo '全部分块已就绪，开始解包。'
bash "$destination/extract.sh" "$destination"
