#!/usr/bin/env bash
set -euo pipefail
if [[ ${AXI_PROFILE:-unified} == unified ]]; then
    exec bash "$(dirname -- "${BASH_SOURCE[0]}")/../../env/bootstrap.sh" "$@"
fi
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
archive="$AXI_PROJECT_DIR/../zhongxing-full-environment-20260831/source-archives/gem5.tar.gz"
revision=2721ed751edac7d4cf3df574c6e0293343a14ba2
expected=072ec046855aa9fb0ca63b5fa8ac47be6ffa1aecc64ea8029170896183a9f6c0
actual=$(sha256sum "$archive")
[[ ${actual%% *} == "$expected" ]] || { echo 'gem5 archive checksum mismatch' >&2; exit 1; }
mkdir -p "$AXI_ENV_ROOT"
if [[ ! -f "$AXI_GEM5_HOME/.source-revision" ]]; then
    [[ ! -e "$AXI_GEM5_HOME" ]] || { echo 'Refusing to overwrite an unmarked source directory' >&2; exit 1; }
    mkdir -p "$AXI_GEM5_HOME"
    tar -xzf "$archive" -C "$AXI_GEM5_HOME"
    echo "$revision" > "$AXI_GEM5_HOME/.source-revision"
fi
[[ $(cat "$AXI_GEM5_HOME/.source-revision") == "$revision" ]]
uv_bin=${AXI_UV:-"$HOME/.local/bin/uv"}
if [[ ! -x "$AXI_PYTHON" ]]; then
    if [[ -x "$uv_bin" ]]; then
        "$uv_bin" venv --python /usr/bin/python3 "$AXI_ENV_ROOT/venv"
    else
        python3 -m venv "$AXI_ENV_ROOT/venv"
    fi
fi
packages=(scons==4.5.2 setuptools==75.3.4 pydot==2.0.0 pyparsing==3.1.4 packaging==24.2)
if [[ -x "$uv_bin" ]]; then
    "$uv_bin" pip install --python "$AXI_PYTHON" "${packages[@]}"
else
    "$AXI_PYTHON" -m pip install "${packages[@]}"
fi
cd "$AXI_GEM5_HOME"
compiler=${AXI_CXX:-g++-10}
if [[ ! -f build/AXI/gem5.build/config ]]; then
    "$AXI_ENV_ROOT/venv/bin/scons" defconfig build/AXI build_opts/X86 CXX="$compiler"
fi
# Bundled Python modules import BaseKvmCPU unconditionally. Compile KVM support;
# the workload uses TimingSimpleCPU and never requires /dev/kvm.
"$AXI_ENV_ROOT/venv/bin/scons" setconfig build/AXI RUBY=n USE_KVM=y USE_SYSTEMC=y CXX="$compiler"
echo "Environment ready: $AXI_GEM5_HOME"
