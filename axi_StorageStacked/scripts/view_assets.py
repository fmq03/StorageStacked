"""Bounded offline data chunks; script loading also works from file:// URLs."""
import json
from pathlib import Path


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')


def chunks(directory, name, rows, size=128, fields=()):
    target = directory / (name + '_data')
    target.mkdir(exist_ok=True)
    manifest = []
    for start in range(0, len(rows), size):
        block = rows[start:start + size]
        filename = f'{start // size:05d}.js'
        relative = target.name + '/' + filename
        (target / filename).write_text('TraceStore.receive(' + compact(relative) + ',' + compact(block) + ');\n')
        item = {'file': relative, 'count': len(block)}
        for field in fields:
            item[field] = sorted({str(r[field]) for r in block})
        if 'seq' in block[0]:
            item['lo'] = min(int(r['seq']) for r in block)
            item['hi'] = max(int(r['seq']) for r in block)
        manifest.append(item)
    # These are generated chunks belonging only to this view, never raw evidence.
    expected = {Path(m['file']).name for m in manifest}
    for path in target.glob('*.js'):
        if path.name not in expected:
            path.unlink()
    (directory / 'view_store.js').write_text(Path(__file__).with_name('view_store.js').read_text())
    return manifest
