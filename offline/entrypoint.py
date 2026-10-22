"""Run the bundled projects and retain fresh, verified evidence in /results."""
import argparse
import atexit
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

ROOT = Path(os.environ['STORAGE_WORKSPACE'])
OUTPUT = Path('/results')

def release_results():
    if 'DELIVERY_UID' not in os.environ:
        return
    uid, gid = int(os.environ['DELIVERY_UID']), int(os.environ['DELIVERY_GID'])
    # Do not follow symlinks when assigning ownership of generated evidence.
    for directory, children, files in os.walk(OUTPUT):
        for name in children + files:
            os.lchown(Path(directory)/name, uid, gid)
        os.chown(directory, uid, gid)

atexit.register(release_results)
RUNTIMES = {'vortex': ('vortex_StorageStacked', 'third_party/gem5/runtime'),
            'coralnpu': ('coralnpu_StorageStacked', 'coralnpu/runtime')}

def invoke(command, cwd=ROOT):
    subprocess.run(list(map(str, command)), cwd=cwd, check=True)

def checked(path):
    value = json.loads(path.read_text())
    if value.get('passed') is not True:
        raise RuntimeError(f'验收未通过：{path}')
    return value

def run_device(device, project, session, config=None):
    name, _ = RUNTIMES[device]
    base = ROOT/name/'user'/project
    if not (base/'src/Makefile').is_file():
        raise ValueError(f'找不到项目：{device}/{project}')
    run_id = 'offline-'+uuid.uuid4().hex
    internal = base/'result'/run_id
    args = ['bash', ROOT/name/'user/run.sh', project, '--output', 'result/'+run_id]
    if config:
        snapshot = base/(run_id+'.json')
        shutil.copyfile(config, snapshot)
        args += ['--config', snapshot.name]
    target = session/device/project
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        invoke(args)
        summary = checked(internal/'summary.json')
    finally:
        if internal.exists():
            shutil.move(str(internal), str(target))
        if config:
            snapshot.unlink(missing_ok=True)
    return {'passed': True, 'summary': str((target/'summary.json').relative_to(session)),
            'output': summary.get('smoke', {}).get('output', summary.get('llm', {}).get('generated_text'))}

def run_axi(session, args, acceptance=False):
    target = session/'axi'
    # Isolate the private C++ runtime without loading it into the device drivers.
    env = dict(os.environ)
    prefix = ROOT/'vortex_StorageStacked/third_party/.cache/tools/toolchain'
    env['PATH'] = str(prefix/'bin')+':'+env['PATH']
    env['LD_LIBRARY_PATH'] = str(prefix/'lib')
    command = ['bash', ROOT/'axi_StorageStacked/run.sh', 'test' if acceptance else 'run',
               '--output', target, *args]
    subprocess.run(list(map(str,command)), cwd=ROOT, env=env, check=True)
    checked(target/'summary.json')
    return {'passed': True, 'summary': 'axi/summary.json'}

def regression(device, session):
    name, runtime = RUNTIMES[device]
    cache = ROOT/name/('third_party/.cache' if device == 'vortex' else 'coralnpu/.cache')/'validation'
    before = set(cache.glob('*'))
    command = ['bash', '-c', 'source "$1/environment.sh"; exec "$AXI_PYTHON" "$1/test.py"',
               'offline-test', str(ROOT/name/runtime)]
    try:
        invoke(command, ROOT/name)
    finally:
        destination = session/device
        destination.mkdir(parents=True, exist_ok=True)
        # Preserve the original relative topology for reports and case evidence.
        for project in (ROOT/name/'user').iterdir():
            if project.is_dir() and (project/'result').exists():
                shutil.copytree(project/'result', destination/'user'/project.name/'result', dirs_exist_ok=True)
        for item in set(cache.glob('*')) - before:
            target = destination/item.relative_to(ROOT/name)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(item,target)
    summaries = list((session/device/('third_party/.cache' if device == 'vortex' else 'coralnpu/.cache')/'validation').glob('*/summary.json'))
    if not summaries:
        raise RuntimeError('回归未生成汇总报告')
    for p in summaries:
        checked(p)
    return {'passed': True, 'summaries': [str(p.relative_to(session)) for p in summaries]}

def main():
    command = sys.argv[1] if len(sys.argv) > 1 else 'all'
    args = sys.argv[2:]
    if command in ('help','--help','-h'):
        print('用法：all | smoke | llm | axi [参数] | vortex <项目> [--config 文件] | coralnpu <项目> [--config 文件] | test | check | shell')
        return
    if command == 'shell':
        os.chdir(ROOT)
        os.execvp('bash', ['bash', *args])
    if command == 'check':
        print((ROOT/'SOURCE_VERSIONS.json').read_text())
        script = """set -e
source "$1/environment.sh"
"$AXI_PYTHON" -c 'import ctypes,sys; ctypes.CDLL(sys.argv[1])' "$MEMSIM_BUILD/libstoragestacked_memsim.so"
"$AXI_GEM5_BIN" --help
"""
        invoke(['bash', '-c', script, 'check', ROOT/RUNTIMES['vortex'][0]/RUNTIMES['vortex'][1]])
        invoke(['bash','-c','source "$1/environment.sh"; "$AXI_PYTHON" "$1/check.py"', 'check', ROOT/RUNTIMES['coralnpu'][0]/RUNTIMES['coralnpu'][1]])
        print('PASS：工具与库检查通过；计算验收请执行 all 或 test。')
        return
    if command not in ('all','smoke','llm','axi','vortex','coralnpu','test'):
        raise ValueError('未知命令；执行 help 查看用法')
    if command not in ('axi','vortex','coralnpu') and args:
        raise ValueError('此命令不接受额外参数')
    if command in RUNTIMES:
        parser = argparse.ArgumentParser()
        parser.add_argument('project', nargs='?', default='smoke')
        parser.add_argument('--config', type=Path)
        options = parser.parse_args(args)
        if options.project in ('.','..') or '/' in options.project:
            raise ValueError('项目名必须是 user/ 下的单个目录名')
    OUTPUT.mkdir(exist_ok=True)
    session = OUTPUT/(datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8])
    session.mkdir()
    summary = {'passed': False, 'command': command, 'source_versions': json.loads((ROOT/'SOURCE_VERSIONS.json').read_text()), 'cases': {}}
    def save():
        (session/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
    save()
    try:
        if command in ('all','test','axi'):
            summary['cases']['axi'] = run_axi(session,args if command=='axi' else [],command!='axi')
            save()
        if command in RUNTIMES:
            summary['cases'][command+'/'+options.project] = run_device(command,options.project,session,options.config)
        elif command == 'test':
            for device in RUNTIMES:
                summary['cases'][device] = regression(device,session)
                save()
        elif command in ('all','smoke','llm'):
            for device in RUNTIMES:
                for project in (('smoke','llm') if command=='all' else (command,)):
                    summary['cases'][device+'/'+project] = run_device(device,project,session)
                    save()
        summary['passed'] = True
        save()
        print('PASS：'+str(session/'summary.json'),flush=True)
    except BaseException as error:
        summary['error'] = str(error)
        save()
        print('FAIL：'+str(session/'summary.json'),file=sys.stderr,flush=True)
        raise

if __name__ == '__main__':
    main()
