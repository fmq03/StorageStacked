#!/usr/bin/env python3
"""Compare deterministic native evidence across flat and grouped result layouts.

CSV traces, returned bytes, memory configuration/core statistics, API checks and
summary values are deterministic. Logs, VCD generation headers, HTML and binary
sizes contain build/run provenance and are not byte-comparison baselines.
Only explicit presentation path fields are normalized in JSON summaries.
"""
import argparse
import json
from pathlib import Path
import re
import sys

# S6b intentionally adds the default trace mode to config.json. This is the
# sole expected addition (NEXT_STEPS 0.4), with exact paths for each producer.
# Never register times, cycles, data or raw records here.
EXPECTED_ADDITIONS = ({
    'artifact': 'config.json',
    'paths': {'axi': '/axi/trace_level', 'coralnpu': '/axi/trace_level',
              'vortex': '/systemc_kernel/system/axi/trace_level'},
    'reference': '<missing>', 'candidate': 'full',
    'reason': 'S6b records the full trace default explicitly',
},)

JSON_EVIDENCE={'responses.json','summary.json','memsim_core.json','memsim_config.json',
               'api_check.json','check_summary.json','aou_check_summary.json',
               'protocol_summary.json','aou_summary.json','link_check_summary.json',
               'memsim_bridge_summary.json','completion.json','soc_summary.json','host_summary.json',
               'llm_summary.json','ucie_config.json','resolved.json','input.json','config.json',
               'npu_model.json','memsim_journeys.json',
               'memsim_check.json','link_summary.json','memsim_image.json'}
GROUPS={'config','logs','checks','views'}
TRACE_LAYERS={'axi','flit','ucie','memsim','vortex','coralnpu'}
PATH_KEYS={'source','report','output_directory'}

def logical(path):
    parts=list(path.parts);result=[];i=0
    while i<len(parts):
        if parts[i] in GROUPS:
            i+=1;continue
        if parts[i]=='traces':
            i+=1
            if i<len(parts) and parts[i] in TRACE_LAYERS:i+=1
            continue
        result.append(parts[i]);i+=1
    return '/'.join(result)

def inventory(root):
    found={}
    for path in sorted(root.rglob('*')):
        if not path.is_file():continue
        key=logical(path.relative_to(root))
        if key in found:raise ValueError('Ambiguous artifact '+key+' in '+str(root))
        found[key]=path
    return found

def normalized(value,root,key=''):
    if isinstance(value,dict):return {k:normalized(v,root,k) for k,v in value.items()}
    if isinstance(value,list):return [normalized(v,root,key) for v in value]
    if isinstance(value,str) and key in PATH_KEYS:
        value=value.replace(str(root),'<RUN>')
        value=re.sub(r'(?:test-|acceptance-|burst-)\d{8}-\d{6}(?:-\d+)?','<RUN>',value)
        if key=='source' and root.name in value.split('/'):
            value=value.replace(root.name,'<RUN>')
    return value

def normalized_json_text(text, root):
    """Replace only declared path values; preserve every other byte of JSON text."""
    pattern=re.compile(r'(?<!\\)"(source|report|output_directory)"\s*:\s*("(?:[^"\\]|\\.)*")')
    def replace(match):
        value=json.loads(match[2]);updated=normalized(value,root,match[1])
        if value==updated:return match[0]
        quoted=json.dumps(updated,ensure_ascii='\\u' in match[2])
        return match[0][:-len(match[2])]+quoted
    return pattern.sub(replace,text)


def without_member(text, path):
    """Remove one exact JSON member, preserving all unrelated bytes."""
    decoder = json.JSONDecoder()
    def skip(pos):
        while pos < len(text) and text[pos].isspace(): pos += 1
        return pos
    def visit(pos, keys):
        pos = skip(pos)
        if text[pos] != '{': raise ValueError('Expected JSON object at addition path')
        pos += 1
        previous_comma = None
        while text[skip(pos)] != '}':
            member_start = pos
            name, end = decoder.raw_decode(text, skip(pos))
            colon = skip(end)
            if text[colon] != ':': raise ValueError('Expected JSON member colon')
            value_start = skip(colon + 1)
            _, end = decoder.raw_decode(text, value_start)
            following = skip(end)
            if name == keys[0]:
                if len(keys) > 1: return visit(value_start, keys[1:])
                if text[following] == ',': return member_start, following + 1
                return previous_comma if previous_comma is not None else member_start, end
            if text[following] != ',': break
            previous_comma, pos = following, following + 1
        raise ValueError('Missing expected addition member')
    start, end = visit(0, path.strip('/').split('/'))
    return text[:start] + text[end:]


def expected_additions(key, kind, reference, candidate, text):
    additions = []
    missing = object()
    def lookup(value, path):
        for part in path.strip('/').split('/'):
            if not isinstance(value, dict) or part not in value: return missing
            value = value[part]
        return value
    for rule in EXPECTED_ADDITIONS:
        if Path(key).name != rule['artifact']: continue
        path = rule['paths'][kind]
        if lookup(reference, path) is missing and lookup(candidate, path) == rule['candidate']:
            text = without_member(text, path)
            additions.append(dict(status='expected_addition', artifact=key, path=path,
                                  reference=rule['reference'], candidate=rule['candidate']))
    return text, additions


def compare(reference,candidate,kind):
    reference=Path(reference).resolve();candidate=Path(candidate).resolve()
    if not reference.is_dir() or not candidate.is_dir():raise ValueError('Both result directories must exist')
    left=inventory(reference);right=inventory(candidate)
    evidence=lambda key:Path(key).suffix=='.csv' or Path(key).name in JSON_EVIDENCE
    selected={key for key in left if evidence(key)}
    if not selected:raise ValueError('No deterministic evidence found')
    missing=sorted(selected-right.keys());extra=sorted(key for key in right if evidence(key) and key not in left)
    changed=[];failed=[];equal=0;raw_equal=0;normalized_only=0;additions=[]
    for key in sorted(selected & right.keys()):
        a,b=left[key],right[key]
        identical=a.read_bytes()==b.read_bytes()
        if identical:raw_equal+=1
        if a.suffix=='.json' and (not identical or a.name=='summary.json'):
            av=json.loads(a.read_text());bv=json.loads(b.read_text())
            if a.name=='summary.json':
                def passed(value):
                    if not isinstance(value,dict) or not value:return False
                    if 'passed' in value:return value['passed'] is True
                    return all(passed(item) for item in value.values())
                if not passed(av):failed.append('reference/'+key)
                if not passed(bv):failed.append('candidate/'+key)
            at=normalized_json_text(a.read_bytes().decode('utf8'),reference)
            bt=normalized_json_text(b.read_bytes().decode('utf8'),candidate)
            same=at==bt
            if not same:
                stripped, accepted=expected_additions(key,kind,av,bv,bt)
                if accepted and at==stripped:
                    additions.extend(accepted)
                    continue
        else:same=identical
        if same:
            equal+=1
            if not identical:normalized_only+=1
        else:changed.append(key)
    for key in left.keys() & right.keys():
        if Path(key).name!='environment.json':continue
        av=json.loads(left[key].read_text());bv=json.loads(right[key].read_text())
        for field in ('toolchain','systemc'):
            if field in av and field in bv and av[field]!=bv[field]:changed.append(key+':'+field)
    return dict(passed=not missing and not extra and not changed and not failed,kind=kind,
                reference=str(reference),candidate=str(candidate),compared=len(selected),equal=equal,raw_byte_equal=raw_equal,normalized_paths_only=normalized_only,
                expected_addition_count=len(additions),expected_additions=additions,
                missing=missing,extra=extra,changed=changed,failed_summaries=failed)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('reference',type=Path);p.add_argument('candidate',type=Path)
    p.add_argument('--kind',choices=('axi','vortex','coralnpu'),required=True)
    p.add_argument('--json-output',type=Path)
    a=p.parse_args()
    try:result=compare(a.reference,a.candidate,a.kind)
    except (OSError,ValueError) as error:result=dict(passed=False,error=str(error))
    text=json.dumps(result,ensure_ascii=False,indent=2)+'\n'
    if a.json_output:
        a.json_output.parent.mkdir(parents=True,exist_ok=True);a.json_output.write_text(text)
    print(text,end='');return 0 if result['passed'] else 1

if __name__=='__main__':sys.exit(main())
