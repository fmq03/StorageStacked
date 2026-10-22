"""Configuration snapshots and device setup used by the shell project runner."""
import argparse
import json
from pathlib import Path
import sys

from paths import ROOT, BUILD, MEMSIM_BUILD, VORTEX_BUILD, ARM_SYSROOT, gem5_binary, host_runtime, relative, dump
from configure import load_config, resolved_parts, address_map
from storage_dependency import require_build, storage_version
from elf_model import read as read_model
from report import write_status
from portable import sanitize


def local_path(base, value):
    if Path(value).is_absolute():
        raise ValueError('Use a relative path')
    path = (base / value).resolve()
    path.relative_to(base.resolve())
    return path


def prepare(project_name, config_name, output_name):
    project = local_path(ROOT / 'user', project_name)
    config = load_config(local_path(project, config_name))
    output = local_path(project, output_name)
    output.relative_to(project / 'result')
    require_build()
    if not (project / 'src/Makefile').is_file():
        raise ValueError('Project requires src/Makefile')
    output.mkdir(parents=True, exist_ok=False)
    dump(output / 'input.json', config)
    write_status(output, 'build')


def resolve(output):
    config = load_config(output / 'input.json')
    architecture, benchmark = resolved_parts(config)
    isa=architecture['host']['isa'];gem5=gem5_binary(isa)
    elf = output / 'build/program.elf'
    if not elf.is_file():
        raise ValueError('Makefile did not produce build/program.elf')
    for name in ('program.vxbin','host.elf','readback.json','programs.json'):
        if not (output/'build'/name).is_file():raise ValueError('Makefile did not produce '+name)
    resolved={'device':'vortex','time_unit':'1 fs','architecture':architecture,
              'benchmark':benchmark,'address_map':address_map(architecture),
              'host_binary':'build/host.elf','kernel':'build/program.vxbin',
              'library':relative(VORTEX_BUILD/'sim/simx/libvortex-gem5.so',output),
              'runtime_dir':relative(host_runtime(isa),output)}
    if isa=='aarch64':resolved['host_sysroot']=relative(ARM_SYSROOT,output)
    if benchmark['name']=='tiny_llm':resolved['llm_model']=read_model(elf)
    dump(output/'resolved.json',resolved)
    files=[elf,output/'build/program.vxbin',output/'build/host.elf',gem5,
           host_runtime(isa)/'libvortex.so',host_runtime(isa)/f'libvortex-gem5-{isa}.so',
           VORTEX_BUILD/'sim/simx/libvortex-gem5.so',MEMSIM_BUILD/'libstoragestacked_memsim.so']
    dump(output/'environment.json',{'storage':storage_version(),
         'files':[{'name':relative(path,output),'bytes':path.stat().st_size} for path in files]})
    (output/'build/simulator.path').write_text(relative(gem5,output)+'\n')
    (output/'build/system.path').write_text(relative(ROOT/'integration/system.py',output)+'\n')


def capture(output):
    # Keep the raw guest streams; append complete records after gem5 has exited.
    # Simulator diagnostics must never interrupt a long guest READBACK line.
    with (output/'run.log').open('ab') as stream:
        for name in ('host_stdout.log','host_stderr.log'):
            path=output/name
            if path.is_file():
                stream.write(('\n['+name+']\n').encode())
                stream.write(path.read_bytes())


def finish(output, stage, code):
    try:
        try:
            summary = json.loads((output / 'summary.json').read_text())
        except (ValueError, OSError):
            summary = {}
        if not isinstance(summary, dict):
            summary = {}
        if code or not summary.get('passed'):
            log = {'build': 'build.log', 'simulate': 'run.log', 'validate': 'validation.log'}[stage]
            error = summary.get('error') or f'{stage} failed (exit {code}); see {log}'
            write_status(output, stage, error)
            if not code:
                raise RuntimeError('Validation did not produce a passing summary')
    finally:
        sanitize(output)


def main():
    parser = argparse.ArgumentParser()
    actions = parser.add_subparsers(dest='action', required=True)
    command = actions.add_parser('prepare')
    for name in ('project', 'config', 'output'):
        command.add_argument(name)
    for action in ('resolve', 'capture', 'status', 'finish'):
        command = actions.add_parser(action)
        command.add_argument('output', type=Path)
        if action in ('status', 'finish'):
            command.add_argument('stage', choices=('build', 'simulate', 'validate'))
        if action == 'finish':
            command.add_argument('code', type=int)
    options = parser.parse_args()
    if options.action == 'prepare':
        prepare(options.project, options.config, options.output)
    elif options.action == 'resolve':
        resolve(options.output.resolve())
    elif options.action == 'capture':
        capture(options.output.resolve())
    elif options.action == 'status':
        write_status(options.output, options.stage)
    else:
        finish(options.output, options.stage, options.code)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error).replace(str(ROOT), '.'), file=sys.stderr)
        sys.exit(1)
