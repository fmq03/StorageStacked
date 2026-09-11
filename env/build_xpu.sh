#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/activate.sh"
"$AXI_PYTHON" "$SS_ROOT/env/check_sources.py"
bash "$HET_PROJECT_ROOT/vortexint/install.sh"
bash "$HET_PROJECT_ROOT/coralnpuint/install.sh"
mkdir -p "$VORTEX_BUILD"
cd "$VORTEX_BUILD"
"$VORTEX_HOME/configure" --xlen=32 --tooldir="$SS_DEPS_ROOT/xpu-toolchains"
# CMake otherwise clones these sources during the build. Use the same pinned
# source cache for both network and packaged installations.
cmake_sources=(yaml-cpp spdlog argparse)
cmake_args=()
for dependency in "${cmake_sources[@]}"; do
    source_dir="$SS_DEPS_ROOT/cmake-sources/$dependency"
    [[ -f "$source_dir/CMakeLists.txt" ]] || { echo '请先执行 bash env/bootstrap_xpu.sh' >&2; exit 1; }
    cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_${dependency^^}=$source_dir")
done
cmake -S "$VORTEX_HOME/third_party/ramulator" -B "$VORTEX_HOME/third_party/ramulator/build" "${cmake_args[@]}"
cmake --build "$VORTEX_HOME/third_party/ramulator/build" --parallel 4
# SoftFloat hardcodes gcc in COMPILE_C; CC alone would be ignored.
softfloat_compile="$AXI_CC -c -Werror-implicit-function-declaration -DSOFTFLOAT_FAST_INT64 "'$(SOFTFLOAT_OPTS) $(C_INCLUDES) -O2 -o $@'
make -C "$VORTEX_HOME/third_party" CC="$AXI_CC" CXX="$AXI_CXX" COMPILE_C="$softfloat_compile" -j4
# SimX's upstream makefile also includes Ramulator headers directly. Put the
# pinned include paths first, including when an old ext/ checkout still exists.
vortex_flags="-I$SS_DEPS_ROOT/cmake-sources/spdlog/include -I$SS_DEPS_ROOT/cmake-sources/yaml-cpp/include ${CXXFLAGS:-}"
env -u DEBUG CXXFLAGS="$vortex_flags" make -C sim/simx USE_GEM5=1 libvortex-gem5 -j4
make -C sw/runtime/stub -j4
make -C sw/runtime/gem5 HOST_ARCH=x86_64 -j4
# Host optimization flags cannot be passed to the RISC-V compiler.
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS make -C tests/regression/vecadd -j4
make -C "$HET_PROJECT_ROOT/workloads/three_source"
make -C "$HET_PROJECT_ROOT/workloads/shared_buffer" CC="$AXI_CC"
cd "$CORALNPU_HOME"
"$SS_DEPS_ROOT/xpu-tools/bin/bazel" --output_user_root="$SS_DEPS_ROOT/bazel" build --jobs=6 \
    --define=storagestacked_native_cpp=1 \
    --repo_env="CC=$AXI_CC" --repo_env="CXX=$AXI_CXX" \
    --action_env="CC=$AXI_CC" --action_env="CXX=$AXI_CXX" \
    --repo_env="CPLUS_INCLUDE_PATH=$SS_DEPS_ROOT/xpu-native/include" \
    --action_env="CPLUS_INCLUDE_PATH=$SS_DEPS_ROOT/xpu-native/include" \
    --host_action_env="CPLUS_INCLUDE_PATH=$SS_DEPS_ROOT/xpu-native/include" \
    --linkopt="-L$SS_DEPS_ROOT/xpu-native/lib" --host_linkopt="-L$SS_DEPS_ROOT/xpu-native/lib" \
    //gem5int:libcoralnpu-gem5.so //gem5int:ddr_touch.elf
"$AXI_PYTHON" - <<'PY'
import os,shutil,subprocess
from pathlib import Path
root=Path(os.environ['SS_ROOT']);npu=Path(os.environ['CORALNPU_HOME'])
build=root/'build/xpu';build.mkdir(parents=True,exist_ok=True)
# cquery chooses the actual target configuration, without cache-path guessing.
cmd=[os.environ['SS_DEPS_ROOT']+'/xpu-tools/bin/bazel','--output_user_root='+os.environ['SS_DEPS_ROOT']+'/bazel',
     'cquery','--define=storagestacked_native_cpp=1','--output=files','//gem5int:ddr_touch.elf']
for kind in ('repo_env','action_env'):
    for key,envkey in [('CC','AXI_CC'),('CXX','AXI_CXX')]:
        cmd.append('--'+kind+'='+key+'='+os.environ[envkey])
for kind in ('repo_env','action_env','host_action_env'):
    cmd.append('--'+kind+'=CPLUS_INCLUDE_PATH='+os.environ['SS_DEPS_ROOT']+'/xpu-native/include')
for kind in ('linkopt','host_linkopt'):
    cmd.append('--'+kind+'=-L'+os.environ['SS_DEPS_ROOT']+'/xpu-native/lib')
files=subprocess.check_output(cmd,cwd=npu,text=True).splitlines()
elf=next(npu/s for s in files if s.endswith('/ddr_touch.elf'))
# Bazel outputs are read-only; replace the previous staged file on rebuild.
staged=build/'ddr_touch.elf'
staged.unlink(missing_ok=True)
shutil.copy2(elf,staged)
PY
bash "$SS_ROOT/env/build.sh"
