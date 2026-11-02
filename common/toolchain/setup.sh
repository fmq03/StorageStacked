#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
prefix=${SS_TOOLCHAIN_PREFIX:-$repo/.cache/toolchain}
lock="$here/conda-linux-64.lock"
if [[ -d "$prefix/conda-meta" ]]; then
    python3 "$here/check.py" "$prefix" "$lock"
    printf '锁定工具链已就绪：%s\n' "$prefix"
    exit 0
fi
bootstrap="$repo/.cache/toolchain-bootstrap"
mamba="$bootstrap/bin/micromamba"
mkdir -p "$bootstrap"
if [[ ! -x "$mamba" ]]; then
    archive="$bootstrap/micromamba-2.3.3.tar.bz2"
    if [[ ! -f "$archive" ]]; then
        [[ ${SS_OFFLINE:-0} != 1 ]] || { echo "离线环境缺少 $archive" >&2; exit 1; }
        curl -fLsS --retry 3 https://micro.mamba.pm/api/micromamba/linux-64/2.3.3 -o "$archive"
    fi
    tar -tjf "$archive" >/dev/null
    tar -xjf "$archive" -C "$bootstrap" bin/micromamba
fi
[[ $("$mamba" --version) == 2.3.3 ]] || { echo '需要 micromamba 2.3.3' >&2; exit 1; }
args=()
[[ ${SS_OFFLINE:-0} != 1 ]] || args+=(--offline)
"$mamba" --no-rc create -y --root-prefix "$repo/.cache/toolchain-packages" \
    --prefix "$prefix" --file "$lock" "${args[@]}"
python3 "$here/check.py" "$prefix" "$lock"
