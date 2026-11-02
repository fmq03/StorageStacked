#!/usr/bin/env bash
set -euo pipefail
root=$STORAGE_WORKSPACE
for device in vortex coralnpu; do
  if [[ $device == vortex ]]; then
    platform=$root/vortex_StorageStacked
    runtime=third_party/gem5/runtime
    build=third_party/.cache/build/memsim
  else
    platform=$root/coralnpu_StorageStacked
    runtime=coralnpu/runtime
    build=coralnpu/.cache/build/memsim
  fi
  (
    source "$platform/$runtime/environment.sh"
    cmake -S "$("$AXI_PYTHON" "$root/axi_StorageStacked/scripts/storage_manifest.py" --field memsim_source)" -B "$platform/$build" \
      -DPython3_EXECUTABLE="$AXI_PYTHON"
  )
done
