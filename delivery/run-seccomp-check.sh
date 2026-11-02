#!/usr/bin/env bash
# Explicit diagnostic fallback; the normal launcher keeps Docker's seccomp policy.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export STORAGE_SECCOMP_MODE=unconfined
exec bash "$here/run.sh" "$@"
