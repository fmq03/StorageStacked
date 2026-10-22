#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/environment.sh"
# The public configuration directory owns the advanced SimX defaults.
if ! cmp -s "$SS_ROOT/third_party/gem5/runtime/defaults/simx.toml" "$VORTEX_HOME/VX_config.toml"; then
    cp "$SS_ROOT/third_party/gem5/runtime/defaults/simx.toml" "$VORTEX_HOME/VX_config.toml"
fi
"$AXI_PYTHON" "$SS_ROOT/third_party/gem5/runtime/prepare_vortex_llvm.py"
simx_flags=$("$AXI_PYTHON" -c 'import sys; from configure import load_config,simx_flags; print(simx_flags(load_config(sys.argv[1])))' "$1")
mkdir -p "$VORTEX_BUILD"
cd "$VORTEX_BUILD"
"$VORTEX_HOME/configure" --xlen=32 --tooldir="$SS_DEPS_ROOT/xpu-toolchains"
# CMake otherwise clones these sources during the build. Use the same pinned
# source cache for both network and packaged installations.
cmake_sources=(yaml-cpp spdlog argparse)
cmake_args=()
for dependency in "${cmake_sources[@]}"; do
    source_dir="$SS_DEPS_ROOT/cmake-sources/$dependency"
    [[ -f "$source_dir/CMakeLists.txt" ]] || { echo '请先执行 ./run.sh setup' >&2; exit 1; }
    cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_${dependency^^}=$source_dir")
done
cmake -S "$VORTEX_HOME/third_party/ramulator" -B "$VORTEX_HOME/third_party/ramulator/build" "${cmake_args[@]}"
cmake --build "$VORTEX_HOME/third_party/ramulator/build" --parallel "$BUILD_JOBS"
# SoftFloat hardcodes gcc in COMPILE_C; CC alone would be ignored.
softfloat_compile="$AXI_CC -c -Werror-implicit-function-declaration -DSOFTFLOAT_FAST_INT64 "'$(SOFTFLOAT_OPTS) $(C_INCLUDES) -O2 -o $@'
make -C "$VORTEX_HOME/third_party" CC="$AXI_CC" CXX="$AXI_CXX" COMPILE_C="$softfloat_compile" -j "$BUILD_JOBS"
# SimX's upstream makefile also includes Ramulator headers directly. Put the
# pinned include paths first, including when an old ext/ checkout still exists.
vortex_flags="-I$SS_DEPS_ROOT/cmake-sources/spdlog/include -I$SS_DEPS_ROOT/cmake-sources/yaml-cpp/include ${CXXFLAGS:-}"
env -u DEBUG CONFIGS="$simx_flags" CXXFLAGS="$vortex_flags" make -C sim/simx USE_GEM5=1 libvortex-gem5 -j "$BUILD_JOBS"
make -C sw/runtime/stub -j "$BUILD_JOBS"
make -C sw/runtime/gem5 HOST_ARCH=x86_64 -j "$BUILD_JOBS"
# The SDK builds user ELF files separately from the reusable device platform.
