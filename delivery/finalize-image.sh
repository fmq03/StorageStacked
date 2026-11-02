#!/usr/bin/env bash
set -euo pipefail
[[ $# -ge 1 && $# -le 2 ]] || { echo '用法：finalize-image.sh 输入镜像 [交付镜像]' >&2; exit 2; }
source_image=$1
delivery_image=${2:-$source_image}
cleanup_container=
release_container() {
    [[ -z $cleanup_container ]] || docker rm -f "$cleanup_container" >/dev/null 2>&1 || :
}
trap release_container EXIT
cleanup_container=$(docker create --pull=never --network=none --entrypoint /bin/bash \
    "$source_image" /opt/storagestacked/clean-runtime-state.sh)
docker start --attach "$cleanup_container"
docker commit \
    --change 'ENTRYPOINT ["/usr/local/bin/python3", "/opt/storagestacked/docker-compat-entrypoint.py"]' \
    --change 'CMD ["all"]' "$cleanup_container" "$delivery_image" >/dev/null
docker rm "$cleanup_container" >/dev/null
cleanup_container=
printf '交付镜像准备完成：%s\n' "$delivery_image"
