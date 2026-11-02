"""Format-independent, instance-owned result directory layout."""
from __future__ import annotations
from contextlib import contextmanager
from pathlib import Path
from types import MappingProxyType
import json
import re

GROUPS = ('config', 'logs', 'build', 'traces', 'checks', 'views')
CONFIG = {'input.json', 'resolved.json', 'config.json', 'ucie_config.json',
          'memsim_config.json', 'npu_model.json', 'environment.json'}
TRACE_LAYERS = ('axi', 'flit', 'ucie', 'memsim', 'vortex', 'coralnpu')
VIEWS = {'trace_flits_data', 'trace_paths_data', 'memsim_data', 'view_store.js'}

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

def load_routes(path):
    routes = json.loads(Path(path).read_text())
    if not isinstance(routes, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in routes.items()):
        raise ValueError('Result routes must be a dictionary of strings')
    return routes


class Layout:
    def __init__(self, routes: dict[str, str]):
        self.routes = MappingProxyType(dict(routes))

    @classmethod
    def merged(cls, *route_dicts):
        routes = {}
        for incoming in route_dicts:
            for name, target in incoming.items():
                if name in routes and routes[name] != target:
                    raise ValueError(f'Conflicting result route for {name}: {routes[name]} != {target}')
                routes[name] = target
        return cls(routes)

    def location(self, name):
        path = Path(name)
        first = path.parts[0]
        if first in GROUPS or first in ('report.md', 'summary.json'):
            return path
        if first in self.routes:
            return Path(self.routes[first]) / Path(*path.parts[1:])
        if first in CONFIG:
            return Path('config') / path
        if first.endswith('.log'):
            return Path('logs') / path
        if first in VIEWS or Path(first).suffix in ('.html', '.svg', '.gtkw'):
            return Path('views') / path
        if first == 'wave_audit' or Path(first).suffix == '.json':
            return Path('checks') / path
        return Path('traces') / path

    def artifact(self, root, name):
        """Read both historical flat results and grouped results."""
        root = Path(root)
        flat = root / name
        if flat.exists():
            return flat
        candidates = [root / self.location(name), root / 'traces' / name]
        candidates += [root / 'traces' / layer / name for layer in TRACE_LAYERS]
        for candidate in candidates:
            if candidate.exists():
                return candidate
        return root / self.location(name) if self.grouped(root) else flat

    def grouped(self, root):
        return any((root / group).is_dir() for group in GROUPS if group != 'build')

    def organize(self, root):
        root = Path(root)
        moves = {}
        for source in list(root.iterdir()):
            if source.is_dir() and (source / 'summary.json').is_file():
                continue  # An acceptance directory may contain independent scenario runs.
            if source.name.startswith('.'):
                continue
            target = root / self.location(source.name)
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
        for group in GROUPS:
            (root / group).mkdir(exist_ok=True)
        for layer in TRACE_LAYERS:
            (root / 'traces' / layer).mkdir(exist_ok=True)
        return moves

    def unpack(self, root):
        """Temporarily restore the shared storage tools' original file interface."""
        root = Path(root)
        moves = {}
        for group in GROUPS:
            if group == 'build' or not (root / group).is_dir():
                continue
            directory = root / group
            sources = []
            for child in list(directory.iterdir()):
                if group == 'traces' and child.name in TRACE_LAYERS and child.is_dir():
                    sources.extend(child.iterdir())
                else:
                    sources.append(child)
            for source in sources:
                target = root / source.name
                if target.exists():
                    raise FileExistsError(f'Duplicate saved artifact: {target}')
                entries = list(source.rglob('*')) if source.is_dir() else [source]
                for item in entries:
                    if item.is_file():
                        moves[item.relative_to(root)] = target.relative_to(root) / item.relative_to(source) if source.is_dir() else target.relative_to(root)
                source.rename(target)
            if group == 'traces':
                for layer in TRACE_LAYERS:
                    child = directory / layer
                    if child.is_dir(): child.rmdir()
            directory.rmdir()
        _rewrite(root, moves)

    @contextmanager
    def working_result(self, root):
        """Revalidation accepts either layout and always restores saved grouping."""
        root = Path(root)
        if self.grouped(root):
            self.unpack(root)
        try:
            yield root
        finally:
            self.organize(root)
