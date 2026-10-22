#!/usr/bin/env python3
"""Build a lexical class/object/trait/method index for Bazel Chisel sources."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
source=ROOT/'coralnpu_StorageStacked/coralnpu/hdl/chisel'
files=sorted(str(p.relative_to(source)) for p in source.rglob('*.scala'))
command=['/usr/bin/ctags-universal','--options=NONE','--langdef=Scala','--langmap=Scala:.scala',r'--regex-Scala=/^[ \t]*((abstract|final|sealed|case|private|protected|override|lazy)[ \t]+)*(class|object|trait|def|val|var|type)[ \t]+([a-zA-Z_][a-zA-Z0-9_]*)/\4/d,definition/', '--tag-relative=yes','--sort=yes','-f','.tags',*files]
subprocess.run(command,cwd=source,check=True)
print('Chisel lexical index:',len(files),'Scala source files')
