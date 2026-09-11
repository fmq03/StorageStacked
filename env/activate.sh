#!/usr/bin/env bash
# Source this file. One Linux x86-64 toolchain for build, workloads and checks.
export SS_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export SS_DEPS_ROOT=${SS_DEPS_ROOT:-"$HOME/.local/share/storagestacked-unified"}
export SS_PREFIX="$SS_DEPS_ROOT/toolchain"
export MAMBA_ROOT_PREFIX="$SS_DEPS_ROOT/mamba"
if [[ ! -x "$SS_PREFIX/bin/python" ]]; then
    echo '请先执行 bash env/bootstrap.sh' >&2
    return 1
fi
eval "$("$SS_DEPS_ROOT/bootstrap/bin/micromamba" shell hook --shell bash)"
micromamba activate "$SS_PREFIX"
export GEM5_HOME="$SS_ROOT/gem5"
export HET_PROJECT_ROOT="$SS_ROOT/gem5_new"
export CORALNPU_HOME="$SS_ROOT/coralnpu"
export VORTEX_HOME="$SS_ROOT/vortex-gpu/vortex"
export VORTEX_BUILD="$SS_ROOT/vortex-gpu/vxbuild"
export MEMSIM_HOME="$SS_ROOT/mem_sim"
export MEMSIM_BUILD="$MEMSIM_HOME/build-unified"
export MEMSIM_BIN="$MEMSIM_BUILD/hbm_sim"
export AXI_PROJECT_DIR="$SS_ROOT/gem5_axi"
export AXI_PROFILE=unified
export AXI_ENV_ROOT="$SS_DEPS_ROOT/runs"
export AXI_GEM5_HOME="$GEM5_HOME"
export AXI_GEM5_BIN="$GEM5_HOME/build/AXI/gem5.opt"
export AXI_PYTHON="$SS_PREFIX/bin/python"
export AXI_CXX="$SS_PREFIX/bin/x86_64-conda-linux-gnu-g++"
export AXI_CC="$SS_PREFIX/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$AXI_CXX"
export CC="$AXI_CC"
export CPATH="$SS_PREFIX/include"
export LIBRARY_PATH="$SS_PREFIX/lib"
export LD_LIBRARY_PATH="$SS_PREFIX/lib"
export PYTHON_CONFIG="$SS_PREFIX/bin/python3-config"
export PYTHONPATH="$HET_PROJECT_ROOT/tools${PYTHONPATH:+:$PYTHONPATH}"
mkdir -p "$AXI_ENV_ROOT"
