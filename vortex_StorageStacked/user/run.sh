#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'HELP'
用法：./run.sh <项目> [--config 相对路径] [--output result/目录] [--test]

  ./run.sh smoke
  ./run.sh llm --config config.json
  ./run.sh smoke --output result/check
  ./run.sh llm --test

配置和输出路径相对于项目目录；默认读取 config.json，并创建新的 result/运行目录。
HELP
}
die() { printf '错误：%s\n' "$*" >&2; exit 2; }

project= config=config.json output= test=0
[[ $# -gt 0 ]] || { usage; exit 2; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --config|--output)
            [[ $# -ge 2 ]] || die "$1 缺少路径"
            if [[ $1 == --config ]]; then config=$2; else output=$2; fi
            shift 2 ;;
        --config=*) config=${1#*=}; shift ;;
        --output=*) output=${1#*=}; shift ;;
        --test) test=1; shift ;;
        --)
            shift
            [[ $# == 1 && -z $project ]] || die '只指定一个项目目录'
            project=$1; shift ;;
        -*) die "未知参数：$1" ;;
        *) [[ -z $project ]] || die '只指定一个项目目录'; project=$1; shift ;;
    esac
done
case "$project" in ''|.|..|*/*) die '请指定 user/ 下的一个项目目录名' ;; esac
[[ -n $config ]] || die '配置路径不能为空'

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
project_dir="user/$project"
[[ -f "$project_dir/src/Makefile" ]] || die "找不到 $project_dir/src/Makefile"
# shellcheck source-path=SCRIPTDIR source=../third_party/gem5/runtime/environment.sh
source third_party/gem5/runtime/environment.sh
runtime=third_party/gem5/runtime
if [[ $test == 1 ]]; then
    [[ $config == config.json && -z $output ]] || die '--test 使用内置回归配置，不能同时指定配置或输出'
    exec "$AXI_PYTHON" "$runtime/test.py" --project "$project"
fi

# 固定本次输入，检查相对路径，并创建新的结果目录。
output=${output:-result/$(date +%Y%m%d-%H%M%S-%N)}
result_dir="$project_dir/$output"
"$AXI_PYTHON" "$runtime/session.py" prepare "$project" "$config" "$output"
stage=build
finish() {
    local code=$?
    trap - EXIT
    if ! "$AXI_PYTHON" "$runtime/session.py" finish "$result_dir" "$stage" "$code"; then code=1; fi
    if [[ $code == 0 ]]; then
        printf 'PASS: %s/%s/report.md\n' "$project" "$output"
    else
        printf 'FAIL: %s/%s/report.md\n' "$project" "$output" >&2
    fi
    exit "$code"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# 已构建的平台可并行运行；改变硬件参数时独占平台缓存。
exec 9>third_party/.cache/build.lock
flock -s 9
printf '[1/3] 编译项目 %s\n' "$project"
if ! "$AXI_PYTHON" "$runtime/build.py" --config "$result_dir/input.json" --ready; then
    flock -u 9
    flock -x 9
    "$AXI_PYTHON" "$runtime/build.py" --config "$result_dir/input.json" >"$result_dir/build.log" 2>&1
    flock -s 9
fi
make -C "$project_dir/src" "CONFIG=../$output/input.json" "OUT=../$output/build" >>"$result_dir/build.log" 2>&1
"$AXI_PYTHON" "$runtime/session.py" resolve "$result_dir" >>"$result_dir/build.log" 2>&1
simulator=$(<"$result_dir/build/simulator.path")
system=$(<"$result_dir/build/system.path")
flock -s 9

stage=simulate
printf '[2/3] gem5 运行 host.elf，Vortex 执行 program.vxbin\n'
"$AXI_PYTHON" "$runtime/session.py" status "$result_dir" "$stage"
simulation_code=0
(cd -- "$result_dir" && "$simulator" --listener-mode=off -d . "$system" --config resolved.json) >"$result_dir/run.log" 2>&1 || simulation_code=$?
"$AXI_PYTHON" "$runtime/session.py" capture "$result_dir"
[[ $simulation_code == 0 ]] || exit "$simulation_code"

stage=validate
printf '[3/3] 校验计算结果与存储链路\n'
"$AXI_PYTHON" "$runtime/session.py" status "$result_dir" "$stage"
"$AXI_PYTHON" "$runtime/validate.py" "$result_dir" >"$result_dir/validation.log" 2>&1
