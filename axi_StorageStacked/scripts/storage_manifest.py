"""Resolve the storage project's public source and checker path manifest."""
import argparse
import json
import os
from pathlib import Path
import shlex

def load(root=None):
    root=Path(root or Path(__file__).resolve().parents[1]).resolve()
    raw=json.loads((root/'storage_manifest.json').read_text())
    if raw.get('version')!=1:raise ValueError('Unsupported storage manifest version')
    def resolve(value):
        p=(root/value).resolve()
        if not p.exists():raise ValueError('Missing storage path: '+str(p))
        return p
    result={}
    for key,value in raw.items():
        if key=='version':result[key]=value
        elif key=='sources':result[key]={name:[resolve(p) for p in paths] for name,paths in value.items()}
        elif isinstance(value,list):result[key]=[resolve(p) for p in value]
        else:result[key]=resolve(value)
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();group=p.add_mutually_exclusive_group(required=True)
    group.add_argument('--field');group.add_argument('--pythonpath',action='store_true')
    group.add_argument('--include-flags',action='store_true');group.add_argument('--sources')
    a=p.parse_args();data=load()
    if a.field:print(data[a.field])
    elif a.pythonpath:print(os.pathsep.join(map(str,data['python_path'])))
    elif a.include_flags:print(' '.join('-I'+shlex.quote(str(p)) for p in data['include_dirs']))
    else:print(' '.join(shlex.quote(str(p)) for p in data['sources'][a.sources]))
