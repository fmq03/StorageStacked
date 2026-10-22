#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/environment.sh"
exec "$AXI_PYTHON" "$SS_ROOT/third_party/gem5/runtime/compile.py" "$@"
