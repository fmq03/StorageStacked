"""Keep project-generated text reports independent of the checkout location."""
from pathlib import Path
from paths import ROOT,STORAGE_ROOT,relative

def sanitize(directory):
    for p in directory.rglob('*'):
        if p.is_file() and p.suffix in ('.json','.log','.md','.html','.txt','.ini','.path'):
            text=p.read_text(errors='replace')
            clean=text.replace(str(ROOT),relative(ROOT,directory)).replace(str(STORAGE_ROOT),relative(STORAGE_ROOT,directory))
            repository=STORAGE_ROOT.parent
            clean=clean.replace(str(repository),relative(repository,directory))
            if text!=clean:p.write_text(clean)
