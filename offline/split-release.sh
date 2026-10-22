#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bundle=${STORAGE_BUNDLE:-zhongxing-storagestacked-offline-20261009-burst}
[[ $bundle =~ ^zhongxing-storagestacked-offline-[0-9]{8}(-[A-Za-z0-9._-]+)?$ ]] || { echo '交付目录名称无效。' >&2; exit 2; }
source_dir=$here/dist/$bundle
destination=${STORAGE_SPLIT_DIR:-$here/dist/$bundle-25MB}
[[ -f $source_dir/DELIVERY.json && -f $source_dir/zhongxing-storagestacked-offline.tar.gz ]] || {
    echo '请先完成新镜像的导出与验收。' >&2; exit 1;
}
[[ ! -e $destination ]] || { echo "分块目录已存在：$destination" >&2; exit 1; }
mkdir -p -- "$destination"
# The image is already gzip-compressed. The outer tar keeps the complete
# delivery together without recompressing it or creating another huge file.
tar -C "$here/dist" -cf - "$bundle" | split -b 25000000 -d -a 4 - "$destination/$bundle.tar.part-"
cp "$here/extract-release.sh" "$destination/extract.sh"
cp "$here/download-release.sh" "$destination/download.sh"
for name in run.sh docker-compat-entrypoint.py run-seccomp-check.sh DOCKER_COMPATIBILITY.md; do
    cp -- "$here/$name" "$destination/$name"
done
python3 - "$source_dir" "$destination" "$bundle" <<'PY'
import json,sys
from pathlib import Path
source,destination=map(Path,sys.argv[1:3]);bundle=sys.argv[3]
parts=sorted(destination.glob(bundle+'.tar.part-*'))
assert parts and all(p.stat().st_size==25000000 for p in parts[:-1])
assert 0<parts[-1].stat().st_size<=25000000
(destination/'PARTS.tsv').write_text(''.join(f'{p.name}\t{p.stat().st_size}\n' for p in parts))
files=[{'name':p.name,'bytes':p.stat().st_size} for p in sorted(source.iterdir()) if p.is_file()]
metadata={'format':'ordered split tar','part_bytes':25000000,'parts':len(parts),
          'total_bytes':sum(p.stat().st_size for p in parts),'delivery_directory':bundle,
          'source_versions':json.loads((source/'SOURCE_VERSIONS.json').read_text()),
          'image':json.loads((source/'DELIVERY.json').read_text())['image'],
          'files':files,'validation':'filenames, sizes, actual extraction and Docker import'}
(destination/'PACKAGE.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
(destination/'README.md').write_text(f'''# 三项目离线 Docker 分块包

本包包含 Linux amd64 完整 Docker 镜像、运行入口、源码、工具链、模型与验收文档。CoralNPU 启用 RVV，Vortex 的默认 LLM 使用 8 个 SIMT 线程。

推荐克隆发布仓库后执行 `bash download.sh`，自动下载分块并解包：

```bash
git clone https://github.com/hy2581/zhongxing-storagestacked-offline.git
cd zhongxing-storagestacked-offline
bash download.sh
cd downloads/{bundle}
bash run.sh
```

下载默认使用单连接，支持续传，已完成且大小符合清单的分块会跳过。每 5 秒显示分块字节数和百分比；连续 60 秒低于 1 字节/秒或单次请求超过 900 秒会重试，每个文件最多尝试 6 次。按 Ctrl+C 可停止下载并保留续传文件。

下载全部 `{bundle}.tar.part-*`、`PARTS.tsv`、`extract.sh`，以及 `run.sh`、`docker-compat-entrypoint.py`、`run-seccomp-check.sh`、`DOCKER_COMPATIBILITY.md` 四个启动补丁文件，放在同一个目录，然后执行：

```bash
bash extract.sh
cd {bundle}
bash run.sh
```

可以用 `bash extract.sh /目标路径` 指定解压位置。已有交付目录不会被覆盖。脚本需要 Linux 上的 Bash、tar 和常用命令，不需要 Python。

共 {len(parts)} 块，每块 25,000,000 字节，最后一块 {parts[-1].stat().st_size:,} 字节。清单只记录文件名和大小，不做哈希校验。脚本按清单顺序合并并解包，直接恢复完整交付目录，不额外保存合并后的大 tar 文件。镜像在交付目录中保持 gzip 压缩，首次运行 `bash run.sh` 自动导入，之后使用 `--pull=never --network=none` 运行。

分块文件约 10.3 GB，解包后的交付目录约 10.3 GB，Docker 镜像约 23.9 GB；首次导入还会临时展开约 24 GB，默认验收会生成数 GiB 结果。下载目录、结果目录和 Docker 数据目录如果在同一块磁盘，建议至少预留 100 GB 可用空间；如果在不同磁盘，需要分别检查各自剩余空间。首次导入和默认验收可能耗时数十分钟。`PACKAGE.json` 记录源码版本与文件大小；解包后的 `VALIDATION.json` 和 `ACCEPTANCE.json` 是本次制作方的实际验收摘要。

分块是一个外层 tar 的连续片段，不能逐块单独解压。Docker 镜像已压缩，外层不重复压缩。
''')
print(f'分块完成：{destination}，{len(parts)} 块，总计 {metadata["total_bytes"]} 字节。')
PY
