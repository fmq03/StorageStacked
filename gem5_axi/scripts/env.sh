#!/usr/bin/env bash
# Source this file. New runs use the workspace's single locked toolchain.
# An explicit legacy source selection remains available for historical checks.
if [[ ${AXI_PROFILE:-unified} == unified ]]; then
    source "$(dirname -- "${BASH_SOURCE[0]}")/../../env/activate.sh"
    return
fi
[[ $AXI_PROFILE == legacy ]] || { echo 'AXI_PROFILE must be unified or legacy' >&2; return 1; }
AXI_PROJECT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export AXI_PROJECT_DIR
export AXI_ENV_ROOT=${AXI_ENV_ROOT:-"$HOME/.local/share/storagestacked"}
export AXI_GEM5_HOME=${AXI_GEM5_HOME:-"$AXI_ENV_ROOT/gem5-2721ed751eda"}
export AXI_GEM5_BIN="$AXI_GEM5_HOME/build/AXI/gem5.opt"
export AXI_PYTHON="$AXI_ENV_ROOT/venv/bin/python"
