"""Restore omitted build profiles from the exact vendored SoftFloat revision."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile

runtime=Path(__file__).resolve().parent
item=json.loads((runtime/'defaults/xpu-artifacts.lock.json').read_text())['softfloat']
source=Path(os.environ['VORTEX_HOME'])/'third_party/softfloat'
cache=Path(os.environ['SS_DEPS_ROOT'])/'xpu-downloads'
cache.mkdir(parents=True,exist_ok=True)
archive=cache/('softfloat-'+item['revision']+'.tar.gz')
if not archive.exists():
    if os.environ.get('SS_OFFLINE')=='1':raise RuntimeError('Missing offline dependency '+str(archive))
    subprocess.run(['curl','-fLsS','--retry','3',item['url'],'-o',str(archive)],check=True)
if hashlib.sha256(archive.read_bytes()).hexdigest()!=item['sha256']:
    raise RuntimeError('SoftFloat archive differs from pinned revision')
with tarfile.open(archive) as tar:
    members={m.name.split('/',1)[1]:m for m in tar.getmembers() if '/' in m.name and m.isfile()}
    for path in (source/'source').rglob('*'):
        if not path.is_file():continue
        name=path.relative_to(source).as_posix()
        if name not in members or path.read_bytes()!=tar.extractfile(members[name]).read():
            raise RuntimeError('Vendored SoftFloat source differs from pinned revision: '+name)
    for name,member in members.items():
        if not name.startswith('build/'):continue
        if '..' in Path(name).parts:raise ValueError('Invalid source archive path')
        path=source/name;data=tar.extractfile(member).read()
        if path.exists() and path.read_bytes()!=data:raise RuntimeError('Existing build profile differs: '+str(path))
        path.parent.mkdir(parents=True,exist_ok=True)
        if not path.exists():path.write_bytes(data)
print('SoftFloat source verified; original build profiles are ready.')
