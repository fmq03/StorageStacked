"""Platform data routes combined with the storage manifest routes."""
from pathlib import Path
from paths import STORAGE_RESULT_ROUTES
from result_core import Layout, load_routes

layout = Layout.merged(load_routes(STORAGE_RESULT_ROUTES),
                       load_routes(Path(__file__).with_name('result_routes.json')))
artifact = layout.artifact
grouped = layout.grouped
location = layout.location
organize = layout.organize
unpack = layout.unpack
working_result = layout.working_result


def write_index(project):
    result = project / 'result'
    if not result.is_dir():
        return
    import json
    rows = []
    for summary in sorted(result.rglob('summary.json')):
        directory = summary.parent
        if not (directory / 'report.md').is_file():
            continue
        config = artifact(directory, 'input.json')
        if not config.is_file():
            continue
        try:
            s = json.loads(summary.read_text())
            n = json.loads(config.read_text())['npu']
        except (OSError, ValueError, KeyError):
            continue
        name = directory.relative_to(result).as_posix()
        rows.append(f"| [{name}]({name}/report.md) | {'PASS' if s.get('passed') else '未通过'} | "
                    f"{'开' if n.get('fetch_l0_enabled', False) else '关'} | "
                    f"{n.get('itcm_kib', 8)} / {n.get('dtcm_kib', 32)} |")
    lines = ['# 运行结果', '', '点击场景名称查看报告；历史失败记录保留原有状态。', '',
             '| 场景 | 验收状态 | L0 | ITCM / DTCM (KiB) |', '|---|---|---|---|', *rows, '',
             '每个场景顶层保留 report.md 和 summary.json；config/ 保存配置，logs/ 保存日志，',
             'build/ 保存程序与模型参数，traces/ 保存原始记录，checks/ 保存检查结果，views/ 保存可视化。', '']
    # Matrix cases finish concurrently; publish a complete index in one rename.
    import os
    import tempfile
    descriptor, name = tempfile.mkstemp(prefix='.index-', dir=result)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write('\n'.join(lines))
        os.replace(name, result / 'README.md')
    finally:
        Path(name).unlink(missing_ok=True)
