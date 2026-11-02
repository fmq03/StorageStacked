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
    cmake -S "$root/axi_StorageStacked/mem_sim" -B "$platform/$build" \
      -DPython3_EXECUTABLE="$AXI_PYTHON"
  )
done
