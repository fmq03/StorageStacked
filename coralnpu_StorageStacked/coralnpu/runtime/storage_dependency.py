"""One explicitly selected sibling storage repository; no hidden driver dependency."""
import subprocess
from paths import ROOT, RUNTIME, BUILD, STORAGE_ROOT, MEMSIM_BUILD, relative, dump

def storage_version():
    marker=STORAGE_ROOT/'VERSION'
    if not marker.is_file() or marker.read_text().strip()!='1.0.0':
        raise RuntimeError('Missing or incompatible storage project; use ./build.sh --storage ../axi_StorageStacked')
    result={'root':relative(STORAGE_ROOT),'version':marker.read_text().strip()}
    if (STORAGE_ROOT/'.git').exists():
        result['commit']=subprocess.check_output(['git','-C',relative(STORAGE_ROOT),'rev-parse','HEAD'],cwd=ROOT,text=True).strip()
        result['dirty']=bool(subprocess.check_output(['git','-C',relative(STORAGE_ROOT),'status','--porcelain'],cwd=ROOT,text=True))
    return result

def require_build():
    if not (BUILD/'libcoralnpu-native.so').is_file() or not (MEMSIM_BUILD/'libstoragestacked_memsim.so').is_file():
        raise RuntimeError('Run ./build.sh first')
