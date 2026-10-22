#!/usr/bin/env bash
set -euo pipefail
runtime=$(dirname -- "${BASH_SOURCE[0]}")
source "$runtime/environment.sh"
exec "$AXI_PYTHON" "$runtime/compile.py" "$@"
