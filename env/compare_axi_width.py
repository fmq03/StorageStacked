#!/usr/bin/env python3
"""Build a read-only baseline comparison in a new AXI256 result directory."""
import argparse
import csv
import html
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('result', type=Path, help='contains baseline64/, memsim/, xpu/, ram/')
parser.add_argument('old_xpu', nargs='?', type=Path, help='previous AXI64 run_xpu.sh results')
args = parser.parse_args()
root = args.result.resolve()
old_root = args.old_xpu.resolve() if args.old_xpu else None


def read(path):
    return json.loads(path.read_text())


def first_write(path):
    protocol = read(path / 'protocol_summary.json')
    width = protocol.get('axi_data_bits', 64)
    with (path / 'axi_events.csv').open() as f:
        rows = list(csv.DictReader(f))
    aw = next(r for r in rows if r['channel'] == 'AW')
    assert int(aw['address']) == 0x90000000
    count = int(aw['len']) + 1
    assert (1 << int(aw['size'])) == width // 8
    data = [r for r in rows if r['channel'] == 'W'][:count]
    raw = b''.join(int(r['data']).to_bytes(width // 8, 'little') for r in data)
    assert raw == bytes((11 + i * 37) & 255 for i in range(64))
    assert all(int(r['strb']) == (1 << (width // 8)) - 1 for r in data)
    return {'width': width, 'beats': count, 'bytes': len(raw), 'data_hex': raw.hex(),
            'period_fs': protocol['period_ticks'],
            'awsize': int(aw['size']), 'awlen': count - 1,
            'beat_details': [{'tick_fs': int(r['tick']), 'wstrb': hex(int(r['strb'])),
                              'bytes_low_address_first': int(r['data']).to_bytes(width // 8, 'little').hex()}
                             for r in data]}


if old_root is not None:
    before = first_write(root / 'baseline64')
    after = first_write(root / 'memsim/directed')
    assert (before['width'], after['width'], before['beats'], after['beats']) == (64, 256, 8, 2)
    assert before['period_fs'] == after['period_fs']
    mem = read(root / 'memsim/summary.json')
    ram = read(root / 'ram/regression_summary.json')
    new, old = read(root / 'xpu/summary.json'), read(old_root / 'summary.json')
    wide = read(root / 'memsim/directed/axi256_check.json')
    assert all(x['passed'] for x in (mem, ram, new, old, wide))
    manifests = [read(p / 'environment/xpu_manifest.json') for p in (old_root, root / 'xpu')]
    identical = {k: manifests[0]['files'][k]['sha256'] == manifests[1]['files'][k]['sha256']
                 for k in manifests[0]['files']}
    assert identical and all(identical.values()), 'XPU binaries differ: comparison is not controlled'
    cases = {}
    for name in ('npu', 'gpu', 'three', 'three_slow'):
        pair = []
        for suite in (old, new):
            c = suite['cases'][name]
            a = c['aou_check_summary']
            pair.append({'parents': c['check_summary']['transactions'],
                         'write_beats': a['target_write_beats'], 'read_beats': a['target_read_beats'],
                         'tx_flits': a['tx_flits'], 'rx_flits': a['rx_flits'],
                         'cpu_exit_ns': c['exit_tick_fs'] / 1e6, **c['devices']})
        cases[name] = dict(zip(('axi64', 'axi256'), pair))
    result = {'passed': True, 'aligned_64B_write': {'axi64': before, 'axi256': after},
              'same_xpu_binary_hashes': identical, 'xpu_comparison': cases,
              'suites': {'memsim_cases': len(mem['cases']), 'xpu_cases': len(new['cases']),
                         'ram_cases': len(ram['cases']), 'native_tests': mem['native_tests_passed']},
              'upper_lane_negative_checks': wide['negative_checks'],
              'cpu_memory_feedback': mem['cpu_feedback'], 'xpu_memory_feedback': new['memory_feedback'],
              'notes': ['The directed workload changed to retain 256-beat and full-width masked coverage; only its identical first 64B write is compared.',
                        'XPU binaries are identical. CPU/CP polling and scheduling can change request counts.',
                        '32B per beat is the bus capacity, not an end-to-end 4x performance claim.',
                        'Flit counts include protocol/control traffic and are not application payload efficiency.']}
    (root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
else:
    result = read(root / 'summary.json')
    assert result['passed']
    before, after = (result['aligned_64B_write'][k] for k in ('axi64', 'axi256'))
    cases = result['xpu_comparison']
    wide = {'negative_checks': result['upper_lane_negative_checks']}
    for suite in ('memsim', 'xpu'):
        assert read(root / suite / 'summary.json')['passed']


def link(path, label):
    if not path.exists():
        return html.escape(label) + '（旧文件已按要求清理）'
    return f'<a href="{html.escape(os.path.relpath(path, root), quote=True)}">{html.escape(label)}</a>'


def table(headers, rows):
    return '<table><thead><tr>' + ''.join(f'<th>{html.escape(str(x))}</th>' for x in headers) + '</tr></thead><tbody>' + ''.join(
        '<tr>' + ''.join(f'<td>{html.escape(str(x))}</td>' for x in row) + '</tr>' for row in rows) + '</tbody></table>'


body = '<h1>原生 AXI256 验证与 AXI64 对照</h1><p class="pass">验收通过 · 当前原生 AXI256</p>'
body += '<p>WDATA/RDATA 256bit，WSTRB 32bit；gem5 → AXI256 → AXI2Flit → UCIe → 在线 mem_sim，并沿原链路返回。Flit 格式未改。</p>'
body += '<h2>同一个对齐 64B 写请求：8 拍 → 2 拍</h2>'
body += table(['版本', '每拍字节', 'AWSIZE', 'AWLEN', 'W 拍数'],
              [(v['width'], v['width'] // 8, v['awsize'], v['awlen'], v['beats']) for v in (before, after)])
body += '<p>地址 0x90000000，写入字节完全相同。以下字节按低地址到高地址排列；时间单位 ns。</p>'
body += table(['位宽', '握手时间', 'WSTRB', '逐拍字节'],
              [(v['width'], d['tick_fs'] / 1e6, d['wstrb'], d['bytes_low_address_first'])
               for v in (before, after) for d in v['beat_details']])
body += '<p>定向用例为了继续覆盖 256 拍边界及全宽掩码写，调整了其他请求；不能直接用整组定向用例的总时间比较性能。</p>'
body += '<h2>GPU/NPU 回归对照</h2><p>所有 GPU/NPU 库、host 程序及 kernel 的 SHA256 与旧回归相同。轮询次数和调度可能随延迟变化。</p>'
body += table(['用例 / 位宽', '父请求', 'W 拍', 'R 拍', '发送 Flit', '接收 Flit', 'GPU 周期', 'NPU 周期', 'CPU 完成 ns'],
              [(name + ' / ' + width, c['parents'], c['write_beats'], c['read_beats'], c['tx_flits'], c['rx_flits'],
                c.get('gpu_cycles', '—'), c.get('npu_cycles', '—'), c['cpu_exit_ns'])
               for name, pair in cases.items() for width, c in pair.items()])
body += '<p>32B/拍表示总线容量；小访存仍使用窄传输。上述小型算子包含大量 CPU 控制及轮询，不能推论 LLM 或整条链路提速四倍。Flit 计数包含控制流量。</p>'
body += '<h2>独立校验与查看入口</h2><p>7 组在线 mem_sim、4 组 GPU/NPU、5 组 RAM 回归；14 项 mem_sim 原生测试。AXI 五通道、两端完整 Flit、DRAM/DFI、读回数据和计算结果均已校验。</p>'
body += table(['高位篡改测试', '结果', '检查器诊断'], [(k, '拒绝，符合预期', v['reason']) for k, v in wide['negative_checks'].items()])
body += '<ul>' + ''.join('<li>' + link(root / p, label) + '</li>' for p, label in [
    ('summary.json', '本页机器可读汇总'), ('memsim/directed/axi256_check.json', 'AXI256 数据与故障注入证据'),
    ('memsim/directed/axi_wave.vcd', 'AXI256 定向波形 VCD'), ('memsim/directed/trace_view.html', 'AXI / Flit 交互查看'),
    ('xpu/three/memsim_view.html', 'CPU + GPU + NPU 请求全过程'), ('xpu/summary.json', '四组 XPU 完整验收'),
    ('memsim/summary.json', '七组在线 mem_sim 完整验收'), ('ram/report.html', 'RAM 兼容性报告'),
    ('cleanup.json', '旧结果清理记录'), ('implementation.patch', 'AXI256 改动的集成代码差异')])
if old_root is not None:
    body += '<li>' + link(old_root / 'three/memsim_view.html', '旧 AXI64 三源全过程（原报告）') + '</li>'
else:
    body += '<li>旧版完整日志、波形和备份已按要求清理；本页保留清理前已核对的统计对照。</li>'
body += '</ul>'
document = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>AXI256 验证</title><style>body{font:16px/1.65 system-ui,sans-serif;margin:32px auto;padding:0 24px;max-width:1450px;color:#183044;background:#f7f9fc}h1,h2{color:#163b65}.pass{color:#087c50;font-weight:bold}table{border-collapse:collapse;background:white;width:100%;margin:20px 0;font-size:14px}th,td{border:1px solid #d7e0eb;padding:9px;text-align:left;overflow-wrap:anywhere}th{background:#eaf1f8}a{color:#145ca8}</style><body>' + body + '</body></html>'
(root / 'report.html').write_text(document)
print(root / 'report.html')
