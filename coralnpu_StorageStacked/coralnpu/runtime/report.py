"""One concise human report, derived only from the final acceptance summary."""
import json


def write_report(root):
    s = json.loads((root / 'summary.json').read_text())
    lines = ['# CoralNPU 运行报告', '']
    if not s.get('passed'):
        lines += ['**未通过验收**', '', f"阶段：{s.get('stage', 'unknown')}。"]
        if s.get('error'):
            lines += ['', '```text', str(s['error']), '```']
    elif 'cases' in s:
        lines += [f"**PASS** — {len(s['cases'])} 个设备场景、{s['native_tests_passed']} 项原生内存测试、在线 C ABI 和错误拒绝检查通过。", '',
                  '| 场景 | 输出 | NPU 周期 | 外部事务 | 报告 |', '|---|---|---:|---:|---|']
        for name, case in s['cases'].items():
            lines.append(f"| {name} | {case['output']} | {case['cycles']} | {case['transactions']} | [查看]({name}/report.md) |")
        lines += ['', '每个项目的默认场景运行一次；各目录保存实际配置与原始证据。']
    else:
        b = s['benchmark']
        c = json.loads((root / 'resolved.json').read_text())
        a = c['architecture']
        source = s['sources'][s['device']]
        lines += [f"**PASS** — {b['name'].upper()} 计算、AXI 五通道、Flit 与在线内存验收通过。", '',
                  f"配置：NPU {a['device_clock_mhz']} MHz；AXI {a['axi']['period_ns']} ns；"
                  f"{a['memory']['standard'].upper()} × {a['memory']['channels']} 通道，时序倍率 {a['memory']['scale']}。", '',
                  f"NPU 周期：{s['devices']['cycles']}；外部事务：{source['transactions']}；"
                  f"平均往返：{source['roundtrip_mean_ns']:.3f} ns。", '']
        if 'npu_model' in s:
            model=s['npu_model']
            lines += [f"实际 RTL：ITCM {model['itcm_kib']} KiB，DTCM {model['dtcm_kib']} KiB；L0={model['fetch_l0_enabled']}；RVV/FP32 启用。",
                      f"DTCM 基址 {model['dtcm_base']:#x}；CSR 基址 {model['csr_base']:#x}。构建参数见 build/model_parameters.h，加载模型见 npu_model.json。",'']
        if 'axi_burst' in s:
            burst=s['axi_burst']
            lines += [f"AXI INCR burst：每请求 {burst['beats_per_request']} 拍，每拍 {burst['bytes_per_beat']} 字节；"
                      f"写 {burst['write_bursts']} 笔 / {burst['write_beats']} 拍，读 {burst['read_bursts']} 笔 / {burst['read_beats']} 拍。", '']
        if b['name'] == 'smoke':
            r = s['smoke']
            lines += [f"输入 **{r['input']}** → 加一 → 输出 **{r['output']}**；独立期望 **{r['expected_output']}**（uint32 回绕）。", '',
                      '| 操作 | 地址 | 值 | 请求 ns | 响应 ns | 往返 ns |', '|---|---|---:|---:|---:|---:|']
            for t in r['timeline']:
                ns = t['time_ns']
                lines.append(f"| {t['command']} | {t['address']} | {t['value']} | {ns['npu_request']:.3f} | {ns['npu_response']:.3f} | {t['roundtrip_ns']:.3f} |")
            lines += ['', 'W=写，R=读；每笔有效数据 4 字节，原生传输 16 字节，AXI 总线 256 bit。',
                      '详细链路时刻见 [smoke_summary.json](smoke_summary.json)。']
        elif b['name'] == 'tiny_llm':
            r = s['llm']; t = r['timing']
            lines += [f"输入：`{json.dumps(r['prompt'], ensure_ascii=False)}` → 生成：`{json.dumps(r['generated_text'], ensure_ascii=False)}`；token ID：{r['generated_token_ids']}。", '',
                      f"模型 {r['model_version']}，{r['model_parameters']} 参数；KV cache={r['kv_cache']}，前向位置计算 {r['forward_calls']} 次。", '',
                      '| 阶段 | 仿真时间 ns |', '|---|---:|',
                      f"| 初始化 | {t['initialization_ns']:.3f} |",
                      f"| Prefill 至首 token | {t['prefill_to_first_token_ns']:.3f} |",
                      f"| 推理至最后一个 token | {t['inference_to_last_token_ns']:.3f} |", '',
                      f"RVV FP32 线性层执行 {r['execution']['rvv_linear_calls']} 次；归一化与注意力使用标量 FP32。",
                      '时间包含观测记录开销；prefill/decode 不含初始化。此字符模型用于功能验证。',
                      '数值误差、KV、访存分布与逐 token 时间见 [llm_summary.json](llm_summary.json)。']
        else:
            lines += ['输出字校验通过：`'+json.dumps(b['expect'], ensure_ascii=False)+'`。']
        lines += ['', '所有时间均为仿真时间。配置见 [resolved.json](resolved.json)；逐请求链路见 [memsim_view.html](memsim_view.html)。',
                  '原始证据：npu_requests.csv、axi_wave.vcd、ucie_flits.csv、memsim_bridge.csv。']
    lines += ['', '最终状态与检查详情：[summary.json](summary.json)。', '']
    (root / 'report.md').write_text('\n'.join(lines))


def write_status(root, stage, error=None):
    summary = {'passed': False, 'stage': stage}
    if error is not None:
        summary['error'] = str(error)
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    write_report(root)
