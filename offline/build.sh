#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
context=$root/offline/.build/context
image=${STORAGE_IMAGE:-zhongxing-storagestacked:offline-20261009-burst}
base=${STORAGE_BASE_IMAGE:-ubuntu:20.04}
docker image inspect "$base" >/dev/null
mkdir -p "$context/workspace"
# Keep sources, installed tools and reusable build caches. Drop historical runs,
# repository metadata, the obsolete Bazel workspace and transient server state.
for project in axi_StorageStacked vortex_StorageStacked coralnpu_StorageStacked markdown; do
  rsync -aH --delete --delete-excluded --link-dest="$root/$project" \
    --exclude='.git' --exclude='__pycache__' --exclude='*.pyc' \
    --exclude='/build/Testing/' --exclude='/results/' --exclude='/validation/' \
    --exclude='/user/*/result/' --exclude='/.cache/' \
    --exclude='/third_party/.cache/tutorial*/' --exclude='/third_party/.cache/validation/' \
    --exclude='/coralnpu/.cache/validation/' \
    --exclude='/coralnpu/.cache/tools/bazel/e6929e4aa2b760202f0ef14aa832744d/' \
    --exclude='/coralnpu/.cache/tools/llm-training/' \
    --exclude='**/tools/tmp/' --exclude='**/tools/cache/' \
    --exclude='**/bazel/*/server/' --exclude='**/bazel/*/sandbox/' \
    "$root/$project/" "$context/workspace/$project/"
done
cp "$root/README.md" "$root/accept.sh" "$context/workspace/"
mkdir -p "$context/workspace/offline"
cp "$root/offline/README.md" "$context/workspace/offline/README.md"
python3 - "$root" "$context/workspace/SOURCE_VERSIONS.json" <<'PY'
import datetime,json,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]); projects={}
for p in ('axi_StorageStacked','vortex_StorageStacked','coralnpu_StorageStacked'):
    commit=subprocess.check_output(['git','-C',str(root/p),'rev-parse','HEAD'],text=True).strip()
    status=subprocess.check_output(['git','-C',str(root/p),'status','--porcelain'],text=True)
    projects[p]={'commit':commit,'working_tree_changes':status.splitlines()}
Path(sys.argv[2]).write_text(json.dumps({'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'workspace_root':str(root),'projects':projects},ensure_ascii=False,indent=2)+'\n')
PY
cp "$root/offline/Dockerfile" "$root/offline/entrypoint.py" "$root/offline/docker-compat-entrypoint.py" "$root/offline/prepare.sh" "$root/offline/fix-native-cache.sh" "$context/"
mkdir -p "$context/system-tools"
cp "$root"/offline/system-tools/*.deb "$context/system-tools/"
# One archive avoids the legacy builder's quadratic per-file COPY lookup.
tar --owner=0 --group=0 -C "$context/workspace" -cf "$context/workspace.tar.part" .
mv "$context/workspace.tar.part" "$context/workspace.tar"
printf 'workspace/\n*.part\n' > "$context/.dockerignore"
docker build --pull=false --network=none --build-arg "BASE_IMAGE=$base" --build-arg "WORKSPACE_ROOT=$root" -t "$image" "$context"
printf '镜像完成：%s\n' "$image"
