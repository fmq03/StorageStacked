#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/activate.sh"
"$AXI_PYTHON" "$SS_ROOT/env/check_sources.py"
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/patch_gem5.py" "$GEM5_HOME"
bash "$HET_PROJECT_ROOT/gem5int/install_devices.sh"
cmake -S "$MEMSIM_HOME" -B "$MEMSIM_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$AXI_CXX"
cmake --build "$MEMSIM_BUILD" -j "${AXI_JOBS:-6}"
cd "$GEM5_HOME"
build_args=("CXX=$AXI_CXX" "CC=$AXI_CC" "PYTHON_CONFIG=$PYTHON_CONFIG"
    "EXTRAS=$AXI_PROJECT_DIR:$HET_PROJECT_ROOT/gem5int/src/hettrace")
if [[ ! -f build/AXI/gem5.build/config ]]; then
    "$SS_PREFIX/bin/scons" defconfig build/AXI build_opts/X86 "${build_args[@]}"
fi
"$SS_PREFIX/bin/scons" setconfig build/AXI RUBY=n USE_KVM=y USE_SYSTEMC=y "${build_args[@]}"
"$SS_PREFIX/bin/scons" build/AXI/gem5.opt "${build_args[@]}" "-j${AXI_JOBS:-6}"
