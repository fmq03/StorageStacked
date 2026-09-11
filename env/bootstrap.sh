#!/usr/bin/env bash
set -euo pipefail
ss_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ss_deps=${SS_DEPS_ROOT:-"$HOME/.local/share/storagestacked-unified"}
ss_mamba="$ss_deps/bootstrap/bin/micromamba"
mkdir -p "$ss_deps/bootstrap" "$ss_deps/downloads"
offline_args=()
[[ ${SS_OFFLINE:-0} != 1 ]] || offline_args+=(--offline)
if [[ ! -x "$ss_mamba" ]]; then
    archive="$ss_deps/downloads/micromamba-2.3.3.tar.bz2"
    if [[ ! -f "$archive" ]]; then
        [[ ${SS_OFFLINE:-0} != 1 ]] || { echo "离线缓存缺少 $archive" >&2; exit 1; }
        curl -fL --retry 3 https://micro.mamba.pm/api/micromamba/linux-64/2.3.3 -o "$archive"
    fi
    echo "e7274528ceb9c20d048a428d6c22d7e02e268f8ffb762c4c365422347c8b8ba2  $archive" | sha256sum -c -
    tar -xjf "$archive" -C "$ss_deps/bootstrap" bin/micromamba
fi
if [[ -d "$ss_deps/toolchain/conda-meta" ]]; then
    # Explicit installs relink packages even when identical. Check first so a
    # repeated setup never rewrites a toolchain that may be compiling gem5.
    if ! diff -u \
        <(sed -n '/^https:/p' "$ss_root/env/conda-linux-64.lock" | sort) \
        <("$ss_mamba" --no-rc env export --explicit --prefix "$ss_deps/toolchain" | sed -n '/^https:/p' | sort); then
        echo '现有环境与锁文件不同，请指定新的 SS_DEPS_ROOT。' >&2
        exit 1
    fi
    echo '现有工具环境与锁文件一致，无需重新安装。'
else
    "$ss_mamba" --no-rc create -y --root-prefix "$ss_deps/mamba" \
        --prefix "$ss_deps/toolchain" --file "$ss_root/env/conda-linux-64.lock" "${offline_args[@]}"
fi
echo "工具环境已准备好。下一步：bash $ss_root/env/build.sh"
