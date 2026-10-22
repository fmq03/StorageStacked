"""Private Bazel invocation shared by the platform and user Makefiles."""
import os
import subprocess
from paths import ROOT, CACHE, SETTINGS

def bazel(command, targets, capture=False):
    env=os.environ
    args=[str(CACHE/'tools/xpu-tools/bin/bazel'),'--output_user_root='+str(CACHE/'tools/bazel'),command,
          '--workspace_status_command=runtime/workspace_status.sh','--define=storagestacked_native_cpp=1',
          '--jobs='+str(SETTINGS['jobs'])]
    for k in ('CC','CXX'):
        args+=['--repo_env='+k+'='+env[k],'--action_env='+k+'='+env[k]]
    include=str(CACHE/'tools/xpu-native/include')
    for prefix in ('--repo_env=','--action_env=','--host_action_env='):args+=[prefix+'CPLUS_INCLUDE_PATH='+include]
    for flag in ('--linkopt=','--host_linkopt='):args+=[flag+'-L'+str(CACHE/'tools/xpu-native/lib')]
    if command=='cquery':args+=['--output=files']
    return subprocess.run(args+targets,cwd=ROOT/'coralnpu',check=True,text=True,stdout=subprocess.PIPE if capture else None).stdout
