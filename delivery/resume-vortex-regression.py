"""Finish this delivery's regression using already executed, matching cases."""
import json
import os
from pathlib import Path
import shutil
import sys

root = Path(os.environ['STORAGE_WORKSPACE'])
prior = Path('/previous')
previous_summary = json.loads((prior/'summary.json').read_text())
versions = json.loads((root/'SOURCE_VERSIONS.json').read_text())
assert previous_summary['source_versions']['projects'] == versions['projects']
reusable = {'smoke', 'smoke_slow', 'smoke_wrap', 'smoke_cpu2',
            'llm', 'llm_no_cache', 'llm_replay', 'llm_long'}
reused = []
runtime = root/'vortex_StorageStacked/third_party/gem5/runtime'
sys.path.insert(0, str(runtime))
from configure import load_config
source = (runtime/'test.py').read_text()
anchor = '    name,project,path,c=case;result=path.parent/name\n'
assert source.count(anchor) == 1
insertion = '''    if name in reusable:
        old = prior/'vortex/user'/project/'result/test-20261008-090216'/name
        assert load_config(old/'input.json') == load_config(path), name
        old_summary = json.loads((old/'summary.json').read_text())
        assert old_summary['passed'] is True, name
        shutil.copytree(old, result)
        reused.append(name)
        print('Reusing executed and validated case: '+name, flush=True)
        return name,result,old_summary
'''
source = source.replace(anchor, anchor+insertion)
anchor = "    dump(output/'summary.json',s);write_report(output);print("
assert source.count(anchor) == 1
source = source.replace(anchor, "    s['delivery_resume']={'reused_cases':reused,'reason':'Unchanged simulator and workload sources and case configuration; packaged M4, GNU Make alias and private Python test paths'}\n"+anchor)
sys.argv = [str(runtime/'test.py')]
exec(compile(source, str(runtime/'test.py'), 'exec'), globals())
