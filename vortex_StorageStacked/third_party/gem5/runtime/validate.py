#!/usr/bin/env python3
from paths import STORAGE_MEMSIM_SOURCE, STORAGE_MEMSIM_ONLINE_CHECK, STORAGE_SOURCE, STORAGE_CHECKS
"""Validate an existing completed simulation from raw AXI/Flit/memory evidence."""
from storage_dependency import STORAGE_ROOT, MEMSIM_BUILD, storage_version
import json,subprocess,sys
from pathlib import Path
from trace_policy import level, incomplete
ROOT=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve()
from result_layout import working_result

if level(out) != 'full':
    incomplete(out)
    from result_layout import organize
    organize(out)
    sys.exit(0)

with working_result(out):
    c=json.loads((out/'resolved.json').read_text())
    (out/'summary.json').write_text(json.dumps({'passed':False,'stage':'validating saved raw evidence','device':c['device']},indent=2)+'\n')
    def run(script,args):
        with (out/'verification.log').open('a') as stream:
            subprocess.run([sys.executable,str(script),*map(str,args)],stdout=stream,stderr=subprocess.STDOUT,check=True)
    try:
        for name in ('check','check_aou','inspect_link','check_memsim'):
            args=[out]
            if name=='check_aou' and c['architecture']['axi']['replay']:args.append('--replay')
            run((ROOT/'third_party/gem5/runtime/bridge' if name == 'check' else STORAGE_CHECKS)/(name+'.py'),args)
        run(STORAGE_CHECKS/'audit_wave.py',[out,'--output',out/'wave_audit'])
        for name in ('trace_view','memsim_view'):
            run(STORAGE_CHECKS/(name+'.py'),[out])
        run(ROOT/'third_party/gem5/runtime/verify.py',[out])
        from report import write_report
        write_report(out)
        print('PASS: '+str(out/'summary.json'))
    except Exception as error:
        (out/'summary.json').write_text(json.dumps({'passed':False,'stage':'validate','error':str(error)},indent=2)+'\n')
        raise
