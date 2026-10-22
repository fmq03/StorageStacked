#!/usr/bin/env bash
set -euo pipefail
ss_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
export SS_ROOT="$ss_root"
source "$ss_root/third_party/gem5/runtime/settings.sh"
ss_deps="$SS_DEPS_ROOT"
mkdir -p "$ss_deps"
ss_deps=$(cd "$ss_deps" && pwd)
# Installed tools and CMake/SCons caches are location-specific, source/config are not.
python3 "$ss_root/third_party/gem5/runtime/relocate.py"
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
    tar -tjf "$archive" >/dev/null
    tar -xjf "$archive" -C "$ss_deps/bootstrap" bin/micromamba
fi
[[ $("$ss_mamba" --version) == 2.3.3 ]] || { echo '需要 micromamba 2.3.3' >&2; exit 1; }
if [[ -d "$ss_deps/toolchain/conda-meta" ]]; then
    # Explicit installs relink packages even when identical. Check first so a
    # repeated setup never rewrites a toolchain that may be compiling gem5.
    if ! diff -u \
        <(sed -n '/^https:/{s/#.*//;p;}' "$ss_root/third_party/gem5/runtime/defaults/conda-linux-64.lock" | sort) \
        <("$ss_mamba" --no-rc env export --explicit --prefix "$ss_deps/toolchain" | sed -n '/^https:/{s/#.*//;p;}' | sort); then
        echo '现有环境与锁文件不同，请指定新的 SS_DEPS_ROOT。' >&2
        exit 1
    fi
    echo '现有工具环境与锁文件一致，无需重新安装。'
else
    "$ss_mamba" --no-rc create -y --root-prefix "$ss_deps/mamba" \
        --prefix "$ss_deps/toolchain" --file "$ss_root/third_party/gem5/runtime/defaults/conda-linux-64.lock" "${offline_args[@]}"
fi
source "$ss_root/third_party/gem5/runtime/environment.sh"
"$AXI_PYTHON" "$SS_ROOT/third_party/gem5/runtime/setup_sources.py"
if [[ ! -d "$SS_DEPS_ROOT/xpu-sysroot/conda-meta" ]]; then
    "$ss_mamba" --no-rc create -y --root-prefix "$SS_DEPS_ROOT/mamba" -p "$SS_DEPS_ROOT/xpu-sysroot" --file "$SS_ROOT/third_party/gem5/runtime/defaults/xpu-runtime-linux-64.lock" "${offline_args[@]}"
fi
if [[ ! -x "$SS_DEPS_ROOT/xpu-toolchains/llvm-vortex/bin/clang" ]]; then
    "$AXI_PYTHON" "$SS_ROOT/third_party/gem5/runtime/fetch_vortex_tools.py"
fi
"$AXI_PYTHON" "$SS_ROOT/third_party/gem5/runtime/prepare_vortex_llvm.py"
"$AXI_PYTHON" "$SS_ROOT/third_party/gem5/runtime/prepare_cmake_sources.py"
printf '%s\n' "$ss_root" >"$ss_root/third_party/.cache/location"
echo "环境已就绪。下一步：./build.sh"
