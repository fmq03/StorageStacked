import fcntl
import os
import subprocess
import sys
from paths import ROOT,CACHE,BUILD,relative
from configure import load_config
from platform_build import build_platform,build_simulator
CACHE.mkdir(exist_ok=True)
with (CACHE/'build.lock').open('a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX)
    build_platform()
    for name in ('smoke','llm'):
        c=load_config(ROOT/'user'/name/'config.json');build_simulator(c)
        subprocess.run(['make','-C','user/'+name+'/src','OUT=../result/build'],cwd=ROOT,check=True)
subprocess.run([sys.executable,'coralnpu/runtime/check.py'],cwd=ROOT,check=True)
print('平台及两个用户项目编译完成。运行：cd user && ./run.sh smoke',flush=True)
if sys.argv[1:] == ['1']:
    subprocess.run([sys.executable,'coralnpu/runtime/test.py'],cwd=ROOT,check=True)
