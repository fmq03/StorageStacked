#!/usr/bin/env python3
"""Check relative Markdown links in maintained repository files (not generated runs)."""
import re
from pathlib import Path
from urllib.parse import unquote, urlsplit
ROOT=Path(__file__).resolve().parents[1]
SKIP={'.git','.vscode','.cache','third_party','coralnpu','build','results','sim','__pycache__'}
# Generated run output, as listed in .gitignore; same-named maintained docs elsewhere are still checked.
GENERATED=re.compile(r'^(?:(?:axi|vortex|coralnpu)_StorageStacked/validation/|(?:vortex|coralnpu)_StorageStacked/user/[^/]+/result/)')

def check(root=ROOT):
    errors=[];count=0
    for path in sorted(root.rglob('*.md')):
        relative=path.relative_to(root)
        if any(p in SKIP for p in relative.parts) or GENERATED.match(relative.as_posix()):continue
        text=re.sub(r'```.*?```','',path.read_text(),flags=re.S)
        text=re.sub(r'`[^`]*`|\$\$.*?\$\$|\$[^$]*\$|\\\[.*?\\\]','',text,flags=re.S)
        for match in re.finditer(r'\]\((<[^>]+>|[^\s)]+)(?:\s+"[^"]*")?\)',text):
            value=match[1].strip('<>');url=urlsplit(value)
            if url.scheme or url.netloc or not url.path or value.startswith('/'):continue
            target=(path.parent/unquote(url.path)).resolve();count+=1
            if not target.exists():
                line=text.count('\n',0,match.start())+1
                errors.append(f'{relative}:{line}: {value}')
    return count,errors

if __name__=='__main__':
    count,errors=check()
    for error in errors:print(error)
    print(f'{count} relative links checked; {len(errors)} broken')
    raise SystemExit(bool(errors))
