#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
deps=coralnpu/.cache/tools
mkdir -p "$deps/bootstrap" "$deps/downloads"
mamba="$deps/bootstrap/bin/micromamba"
if [[ ! -x "$mamba" ]]; then
    curl -fL --retry 3 https://micro.mamba.pm/api/micromamba/linux-64/2.3.3 -o "$deps/downloads/micromamba-2.3.3.tar.bz2"
    tar -xjf "$deps/downloads/micromamba-2.3.3.tar.bz2" -C "$deps/bootstrap" bin/micromamba
fi
if [[ ! -x "$deps/toolchain/bin/python" ]]; then
    args=()
    [[ ${SS_OFFLINE:-0} != 1 ]] || args+=(--offline)
    "$mamba" --no-rc create -y --root-prefix "$deps/mamba" --prefix "$deps/toolchain" \
        --file coralnpu/runtime/defaults/conda-linux-64.lock "${args[@]}"
fi
source coralnpu/runtime/environment.sh
"$AXI_PYTHON" coralnpu/runtime/setup_npu_tools.py
if [[ ! -f "$SS_DEPS_ROOT/xpu-native/lib/liblz4.so" ]]; then
    tar -xf "$SS_DEPS_ROOT/xpu-downloads/lz4-1.10.0.tar.gz" -C "$SS_DEPS_ROOT/xpu-downloads"
    make -C "$SS_DEPS_ROOT/xpu-downloads/lz4-1.10.0/lib" -j "$BUILD_JOBS" CC="$AXI_CC" PREFIX="$SS_DEPS_ROOT/xpu-native" install
fi
