"""Recreate only disposable build prefixes when a prepared checkout moves."""
from pathlib import Path
import shutil
root=Path(__file__).resolve().parents[3]
cache=root/'third_party/.cache';marker=cache/'location'
if marker.exists() and marker.read_text().strip()!=str(root):
    for path in (cache/'tools/toolchain',cache/'tools/xpu-sysroot',cache/'tools/host-bin',
                 cache/'build',root/'third_party/gem5/build',root/'third_party/vortex/third_party/ramulator/build'):
        if path.is_dir():shutil.rmtree(path)
    print('检测到仓库位置变化，已清理可重建的工具前缀和编译缓存；保留包缓存与用户结果。')
