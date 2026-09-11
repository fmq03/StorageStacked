#!/usr/bin/env bash
# Read-only readiness check for the native Ubuntu workflow.  This script does
# not install packages, build sources, apply patches, or modify user settings.

set -u

SELF_DIR=$(dirname "$(readlink -f "$0")")
# shellcheck source=native_env.sh
. "$SELF_DIR/native_env.sh"
GEM5_SCONS=${GEM5_SCONS:-$GEM5_HOME/.venv/bin/scons}

failures=0

ok() {
    printf '  ok   %s\n' "$*"
}

bad() {
    printf '  MISS %s\n' "$*" >&2
    failures=$((failures + 1))
}

check_command() {
    if command -v "$1" >/dev/null 2>&1; then
        ok "$1 -> $(command -v "$1")"
    else
        bad "命令 $1"
    fi
}

check_directory() {
    if [ -d "$1" ]; then
        ok "$2 -> $1"
    else
        bad "$2 目录 $1"
    fi
}

check_file() {
    if [ -f "$1" ]; then
        ok "$2 -> $1"
    else
        bad "$2 $1"
    fi
}

check_executable() {
    if [ -x "$1" ]; then
        ok "$2 -> $1"
    else
        bad "$2 $1"
    fi
}

echo "Native Linux environment"
echo "  project    $HET_PROJECT_ROOT"
echo "  gem5       $GEM5_HOME"
echo "  CoralNPU   $CORALNPU_HOME"
echo "  Vortex     $VORTEX_HOME"
echo "  VX build   $VORTEX_BUILD"
echo "  mem_sim    $MEMSIM_HOME"
echo "  hbm_sim    $MEMSIM_BIN"

echo
echo "Commands"
for command_name in git make gcc g++ python3 cmake iverilog vvp verilator bazel; do
    check_command "$command_name"
done

echo
echo "Source trees"
check_directory "$GEM5_HOME" "gem5"
check_directory "$CORALNPU_HOME" "CoralNPU"
check_directory "$VORTEX_HOME" "Vortex"
check_directory "$MEMSIM_HOME" "mem_sim (Git 或带版本记录的源码包)"
check_file "$MEMSIM_HOME/CMakeLists.txt" "mem_sim source"
check_file "$VORTEX_HOME/third_party/ramulator/CMakeLists.txt" \
    "Vortex Ramulator dependency source"

echo
echo "Pinned source revisions"
if ! python3 "$SELF_DIR/upstreams.py" check; then
    bad "源码版本/源码包检查失败（以 upstream.lock.json 为准）"
fi

echo
echo "Run-ready artifacts"
check_executable "$GEM5_HOME/build/X86/gem5.opt" "gem5.opt"
check_executable "$GEM5_SCONS" "gem5 SCons (GEM5_SCONS 可指定已有构建环境)"
check_file "$GEM5_HOME/configs/het/het_system.py" "installed three-source config"
check_file "$GEM5_HOME/build/X86/params/HetAxiMonitor.hh" \
    "built HetAxiMonitor params"
check_file "$GEM5_HOME/build/X86/params/UnifiedTimingMemory.hh" \
    "built functional memory params"
check_file "$GEM5_HOME/build/X86/params/VortexGPGPU.hh" \
    "built Vortex SimObject params"
check_file "$GEM5_HOME/build/X86/params/CoralNPU.hh" \
    "built CoralNPU SimObject params"
check_executable "$MEMSIM_BIN" "external hbm_sim"
check_file "$VORTEX_HOME/third_party/ramulator/libramulator.so" \
    "Vortex Ramulator dependency library"
check_file "$VORTEX_BUILD/sim/simx/libvortex-gem5.so" "Vortex gem5 library"
check_file "$VORTEX_BUILD/sw/runtime/libvortex.so" "Vortex host runtime"
check_file "$VORTEX_BUILD/sw/runtime/libvortex-gem5-x86_64.so" "Vortex gem5 driver"
check_executable "$VORTEX_BUILD/tests/regression/vecadd/vecadd" \
    "Vortex vecadd host workload"
check_file "$VORTEX_BUILD/tests/regression/vecadd/kernel.vxbin" "Vortex vecadd kernel"
check_file "$CORALNPU_HOME/bazel-bin/gem5int/libcoralnpu-gem5.so" \
    "CoralNPU gem5 library"
CORALNPU_BAZEL_OUT=$(readlink -f "$CORALNPU_HOME/bazel-out" 2>/dev/null || true)
CORALNPU_KERNEL=
if [ -n "$CORALNPU_BAZEL_OUT" ]; then
    CORALNPU_KERNEL=$(find "$CORALNPU_BAZEL_OUT" \
        -path '*/gem5int/ddr_touch.elf' -type f -print -quit 2>/dev/null)
fi
check_file "$CORALNPU_KERNEL" "CoralNPU ddr_touch workload"
check_file "$HET_PROJECT_ROOT/workloads/three_source/host_main.cpp" \
    "three-source host workload source"

echo
echo "Versions"
uname -srmo | sed 's/^/  /'
gcc --version | sed -n '1s/^/  /p'
python3 --version 2>&1 | sed 's/^/  /'
iverilog -V 2>&1 | sed -n '1s/^/  /p'
verilator --version 2>&1 | sed 's/^/  /'
if [ -f "$CORALNPU_HOME/.bazelversion" ]; then
    sed 's/^/  CoralNPU .bazelversion: /' "$CORALNPU_HOME/.bazelversion"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "READY: native Linux environment can run the full three-source workflow."
else
    echo "NOT READY: $failures required command/source/artifact checks failed." >&2
    echo "Build the missing items in the project manual docs/USER_MANUAL.md, then rerun this script." >&2
    exit 1
fi
