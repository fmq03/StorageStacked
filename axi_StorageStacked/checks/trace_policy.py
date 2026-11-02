"""Light diagnostics cannot be accepted as complete link verification."""
import json
from pathlib import Path
from result_layout import artifact

def level(root):
    for name,group in [('resolved.json','architecture'),('input.json','simulation'),('input.json','config')]:
        path=artifact(root,name)
        if path.is_file():
            data=json.loads(path.read_text())
            value=data.get(group,{}).get('trace_level')
            if value is not None:return value
    return 'full'

def incomplete(root):
    result=dict(passed=False,link_verified=False,trace_level=level(root),stage='diagnostic simulation complete')
    (Path(root)/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    (Path(root)/'report.md').write_text('# 诊断运行\n\ntrace_level=light；实际仿真已结束，完整链路验收未执行。\n\nsummary.json 保持 passed=false、link_verified=false；请用 full 重新运行验收。\n')
    return result

def require_full(root):
    if level(root) != 'full':
        incomplete(root)
        raise ValueError('Link acceptance requires trace_level=full; link_verified=false')
