#!/usr/bin/env bash
set -euo pipefail
# This runs in a new container after the build container has exited.
# Its private Bazel processes are gone; keep compiled outputs and repositories.
cache=$STORAGE_WORKSPACE/coralnpu_StorageStacked/coralnpu/.cache/tools/bazel
find "$cache" -mindepth 2 -maxdepth 2 \
  -type d \( -name server -o -name bazel-workers -o -name sandbox \) -exec rm -rf {} +
printf 'Bazel 临时服务、worker 和 sandbox 状态已清理。\n'
