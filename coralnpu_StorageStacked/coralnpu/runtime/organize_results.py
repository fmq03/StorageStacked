"""Migrate saved user results, retaining presentation originals and a move manifest."""
import datetime
import json
import shutil
from paths import ROOT, CACHE, dump, relative
from result_layout import organize, write_index


def main():
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    record = CACHE / 'validation' / ('result-layout-' + stamp)
    record.mkdir(parents=True)
    runs = []
    for project in sorted((ROOT / 'user').iterdir()):
        result = project / 'result'
        if not result.is_dir():
            continue
        directories = sorted({p.parent for p in result.rglob('input.json')
                              if p.parent.name != 'config'})
        for directory in directories:
            # Only presentation links are rewritten. Keep their original content.
            for path in directory.rglob('*'):
                if path.is_file() and path.suffix in ('.md', '.html', '.gtkw'):
                    destination = record / 'originals' / path.relative_to(ROOT)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, destination)
            moves = organize(directory)
            runs.append({'run': relative(directory), 'files': [
                {'from': old.as_posix(), 'to': new.as_posix(),
                 'bytes': (directory / new).stat().st_size}
                for old, new in moves.items()]})
            dump(record / 'manifest.json', {'runs': runs, 'complete': False})
        write_index(project)
    dump(record / 'manifest.json', {'runs': runs, 'complete': True})
    print(json.dumps({'runs': len(runs), 'manifest': relative(record / 'manifest.json')}))


if __name__ == '__main__':
    main()
