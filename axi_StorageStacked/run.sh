#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
command=${1:-run}
[[ $# == 0 ]] || shift
case "$command" in
    setup)
        for tool in cmake make c++ python3; do command -v "$tool" >/dev/null; done
        test -f "$root/third_party/systemc/src/systemc"
        echo 'Source and build tools available; ./run.sh build compiles the project.' ;;
    build)
        cmake -S "$root" -B "$root/build" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release "$@"
        cmake --build "$root/build" -j "${BUILD_JOBS:-12}" ;;
    run|test|smoke) exec python3 "$root/scripts/$command.py" "$@" ;;
    *) echo 'Usage: ./run.sh {setup|build|run|test|smoke} [arguments]' >&2; exit 2 ;;
esac
