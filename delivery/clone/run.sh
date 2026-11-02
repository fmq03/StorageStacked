#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bundle=$root/downloads/zhongxing-storagestacked-offline-20261022
export STORAGE_ARCHIVE=${STORAGE_ARCHIVE:-$bundle/zhongxing-storagestacked-offline.tar.gz}
export STORAGE_RESULTS_DIR=${STORAGE_RESULTS_DIR:-$root/results}
exec bash "$root/offline/run.sh" "$@"
