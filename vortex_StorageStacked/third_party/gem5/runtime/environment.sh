#!/usr/bin/env bash
# Source this file. One Linux x86-64 toolchain for build, workloads and checks.
# Host preload settings can inject an incompatible C++ runtime into private LLVM.
unset LD_PRELOAD LD_AUDIT PYTHONOPTIMIZE
export PYTHONDONTWRITEBYTECODE=1
export SS_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
source "$SS_ROOT/third_party/gem5/runtime/settings.sh"
mkdir -p "$SS_DEPS_ROOT"
export SS_DEPS_ROOT=$(cd "$SS_DEPS_ROOT" && pwd)
export SS_PREFIX="$SS_DEPS_ROOT/toolchain"
export MAMBA_ROOT_PREFIX="$SS_DEPS_ROOT/mamba"
if [[ ! -x "$SS_PREFIX/bin/python" ]]; then
    echo '请先在仓库根目录执行 ./build.sh' >&2
    return 1
fi
eval "$("$SS_DEPS_ROOT/bootstrap/bin/micromamba" shell hook --shell bash)"
micromamba activate "$SS_PREFIX"
export GEM5_HOME="$SS_ROOT/third_party/gem5"
export HET_PROJECT_ROOT="$SS_ROOT/third_party/gem5/src"
export DEVICE_HOME="$SS_ROOT/third_party/vortex"
export DEVICE_BUILD="$SS_ROOT/third_party/.cache/build/vortex"
export STORAGE_STACK_ROOT=$("$SS_PREFIX/bin/python" -c 'import sys; sys.path.insert(0,sys.argv[1]); from paths import STORAGE_ROOT; print(STORAGE_ROOT)' "$SS_ROOT/third_party/gem5/runtime")
if [[ ! -f "$STORAGE_STACK_ROOT/storage_axi/storage_config.hh" ]]; then
    echo "缺少独立存储项目：请执行 build.sh --storage 配置相对路径" >&2
    return 1
fi
export STORAGE_STACK_ROOT=$(cd "$STORAGE_STACK_ROOT" && pwd)
export MEMSIM_HOME="$STORAGE_STACK_ROOT/mem_sim"
export MEMSIM_BUILD="$SS_ROOT/third_party/.cache/build/memsim"
export MEMSIM_BIN="$MEMSIM_BUILD/hbm_sim"
export AXI_PROJECT_DIR="$SS_ROOT/integration"
export AXI_PROFILE=unified
export AXI_ENV_ROOT="$SS_DEPS_ROOT/runs"
export AXI_GEM5_HOME="$GEM5_HOME"
export AXI_GEM5_BIN="$GEM5_HOME/build/AXI/gem5.opt"
export AXI_PYTHON="$SS_PREFIX/bin/python"
export AXI_CXX="$SS_PREFIX/bin/x86_64-conda-linux-gnu-g++"
export AXI_CC="$SS_PREFIX/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$AXI_CXX"
export CC="$AXI_CC"
# Upstream makefiles invoke gcc/g++ directly; keep those on the same toolchain.
mkdir -p "$SS_DEPS_ROOT/host-bin" "$SS_DEPS_ROOT/tmp" "$SS_DEPS_ROOT/tmp/ccache" "$SS_DEPS_ROOT/cache"
ln -sfn "$AXI_CC" "$SS_DEPS_ROOT/host-bin/gcc"
ln -sfn "$AXI_CXX" "$SS_DEPS_ROOT/host-bin/g++"
# Keep build tools independent of the calling shell.
export PATH="$SS_DEPS_ROOT/host-bin:$SS_DEPS_ROOT/xpu-tools/bin:$SS_PREFIX/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export GEM5_SCONS="$SS_PREFIX/bin/scons"
export TMPDIR="$SS_DEPS_ROOT/tmp"
export XDG_CACHE_HOME="$SS_DEPS_ROOT/cache"
export CCACHE_TEMPDIR="$SS_DEPS_ROOT/tmp/ccache"
export CPATH="$SS_PREFIX/include"
export LIBRARY_PATH="$SS_PREFIX/lib"
export LD_LIBRARY_PATH="$SS_PREFIX/lib"
export PYTHON_CONFIG="$SS_PREFIX/bin/python3-config"
export PYTHONPATH="$SS_ROOT/third_party/gem5/runtime"
mkdir -p "$AXI_ENV_ROOT"
export VORTEX_HOME="$DEVICE_HOME"
export VORTEX_BUILD="$DEVICE_BUILD"

export PYTHONPATH="$STORAGE_STACK_ROOT/scripts${PYTHONPATH:+:$PYTHONPATH}"
