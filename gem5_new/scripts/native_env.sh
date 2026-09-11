#!/usr/bin/env bash
# Source this file from the project root (or from any directory) to select the
# source/build trees used by the native-Linux workflow.

HET_PROJECT_ROOT=$(dirname "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")")
HET_WORKSPACE_ROOT=$(dirname "$HET_PROJECT_ROOT")

export HET_PROJECT_ROOT
export GEM5_HOME=${GEM5_HOME:-$HET_WORKSPACE_ROOT/gem5}
export CORALNPU_HOME=${CORALNPU_HOME:-$HET_WORKSPACE_ROOT/coralnpu}
export VORTEX_HOME=${VORTEX_HOME:-$HET_WORKSPACE_ROOT/vortex-gpu/vortex}
export VORTEX_BUILD=${VORTEX_BUILD:-$HET_WORKSPACE_ROOT/vortex-gpu/vxbuild}
export MEMSIM_HOME=${MEMSIM_HOME:-$HET_WORKSPACE_ROOT/mem_sim}
export MEMSIM_BUILD=${MEMSIM_BUILD:-$MEMSIM_HOME/build}
export MEMSIM_BIN=${MEMSIM_BIN:-$MEMSIM_BUILD/hbm_sim}

# Vortex SimX 仍依赖 $VORTEX_HOME/third_party/ramulator。它是 Vortex 自带的
# 构建/运行时依赖，不是本项目旧的在线 Ramulator 内存后端。

case ":${PYTHONPATH:-}:" in
    *":$HET_PROJECT_ROOT/tools:"*) ;;
    *) export PYTHONPATH="$HET_PROJECT_ROOT/tools${PYTHONPATH:+:$PYTHONPATH}" ;;
esac
