"""Group saved run artifacts while keeping the flat simulator/checker interface."""
from contextlib import contextmanager
from pathlib import Path
import re

GROUPS = ('config', 'logs', 'build', 'traces', 'checks', 'views')
CONFIG = {'input.json', 'resolved.json', 'config.json', 'ucie_config.json',
          'memsim_config.json', 'npu_model.json', 'environment.json'}
VIEWS = {'trace_flits_data', 'trace_paths_data', 'memsim_data', 'view_store.js'}


def location(name):
    path = Path(name)
    first = path.parts[0]
    if first in GROUPS or first in ('report.md', 'summary.json'):
        return path
    if first in CONFIG:
        return Path('config') / path
    if first.endswith('.log'):
        return Path('logs') / path
    if first in VIEWS or Path(first).suffix in ('.html', '.svg', '.gtkw'):
        return Path('views') / path
    if first == 'wave_audit' or Path(first).suffix == '.json':
        return Path('checks') / path
    return Path('traces') / path


def artifact(root, name):
    """Read both historical flat results and grouped results."""
    root = Path(root)
    flat = root / name
    return flat if flat.exists() else root / location(name)


def grouped(root):
    return any((root / group).is_dir() for group in GROUPS if group != 'build')


def _rewrite(root, moves):
    """Only presentation links change; raw logs, JSON, CSV and VCD stay intact."""
    if not moves:
        return
    import os
    files = [p for p in root.rglob('*') if p.is_file() and
             p.suffix in ('.md', '.html', '.gtkw')]
    for path in files:
        # Links were relative to this file's previous location.
        current = path.relative_to(root)
        previous = next((old for old, new in moves.items() if new == current), current)
        replacements = {}
        for old, new in moves.items():
            before = os.path.relpath(old, previous.parent)
            after = os.path.relpath(new, current.parent)
            if before != after:
                replacements[before] = after
        if not replacements:
            continue
        pattern = re.compile(r'(?<![\w/.-])(' + '|'.join(
            re.escape(k) for k in sorted(replacements, key=len, reverse=True)) + r')(?![\w/.-])')
        text = path.read_text()
        updated = pattern.sub(lambda m: replacements[m[0]], text)
        if text != updated:
            path.write_text(updated)


def organize(root):
    root = Path(root)
    moves = {}
    for source in list(root.iterdir()):
        if source.name.startswith('.'):
            continue
        target = root / location(source.name)
        if source == target:
            continue
        if target.exists():
            raise FileExistsError(f'Refusing to replace saved evidence: {target}')
        entries = list(source.rglob('*')) if source.is_dir() else [source]
        for item in entries:
            if item.is_file():
                moves[item.relative_to(root)] = target.relative_to(root) / item.relative_to(source) if source.is_dir() else target.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        source.rename(target)
    _rewrite(root, moves)
    return moves


def unpack(root):
    """Temporarily restore the shared storage tools' original file interface."""
    root = Path(root)
    moves = {}
    for group in GROUPS:
        if group == 'build' or not (root / group).is_dir():
            continue
        for source in list((root / group).iterdir()):
            target = root / source.name
            if target.exists():
                raise FileExistsError(f'Duplicate saved artifact: {target}')
            entries = list(source.rglob('*')) if source.is_dir() else [source]
            for item in entries:
                if item.is_file():
                    moves[item.relative_to(root)] = target.relative_to(root) / item.relative_to(source) if source.is_dir() else target.relative_to(root)
            source.rename(target)
        (root / group).rmdir()
    _rewrite(root, moves)


@contextmanager
def working_result(root):
    """Revalidation accepts either layout and always restores saved grouping."""
    root = Path(root)
    if grouped(root):
        unpack(root)
    try:
        yield root
    finally:
        organize(root)


def write_index(project):
    result = project / 'result'
    if not result.is_dir():
        return
    import json
    rows = []
    for summary in sorted(result.rglob('summary.json')):
        directory = summary.parent
        if not (directory / 'report.md').is_file():
            continue
        config = artifact(directory, 'input.json')
        if not config.is_file():
            continue
        try:
            s = json.loads(summary.read_text())
            n = json.loads(config.read_text())['npu']
        except (OSError, ValueError, KeyError):
            continue
        name = directory.relative_to(result).as_posix()
        rows.append(f"| [{name}]({name}/report.md) | {'PASS' if s.get('passed') else '未通过'} | "
                    f"{'开' if n.get('fetch_l0_enabled', False) else '关'} | "
                    f"{n.get('itcm_kib', 8)} / {n.get('dtcm_kib', 32)} |")
    lines = ['# 运行结果', '', '点击场景名称查看报告；历史失败记录保留原有状态。', '',
             '| 场景 | 验收状态 | L0 | ITCM / DTCM (KiB) |', '|---|---|---|---|', *rows, '',
             '每个场景顶层保留 report.md 和 summary.json；config/ 保存配置，logs/ 保存日志，',
             'build/ 保存程序与模型参数，traces/ 保存原始记录，checks/ 保存检查结果，views/ 保存可视化。', '']
    # Matrix cases finish concurrently; publish a complete index in one rename.
    import os
    import tempfile
    descriptor, name = tempfile.mkstemp(prefix='.index-', dir=result)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write('\n'.join(lines))
        os.replace(name, result / 'README.md')
    finally:
        Path(name).unlink(missing_ok=True)
