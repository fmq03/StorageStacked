"""Check library isolation; computation is accepted only after actual simulation."""
import ctypes
import json
import os
import re
import subprocess
from pathlib import Path
from paths import ROOT,BUILD,MEMSIM_BUILD,relative
from storage_dependency import require_build
require_build()
artifacts=[BUILD/'libcoralnpu-native.so',MEMSIM_BUILD/'libstoragestacked_memsim.so']
for artifact in artifacts:
    ctypes.CDLL(str(artifact))
    linked=subprocess.check_output(['ldd',relative(artifact)],cwd=ROOT,text=True)
    assert 'not found' not in linked and 'gem5' not in linked
    for target in re.findall(r'=> (\S+)',linked):
        p=Path(target)
        assert p.is_relative_to(ROOT) or str(p).startswith(('/lib/','/lib64/','/usr/lib/')),target
    symbols=subprocess.check_output(['nm','-C',relative(artifact)],cwd=ROOT,text=True)
    assert 'gem5::' not in symbols and 'sc_core::' not in symbols
    if artifact.name=='libcoralnpu-native.so':
        library=ctypes.CDLL(str(artifact));library.coralnpu_configuration.restype=ctypes.c_char_p
        actual=json.loads(library.coralnpu_configuration())
        assert actual['rvv_enabled'] and actual['float_enabled'], 'Native model requires RVV and FP32'
print('环境与库隔离检查通过；计算验收需要实际运行用户项目。')
