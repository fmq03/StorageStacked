#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$root/.." && pwd)
build=${STORAGE_BUILD_DIR:-$root/build}
command=${1:-run}
[[ $# == 0 ]] || shift
case "$command" in
    setup)
        bash "$repo/common/toolchain/setup.sh"
        source "$repo/common/toolchain/env.sh"
        if [[ -n ${STORAGE_SYSTEMC_PREFIX:-} ]]; then
            test -f "$STORAGE_SYSTEMC_PREFIX/include/systemc"
        else
            test -f "$root/third_party/systemc/src/systemc"
        fi
        echo 'Source and build tools available; ./run.sh build compiles the project.' ;;
    build)
        source "$repo/common/toolchain/env.sh"
        options=("-DSTORAGE_SYSTEMC_PREFIX=${STORAGE_SYSTEMC_PREFIX:-}" "-DSTORAGE_SYSTEMC_TARGET=${STORAGE_SYSTEMC_TARGET:-}" "-DSTORAGE_CXX_STANDARD=${STORAGE_CXX_STANDARD:-17}")
        cmake -S "$root" -B "$build" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_ASM_COMPILER="$CC" "${options[@]}" "$@"
        cmake --build "$build" -j "${BUILD_JOBS:-12}" ;;
    systemc-prefix)
        if [[ -n ${STORAGE_SYSTEMC_TARGET:-} && -n ${STORAGE_SYSTEMC_PREFIX:-} ]]; then
            echo 'STORAGE_SYSTEMC_TARGET 与 STORAGE_SYSTEMC_PREFIX 互斥' >&2; exit 2
        fi
        if [[ -n ${SYSTEMC_HOME:-} && -n ${STORAGE_SYSTEMC_PREFIX:-} && $(realpath -m "$SYSTEMC_HOME") != $(realpath -m "$STORAGE_SYSTEMC_PREFIX") ]]; then
            echo 'SYSTEMC_HOME 与 STORAGE_SYSTEMC_PREFIX 指向不同安装' >&2; exit 2
        fi
        selected=${SYSTEMC_HOME:-${STORAGE_SYSTEMC_PREFIX:-}}
        if [[ -n $selected ]]; then
            [[ -f "$selected/include/systemc" ]] || { echo "SystemC 头文件缺失：$selected" >&2; exit 2; }
            library_found=0
            for library in "$selected"/{lib,lib64}/libsystemc.{a,so}; do
                [[ ! -f $library ]] || library_found=1
            done
            [[ $library_found == 1 ]] || { echo "SystemC 库缺失：$selected" >&2; exit 2; }
        fi
        "$root/run.sh" build >&2
        if [[ -z $selected ]]; then
            selected=$repo/.cache/systemc-debug
            mkdir -p "$selected/lib"
            ln -sfn "$root/third_party/systemc/src" "$selected/include"
            ln -sfn "$(realpath "$build/libstorage_systemc.a")" "$selected/lib/libsystemc.a"
        fi
        printf '%s\n' "$(cd "$selected" && pwd)" ;;
    run|test|smoke)
        source "$repo/common/toolchain/env.sh"
        export PYTHONPATH="$("$SS_PYTHON" "$root/scripts/storage_manifest.py" --pythonpath)${PYTHONPATH:+:$PYTHONPATH}"
        exec "$SS_PYTHON" "$root/scripts/$command.py" "$@" ;;
    *) echo 'Usage: ./run.sh {setup|build|systemc-prefix|run|test|smoke} [arguments]' >&2; exit 2 ;;
esac
