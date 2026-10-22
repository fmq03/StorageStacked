#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ! -x "$root/.vscode/tools/clangd_22.1.0/bin/clangd" ]]; then
    python3 - "$root" <<'PY'
from pathlib import Path
import sys, tempfile, urllib.request, zipfile
destination=Path(sys.argv[1])/'.vscode/tools'
destination.mkdir(parents=True,exist_ok=True)
url='https://github.com/clangd/clangd/releases/download/22.1.0/clangd-linux-22.1.0.zip'
with tempfile.TemporaryDirectory(prefix='zhongxing-clangd-') as directory:
    package=Path(directory)/'clangd.zip'
    urllib.request.urlretrieve(url,package)
    if package.stat().st_size!=96066963:raise RuntimeError('Unexpected clangd package size')
    with zipfile.ZipFile(package) as archive:archive.extractall(destination)
binary=destination/'clangd_22.1.0/bin/clangd'
binary.chmod(binary.stat().st_mode|0o100)
PY
fi
"$root/.vscode/tools/clangd_22.1.0/bin/clangd" --version
# A private verification dependency; Python editing itself uses installed Pylance.
if [[ ! -x "$root/.vscode/deps/node_modules/.bin/pyright-langserver" ]]; then
    npm install --prefix "$root/.vscode/deps" --no-audit --no-fund pyright@1.1.408
fi
