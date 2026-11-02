#!/usr/bin/env python3
"""Collect explicit test evidence; test formats and expected cases belong to callers."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def start(directory, expected):
    directory = Path(directory).resolve()
    run = directory / 'runs' / uuid.uuid4().hex
    run.mkdir(parents=True)
    write(directory / 'current.json', {'run': str(run)})
    write(directory / 'summary.json', dict(passed=False, executed=0, expected=expected,
                                          failed=expected, cases=[]))
    return run


def current(directory):
    directory = Path(directory).resolve()
    if not (directory / 'current.json').exists():
        return start(directory, 0)
    return Path(json.loads((directory / 'current.json').read_text())['run'])


def record(directory, name, command, returncode, pass_count, fail_count, **evidence):
    if not re.fullmatch(r'[\w.-]+', name):
        raise ValueError('Invalid case name: ' + name)
    passed = returncode == 0 and pass_count > 0 and fail_count == 0 and evidence.get('complete', True)
    case = dict(name=name, command=command, returncode=returncode,
                pass_count=pass_count, fail_count=fail_count, passed=passed, **evidence)
    write(current(directory) / (name + '.json'), case)
    return case


def finish(directory, expected, names):
    directory = Path(directory).resolve()
    run = current(directory)
    cases = [json.loads(p.read_text()) for p in sorted(run.glob('*.json')) if p.name != 'summary.json']
    actual = {c['name'] for c in cases}
    requested = set(names)
    complete = len(names) == len(requested) == expected and actual == requested
    failed = sum(not c['passed'] for c in cases)
    report = dict(passed=complete and len(cases) == expected and expected > 0 and failed == 0,
                  executed=len(cases), expected=expected, failed=failed,
                  missing=sorted(requested-actual), unexpected=sorted(actual-requested), cases=cases)
    write(run / 'summary.json', report)
    write(directory / 'summary.json', report)
    return report


def execute(options):
    command = options.command
    if command and command[0] == '--':
        command = command[1:]
    if not command:
        raise ValueError('A test command is required')
    log = current(options.directory) / (options.name + '-' + uuid.uuid4().hex + '.log')
    with log.open('wb') as stream:
        try:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
            returncode = result.returncode
        except OSError as error:
            stream.write(str(error).encode())
            returncode = 127
    output = log.read_text(errors='replace')
    print(output, end='')
    if options.counts:
        matches = list(re.finditer(options.counts, output, re.M))
        pass_count = sum(int(m[1]) for m in matches)
        fail_count = sum(int(m[2]) for m in matches)
    else:
        matches = list(re.finditer(options.pass_pattern, output, re.M))
        pass_count = len(matches)
        fail_count = len(list(re.finditer(options.fail_pattern, output, re.M)))
    complete = bool(matches) and (not options.require or bool(re.search(options.require, output, re.M)))
    return record(options.directory, options.name, command, returncode, pass_count, fail_count,
                  complete=complete, log=str(log))['passed']


def ctest(directory, build):
    plan = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True))
    names = [test['name'] for test in plan['tests']]
    run = start(directory, len(names))
    junit = run / 'ctest.xml'
    command = ['ctest', '--test-dir', str(build), '--output-on-failure', '--output-junit', str(junit)]
    with (run / 'ctest.log').open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    print((run / 'ctest.log').read_text())
    if junit.exists():
        for case in ET.parse(junit).getroot().iter('testcase'):
            ok = case.get('status') == 'run' and case.find('failure') is None and case.find('skipped') is None
            record(directory, case.attrib['name'], command, 0 if ok else 1, int(ok), int(not ok),
                   junit=str(junit))
    report = finish(directory, len(names), names)
    if result.returncode:
        report['passed'] = False
        write(run / 'summary.json', report)
        write(Path(directory) / 'summary.json', report)
    return report['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    init = sub.add_parser('init'); init.add_argument('--directory', required=True); init.add_argument('--expected', type=int, required=True)
    run = sub.add_parser('run'); run.add_argument('--directory', required=True); run.add_argument('--name', required=True)
    run.add_argument('--counts'); run.add_argument('--pass-pattern', default=r'^\s*\[PASS\]')
    run.add_argument('--fail-pattern', default=r'^\s*\[FAIL\]'); run.add_argument('--require')
    run.add_argument('command', nargs=argparse.REMAINDER)
    end = sub.add_parser('finish'); end.add_argument('--directory', required=True); end.add_argument('--expected', type=int, required=True)
    end.add_argument('--names', nargs='+', required=True)
    check = sub.add_parser('check'); check.add_argument('path', type=Path)
    native = sub.add_parser('ctest'); native.add_argument('--directory', required=True); native.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    if args.action == 'init':
        start(args.directory, args.expected); passed = True
    elif args.action == 'run':
        passed = execute(args)
    elif args.action == 'finish':
        report = finish(args.directory, args.expected, args.names)
        print(json.dumps({k:v for k,v in report.items() if k != 'cases'}, ensure_ascii=False)); passed = report['passed']
    elif args.action == 'ctest':
        passed = ctest(args.directory, args.build)
    else:
        report = json.loads(args.path.read_text())
        passed = report['passed'] is True and report['executed'] == report['expected'] > 0 and report['failed'] == 0
        passed = passed and len(report['cases']) == report['executed'] and all(c['passed'] and c['returncode']==0 and c['pass_count']>0 and c['fail_count']==0 for c in report['cases'])
        print(str(args.path) + ': ' + ('PASS' if passed else 'FAIL'))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
