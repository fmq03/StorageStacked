"""Check native libraries, selected device and one SystemC kernel."""
import argparse,ctypes,json,os,re,subprocess
from pathlib import Path
from paths import ROOT,CACHE,VORTEX_BUILD,MEMSIM_BUILD,gem5_binary
from configure import load_config
from storage_dependency import require_build
parser=argparse.ArgumentParser();parser.add_argument('--config',default=ROOT/'user/smoke/config.json');options=parser.parse_args()
isa=load_config(options.config)['host']['isa'];GEM5=gem5_binary(isa)
require_build()
subprocess.run([str(GEM5),'--listener-mode=off','-d',str(CACHE/'check'),'-c',"import m5.objects as o; assert hasattr(o,'VortexGPGPU'); assert hasattr(o,'StorageBridge'); assert not hasattr(o,'CoralNPU'); print('DEVICE_ISOLATION_PASS')"],check=True)
lib=VORTEX_BUILD/'sim/simx/libvortex-gem5.so'
for p in (lib,MEMSIM_BUILD/'libstoragestacked_memsim.so'):ctypes.CDLL(str(p))
linked='\n'.join(subprocess.check_output(['ldd',str(p)],text=True) for p in (lib,GEM5))
assert 'not found' not in linked and 'libsystemc' not in linked.lower()
allowed=(ROOT,Path('/lib'),Path('/lib64'),Path('/usr'))
for dependency in re.findall(r'(/[\w./+-]+)',linked):assert any(Path(dependency).is_relative_to(p) for p in allowed),dependency
print(json.dumps({'available':True,'device':'vortex','host_isa':isa,'scope':'Build availability; computation is checked by user/run.sh'}))
