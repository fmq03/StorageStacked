#!/usr/bin/env bash
# Read-only readiness check for the native workflow.  This script does
# not install packages, build sources, apply patches, or modify user settings.
#
# Profiles:
#   offline - gem5/build/X86 + gem5/.venv/bin/scons + mem_sim/build/hbm_sim
#   unified - gem5/build/AXI + $SS_PREFIX/bin/scons + mem_sim/build-unified
#             libstoragestacked_memsim.so (online C ABI, no hbm_sim)
#   auto    - unified when env/activate.sh exists and mem_sim has no own .git
#
# Profile-local paths are derived here only; native_env.sh keeps its global
# offline defaults (MEMSIM_BUILD=build, MEMSIM_BIN=build/hbm_sim) untouched so
# the offline hbm_sim, LLM-memory workload and legacy make flow keep working.

set -u

SELF_DIR=$(dirname "$(readlink -f "$0")")
# shellcheck source=native_env.sh
. "$SELF_DIR/native_env.sh"

PROFILE=${HET_PREFLIGHT_PROFILE:-auto}
if [ "$PROFILE" = "auto" ]; then
    if [ -f "$HET_WORKSPACE_ROOT/env/activate.sh" ] && [ ! -e "$MEMSIM_HOME/.git" ]; then
        PROFILE=unified
    else
        PROFILE=offline
    fi
fi
case "$PROFILE" in
    unified | offline) ;;
    *)
        echo "未知 HET_PREFLIGHT_PROFILE=$PROFILE（应为 auto/offline/unified）" >&2
        exit 2
        ;;
esac

MEMSIM_UNIFIED_BUILD=${MEMSIM_UNIFIED_BUILD:-$MEMSIM_HOME/build-unified}
SS_DEPS_ROOT=${SS_DEPS_ROOT:-$HOME/.local/share/storagestacked-unified}
SS_PREFIX=${SS_PREFIX:-$SS_DEPS_ROOT/toolchain}

if [ "$PROFILE" = "unified" ]; then
    GEM5_BUILD=${GEM5_BUILD:-$GEM5_HOME/build/AXI}
    GEM5_SCONS=${GEM5_SCONS:-$SS_PREFIX/bin/scons}
else
    GEM5_BUILD=${GEM5_BUILD:-$GEM5_HOME/build/X86}
    GEM5_SCONS=${GEM5_SCONS:-$GEM5_HOME/.venv/bin/scons}
fi

failures=0

ok() {
    printf '  ok   %s\n' "$*"
}

# Informational only: a dirty working tree (submodule adaptations, uncommitted
# edits) does not block the unified workflow, because every gitlink still
# matches the lock file.  Never counts as a failure.
warn() {
    printf '  warn %s\n' "$*" >&2
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

echo "Native Linux environment (profile: $PROFILE)"
echo "  project    $HET_PROJECT_ROOT"
echo "  gem5       $GEM5_HOME"
echo "  CoralNPU   $CORALNPU_HOME"
echo "  Vortex     $VORTEX_HOME"
echo "  VX build   $VORTEX_BUILD"
echo "  mem_sim    $MEMSIM_HOME"
if [ "$PROFILE" = "unified" ]; then
    echo "  build dir  $GEM5_BUILD"
    echo "  scons      $GEM5_SCONS"
    echo "  online so  $MEMSIM_UNIFIED_BUILD/libstoragestacked_memsim.so"
else
    echo "  hbm_sim    $MEMSIM_BIN"
fi

echo
echo "Commands"
if [ "$PROFILE" = "unified" ]; then
    # iverilog/vvp serve storage_chain RTL only; verilator and bazel are
    # flow-internal here (Bazel-managed, and invoked by absolute path), so they
    # are checked as locked artifacts below rather than as PATH commands.
    command_list="git make gcc g++ python3 cmake patch"
else
    command_list="git make gcc g++ python3 cmake iverilog vvp verilator bazel"
fi
for command_name in $command_list; do
    check_command "$command_name"
done

echo
echo "Source trees"
check_directory "$GEM5_HOME" "gem5"
check_directory "$CORALNPU_HOME" "CoralNPU"
check_directory "$VORTEX_HOME" "Vortex"
if [ "$PROFILE" = "unified" ]; then
    check_directory "$MEMSIM_HOME" "mem_sim (主仓库普通源码目录)"
    if [ -e "$MEMSIM_HOME/.git" ]; then
        bad "unified 布局下 mem_sim 不应保留独立 .git"
    else
        ok "mem_sim 无独立 .git（符合 monorepo 布局）"
    fi
else
    check_directory "$MEMSIM_HOME" "mem_sim (Git 或带版本记录的源码包)"
fi
check_file "$MEMSIM_HOME/CMakeLists.txt" "mem_sim source"
check_file "$VORTEX_HOME/third_party/ramulator/CMakeLists.txt" \
    "Vortex Ramulator dependency source"

echo
echo "Pinned source revisions"
if ! python3 "$SELF_DIR/upstreams.py" check --profile "$PROFILE"; then
    bad "源码版本检查失败（profile=$PROFILE）"
fi

if [ "$PROFILE" = "unified" ]; then
    echo
    echo "Source layout and gitlink"
    if source_output=$(python3 "$HET_WORKSPACE_ROOT/env/check_sources.py" 2>&1); then
        ok "source revisions: ready（外部子模块 gitlink 与锁一致，内部目录为主仓库普通目录）"
    else
        bad "source revisions: $(printf '%s' "$source_output" | head -1)"
    fi
    # Dirty is expected (external adaptations + uncommitted edits). Report it,
    # never fail on it: gitlinks above are what the acceptance depends on.
    dirty_count=$(git -C "$HET_WORKSPACE_ROOT" status --porcelain 2>/dev/null | wc -l)
    if [ "$dirty_count" -eq 0 ]; then
        ok "working tree: clean"
    else
        warn "working tree: dirty（$dirty_count 项未提交改动或子模块内适配；informational，不影响验收）"
    fi
fi

echo
echo "Run-ready artifacts"
check_executable "$GEM5_BUILD/gem5.opt" "gem5.opt ($PROFILE)"
check_executable "$GEM5_SCONS" "gem5 SCons (GEM5_SCONS 可指定已有构建环境)"
for params in HetAxiMonitor UnifiedTimingMemory VortexGPGPU CoralNPU; do
    check_file "$GEM5_BUILD/params/$params.hh" "built $params params"
done
if [ "$PROFILE" = "unified" ]; then
    # The online path links the shared library through mem_sim/integration/online.h.
    # hbm_sim is the offline CLI and is NOT the acceptance artifact here.
    check_file "$MEMSIM_UNIFIED_BUILD/libstoragestacked_memsim.so" \
        "online mem_sim library (C ABI)"
    # build_xpu.sh invokes this by absolute path; PATH bazel is not required.
    check_executable "$SS_DEPS_ROOT/xpu-tools/bin/bazel" "locked bazel (xpu-tools)"
else
    check_file "$GEM5_HOME/configs/het/het_system.py" "installed three-source config"
    check_executable "$MEMSIM_BIN" "external hbm_sim"
fi
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

if [ "$PROFILE" = "unified" ]; then
    echo
    echo "Installed device sources vs project sources"
    # install_devices.sh copies without deleting: a stale leftover is only
    # detectable by comparing whole directories.
    for component in dev/coralnpu dev/vortex mem/unified_timing; do
        project_src="$HET_PROJECT_ROOT/gem5int/src/$component"
        installed="$GEM5_HOME/src/$component"
        if [ ! -d "$installed" ]; then
            bad "已安装设备目录缺失: src/$component"
        elif diff -r --exclude='__pycache__' "$project_src" "$installed" >/dev/null 2>&1; then
            ok "src/$component 与 gem5_new 真值源一致"
        else
            bad "src/$component 与 gem5_new 真值源不一致（陈旧副本或只增不删残留）"
        fi
    done

    echo
    echo "gem5 source patches"
    # Missing patches cause silent wrong results (lost WSTRB, bad VCD bit
    # strings), so assert them instead of trusting the build log.
    if patch -R -p1 -s -f --dry-run -d "$GEM5_HOME" \
            -i "$HET_PROJECT_ROOT/gem5int/patches/dma_byte_enable.patch" >/dev/null 2>&1; then
        ok "dma_byte_enable 已应用"
    else
        bad "dma_byte_enable 未应用（install_devices.sh）"
    fi
    if grep -q 'getConstPtr<unsigned char>()' \
            "$GEM5_HOME/src/systemc/tlm_bridge/gem5_to_tlm.cc" 2>/dev/null; then
        ok "masked_write 已应用"
    else
        bad "masked_write 未应用（gem5_axi/scripts/patch_gem5.py）"
    fi
    if grep -q 'str.resize(w);' "$GEM5_HOME/src/systemc/utils/vcd.cc" 2>/dev/null; then
        ok "vcd_integral 已应用"
    else
        bad "vcd_integral 未应用（gem5_axi/scripts/patch_gem5.py）"
    fi
fi

echo
echo "Versions"
uname -srmo | sed 's/^/  /'
gcc --version | sed -n '1s/^/  /p'
python3 --version 2>&1 | sed 's/^/  /'
if [ "$PROFILE" = "offline" ]; then
    iverilog -V 2>&1 | sed -n '1s/^/  /p'
fi
verilator --version 2>&1 | sed 's/^/  /'
if [ -f "$CORALNPU_HOME/.bazelversion" ]; then
    sed 's/^/  CoralNPU .bazelversion: /' "$CORALNPU_HOME/.bazelversion"
fi

echo
if [ "$failures" -eq 0 ]; then
    if [ "$PROFILE" = "unified" ]; then
        echo "READY: unified online workflow (AXI256/UCIe -> mem_sim C ABI) can run."
    else
        echo "READY: native Linux environment can run the full three-source workflow."
    fi
else
    echo "NOT READY: $failures required command/source/artifact checks failed." >&2
    echo "Build the missing items in the project manual docs/USER_MANUAL.md, then rerun this script." >&2
    exit 1
fi
