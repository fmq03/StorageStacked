"""Configuration snapshots and device setup used by the shell project runner."""
import argparse
import json
from pathlib import Path
import sys

from paths import ROOT, BUILD, MEMSIM_BUILD, relative, dump
from configure import load_config, resolved_parts
from storage_dependency import require_build, storage_version
from platform_build import build_simulator
from rtl_build import native_library
from elf_model import read as read_model
from report import write_status
from portable import sanitize
from result_layout import organize, write_index


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
    elf = output / 'build/program.elf'
    if not elf.is_file():
        raise ValueError('Makefile did not produce build/program.elf')
    simulator = build_simulator(config)
    resolved = {'device': 'coralnpu', 'architecture': architecture,
                'benchmark': benchmark, 'kernel': 'build/program.elf'}
    if benchmark['name'] == 'tiny_llm':
        resolved['llm_model'] = read_model(elf)
    dump(output / 'resolved.json', resolved)
    import shutil
    shutil.copy2(native_library(config).parent/'model_parameters.h',output/'build/model_parameters.h')
    shutil.copy2(native_library(config).parent/'build.json',output/'build/rtl.json')
    files = (elf, simulator, native_library(config),
             MEMSIM_BUILD / 'libstoragestacked_memsim.so')
    dump(output / 'environment.json', {
        'storage': storage_version(),
        'files': [{'name': relative(path, output), 'bytes': path.stat().st_size} for path in files]})
    (output / 'build/simulator.path').write_text(relative(simulator, output) + '\n')


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
        organize(output)
        project = next(parent.parent for parent in output.parents if parent.name == 'result')
        write_index(project)


def main():
    parser = argparse.ArgumentParser()
    actions = parser.add_subparsers(dest='action', required=True)
    command = actions.add_parser('prepare')
    for name in ('project', 'config', 'output'):
        command.add_argument(name)
    for action in ('resolve', 'status', 'finish'):
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
