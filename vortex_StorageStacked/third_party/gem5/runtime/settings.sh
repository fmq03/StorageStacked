export SS_DEPS_ROOT="$SS_ROOT/third_party/.cache/tools"
export BUILD_JOBS=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["jobs"])' "$SS_ROOT/third_party/gem5/runtime/paths.json")
