#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    exec bash coralnpu/runtime/build.sh "$@"
fi
mkdir -p coralnpu/.cache
echo '正在配置路径、准备工具并编译平台与用户项目……'
if bash coralnpu/runtime/build.sh "$@" > coralnpu/.cache/last-build.log 2>&1; then
    tail -n 2 coralnpu/.cache/last-build.log
else
    echo '构建失败，以下是末尾日志（完整日志：coralnpu/.cache/last-build.log）：' >&2
    tail -n 25 coralnpu/.cache/last-build.log >&2
    exit 1
fi
