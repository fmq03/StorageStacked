#!/usr/bin/env bash
unset LD_PRELOAD LD_AUDIT
export SS_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
export SS_DEPS_ROOT="$SS_ROOT/coralnpu/.cache/tools"
export SS_PREFIX="$SS_DEPS_ROOT/toolchain"
export AXI_PYTHON="$SS_PREFIX/bin/python"
if [[ ! -x "$AXI_PYTHON" ]]; then
    echo '工具环境尚未准备，请在仓库根目录执行 ./build.sh' >&2
    return 1
fi
export STORAGE_STACK_ROOT=$("$AXI_PYTHON" -c 'import sys; sys.path.insert(0,sys.argv[1]); from paths import STORAGE_ROOT; print(STORAGE_ROOT)' "$SS_ROOT/coralnpu/runtime")
export BUILD_JOBS=$("$AXI_PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["jobs"])' "$SS_ROOT/coralnpu/runtime/paths.json")
export CORALNPU_HOME="$SS_ROOT/coralnpu"
export MEMSIM_BUILD="$SS_ROOT/coralnpu/.cache/build/memsim"
export AXI_CXX="$SS_PREFIX/bin/x86_64-conda-linux-gnu-g++"
export AXI_CC="$SS_PREFIX/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$AXI_CXX" CC="$AXI_CC"
mkdir -p "$SS_DEPS_ROOT/host-bin" "$SS_DEPS_ROOT/tmp" "$SS_DEPS_ROOT/tmp/ccache" "$SS_DEPS_ROOT/cache"
ln -sfn ../toolchain/bin/x86_64-conda-linux-gnu-gcc "$SS_DEPS_ROOT/host-bin/gcc"
ln -sfn ../toolchain/bin/x86_64-conda-linux-gnu-g++ "$SS_DEPS_ROOT/host-bin/g++"
ss_tool_path="$SS_DEPS_ROOT/host-bin:$SS_DEPS_ROOT/xpu-tools/bin:$SS_PREFIX/bin"
while [[ $PATH == "$ss_tool_path:"* ]]; do PATH=${PATH#"$ss_tool_path:"}; done
export PATH="$ss_tool_path:$PATH"
unset ss_tool_path
export TMPDIR="$SS_DEPS_ROOT/tmp" XDG_CACHE_HOME="$SS_DEPS_ROOT/cache" CCACHE_TEMPDIR="$SS_DEPS_ROOT/tmp/ccache"
export CPATH="$SS_PREFIX/include" LIBRARY_PATH="$SS_PREFIX/lib"
export LD_LIBRARY_PATH="$MEMSIM_BUILD:$SS_DEPS_ROOT/xpu-native/lib:$SS_PREFIX/lib"
export PYTHONPATH="$STORAGE_STACK_ROOT/scripts"
