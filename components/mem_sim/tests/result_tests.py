#!/usr/bin/env python3
"""Result contract: terminal view cannot change simulation/tool input data."""
import csv
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from result_io import count, number, parse_stats, read_result, require_valid_run, run_simulator

BINARY = Path(sys.argv.pop(1)).resolve()


class ResultContract(unittest.TestCase):
    def test_reference_fixture_capacity_and_last_address(self):
        # Exercise actual cfg -> resolver -> byte-addressed backend, independently
        # of the pass-through coordinate harness used by the external comparison.
        cases = [('hbm3', 'ramulator2_reference_1ch', 2**30, 32, 32),
                 ('hbm4', 'ramulator2_reference_1ch', 2**30, 32, 32),
                 ('lpddr5', 'ramulator2_reference_1ch', 2**31, 64, 32),
                 ('lpddr6', 'ramulator2_reference_1ch', 2**31, 64, 32),
                 ('hbm3', 'dramsim3_hbm2_common', 2**29, 32, 64)]
        with tempfile.TemporaryDirectory(prefix='hbm_reference_geometry_') as directory:
            trace = Path(directory) / 'boundary.trace'
            for standard, preset, capacity, columns, transaction in cases:
                with self.subTest(standard=standard, preset=preset):
                    common = [str(BINARY), '--config', str(ROOT / 'configs/validation' / f'{standard}.cfg'),
                              '--preset', preset, '--requests', '0', '--trace', str(trace)]
                    last = capacity - transaction
                    first_data, last_data = '12' * transaction, '34' * transaction
                    trace.write_text(f'0 W 0x0 data={first_data}\n1000 W {last:#x} data={last_data}\n'
                                     f'2000 R 0x0 expect={first_data}\n3000 R {last:#x} expect={last_data}\n')
                    stats, _ = run_simulator([*common, '--validate-cmd-trace'], cwd=ROOT, diagnostic=True)
                    self.assertEqual(count(stats, 'capacity_per_instance_bytes'), capacity)
                    self.assertEqual(count(stats, 'columns'), columns)
                    self.assertEqual(count(stats, 'data_mismatches'), 0)
                    self.assertEqual(count(stats, 'data_checked_reads'), 2)
                    self.assertEqual(count(stats, 'completed_reads'), 2)
                    self.assertEqual(count(stats, 'completed_writes'), 2)
                    self.assertEqual(count(stats, 'remaining_requests'), 0)
                    trace.write_text(f'0 R {capacity:#x}\n')
                    invalid = subprocess.run(common, cwd=ROOT, capture_output=True, text=True, timeout=30)
                    self.assertNotEqual(invalid.returncode, 0, 'address at capacity must be rejected')

    def test_hbm_template_sid_and_resolved_replay(self):
        with tempfile.TemporaryDirectory(prefix='hbm_sid_contract_') as directory:
            root = Path(directory)
            config = root / 'copy.cfg'
            config.write_text((ROOT / 'configs/hbm.cfg').read_text().replace(
                'stack_height = 8', 'stack_height = 16'))
            snapshot, first, replay = root / 'resolved.cfg', root / 'first.json', root / 'replay.json'
            for standard, capacity in (('hbm3', 32 * 2**30), ('hbm4', 64 * 2**30)):
                trace = root / 'sid.trace'
                trace.write_text(f'W 0x0 data=12\nW {capacity - 32:#x} data=34\n'
                                 f'R 0x0 expect=12\nR {capacity - 32:#x} expect=34\n')
                common = [str(BINARY), '--standard', standard, '--requests', '0', '--trace', str(trace),
                          '--validate-cmd-trace', '--validate-dfi-trace']
                run = subprocess.run([*common, '--config', str(config), '--stats-json', str(first),
                                      '--dump-resolved-config', str(snapshot)],
                                     cwd=ROOT, capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
                a = json.loads(first.read_text())
                self.assertEqual(a['run_status'], 'completed')
                self.assertEqual(a['model']['sids'], 4)
                self.assertEqual(a['model']['capacity_per_instance_bytes'], capacity)
                self.assertEqual(a['validation']['cmd_validation'], 'pass')
                self.assertEqual(a['validation']['dfi_validation'], 'pass')
                self.assertEqual(a['validation']['data_checked_reads'], 2)
                self.assertEqual(a['validation']['data_mismatches'], 0)
                self.assertRegex(snapshot.read_text(), r'(?m)^sids = 4$')
                run = subprocess.run([*common, '--config', str(snapshot), '--stats-json', str(replay)],
                                     cwd=ROOT, capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
                b = json.loads(replay.read_text())
                for key in ('model', 'metrics', 'validation'):
                    self.assertEqual(a[key], b[key], key)

    def test_profile_index_matches_resolved_model(self):
        with tempfile.TemporaryDirectory(prefix='hbm_index_contract_') as directory:
            resolved = Path(directory) / 'resolved.cfg'
            with (ROOT / 'configs/profile_index.csv').open(newline='') as stream:
                rows = list(csv.DictReader(stream))
            self.assertTrue(rows)
            for row in rows:
                with self.subTest(config=row['config'], preset=row['preset']):
                    arguments = [str(BINARY), '--config', row['config'], '--standard',
                                 row['standard'].lower(), '--check-config',
                                 '--dump-resolved-config', str(resolved)]
                    if row['preset']:
                        arguments += ['--preset', row['preset']]
                    run = subprocess.run(arguments, cwd=ROOT, text=True, capture_output=True, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    text = resolved.read_text()
                    for key in ('speed_bin_mbps', 'density_gb', 'stack_height'):
                        values = re.findall(r'^' + key + r' = (.+)$', text, re.M)
                        self.assertEqual(len(values), 1, (key, values))
                        self.assertAlmostEqual(float(row[key]), float(values[0]), places=8,
                                               msg=f'{key} index must describe the executed model')
                    self.assertIn('reference_model', row)

    def test_default_hbm4_row_reuse_includes_routed_channel(self):
        # Independent address calculation: before 2048 transactions, the XOR
        # routed channel makes every bank/row distinct despite repeated columns.
        def keys(hosts):
            result = []
            for transaction in range(hosts * 2):
                remainder = transaction // 32
                bank, remainder = remainder % 8, remainder // 8
                bg, remainder = remainder % 2, remainder // 2
                pc, remainder = remainder % 2, remainder // 2
                sid, remainder = remainder % 2, remainder // 2
                channel = (transaction ^ (transaction >> 6) ^ (transaction >> 12)) % 32
                result.append((channel, pc, sid, bg, bank, remainder // 32))
            return result
        self.assertEqual(len(set(keys(1024))), 2048)
        self.assertEqual(len({key[1:] for key in keys(1024)}), 64)
        self.assertEqual(len(set(keys(2048))), 2048)
        with tempfile.TemporaryDirectory(prefix='hbm_row_hit_contract_') as directory:
            output = Path(directory) / 'result.json'
            for requests in (1024, 2048):
                run = subprocess.run([str(BINARY), '--config', 'configs/hbm.cfg', '--standard',
                                      'hbm4', '--requests', str(requests), '--stats-json', str(output)],
                                     cwd=ROOT, text=True, capture_output=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
                metrics = json.loads(output.read_text())['metrics']
                self.assertEqual(metrics['dram_transactions'], requests * 2)
                if requests == 1024:
                    self.assertEqual(metrics['row_hit_pct'], 0)
                else:
                    # Geometry predicts 50% address reuse, not the exact row-hit
                    # statistic: refresh and first-scheduling state also matter.
                    self.assertGreater(metrics['row_hit_pct'], 0)
                    self.assertLessEqual(metrics['row_hit_pct'], 50)

    def test_lpddr_default_density_and_neutral_sid(self):
        with tempfile.TemporaryDirectory(prefix='lpddr_default_contract_') as directory:
            output = Path(directory) / 'result.json'
            for standard, capacity in (('lpddr5', 2**31), ('lpddr6', 2**32)):
                run = subprocess.run([str(BINARY), '--config', 'configs/lpddr.cfg', '--standard',
                                      standard, '--requests', '8', '--stats-json', str(output)],
                                     cwd=ROOT, text=True, capture_output=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                model = json.loads(output.read_text())['model']
                self.assertEqual(model['capacity_per_instance_bytes'], capacity)
                self.assertEqual(model['density_gb'], 16)
                self.assertNotIn('sids', model)
                self.assertNotIn('Nominal density_gb differs', run.stdout)

    def test_shared_text_parser(self):
        import model_validation
        import ramulator2_differential
        self.assertIs(model_validation.parse_stats, parse_stats)
        self.assertNotIn('parse_stats', vars(ramulator2_differential))
        self.assertEqual(parse_stats(' # ignored: 9\nx : 1\nx: 1\n'), {'x': '1'})
        with self.assertRaisesRegex(ValueError, 'conflicting metric'):
            model_validation.parse_stats('x: 1\nx: 2\n')

    def test_current_cli_surface(self):
        help_run = subprocess.run([str(BINARY), '--help'], cwd=ROOT,
                                  text=True, capture_output=True, timeout=20)
        self.assertEqual(help_run.returncode, 0, help_run.stderr)
        self.assertIn('summary|diagnostic', help_run.stdout)
        self.assertNotIn('--timing-profile-file', help_run.stdout)
        for arguments in (['--stats-view', 'full'],
                          ['--timing-profile-file', 'unused.cfg']):
            with self.subTest(arguments=arguments):
                run = subprocess.run([str(BINARY), *arguments], cwd=ROOT,
                                     text=True, capture_output=True, timeout=20)
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertIn('error:', run.stderr)
        with tempfile.TemporaryDirectory(prefix='hbm_option_contract_') as directory:
            cfg = Path(directory) / 'invalid.cfg'
            for assignment in ('stats_view=full', 'timing_profile_file=unused.cfg'):
                cfg.write_text('[meta]\nschema_version=3\n[override]\n' + assignment + '\n')
                run = subprocess.run([str(BINARY), '--config', str(cfg), '--requests', '0'],
                                     cwd=ROOT, text=True, capture_output=True, timeout=20)
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertIn('error:', run.stderr)

    def test_lpddr_density_report_with_multiple_channels_and_ranks(self):
        with tempfile.TemporaryDirectory(prefix='lpddr_density_contract_') as directory:
            cfg = Path(directory) / 'case.cfg'
            result = Path(directory) / 'result.json'
            cfg.write_text('[meta]\nschema_version=3\n[override]\nchannels=2\nranks=2\n')
            for standard, expected_gib in (('lpddr5', 8), ('lpddr6', 16)):
                run = subprocess.run([str(BINARY), '--standard', standard, '--config', str(cfg),
                                      '--requests', '8', '--stats-json', str(result)], cwd=ROOT,
                                     text=True, capture_output=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                model = json.loads(result.read_text())['model']
                self.assertEqual(model['density_gb'], 16)
                self.assertEqual(model['capacity_per_instance_bytes'], expected_gib * 2**30)
                self.assertNotIn('Nominal density_gb differs', run.stdout)

    def test_timing_provenance_is_section_local_and_order_independent(self):
        with tempfile.TemporaryDirectory(prefix='hbm_source_contract_') as directory:
            temp = Path(directory)
            cfg, table = temp / 'case.cfg', temp / 'timing.csv'

            def run_table(text, extra=()):
                if '[meta]' in text:
                    text = text.replace('[meta]', '[meta]\nschema_version=3', 1)
                elif '[' in text:
                    text = '[meta]\nschema_version=3\n' + text
                cfg.write_text(text)
                run = subprocess.run([str(BINARY), '--config', str(cfg),
                                      '--standard', 'hbm4', '--requests', '0',
                                      '--dump-timing-table', str(table), *extra],
                                     cwd=ROOT, text=True, capture_output=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                with table.open(newline='') as stream:
                    return {row['name']: row for row in csv.DictReader(stream)}

            for section in ('[dram.timing]', '[override]', ''):
                source_key = 'source' if section == '[dram.timing]' else 'timing_override_source'
                before = run_table(f'{section}\n{source_key}=vendor\nnCL=30\n')
                after = run_table(f'{section}\nnCL=30\n{source_key}=vendor\n')
                self.assertEqual(before['nCL']['source'], 'vendor')
                self.assertEqual(before['nCL'], after['nCL'])

            rows = run_table('[dram.timing]\nnCL=30\nsource=vendor\n'
                             '[override]\nnCWL=10\n')
            self.assertEqual(rows['nCL']['source'], 'vendor')
            self.assertEqual(rows['nCWL']['source'], 'research_default')
            rows = run_table('[dram.timing]\nnCL=30\nsource=vendor\n'
                             '[override]\nnCL=31\n')
            self.assertEqual(rows['nCL']['source'], 'research_default')
            # A child config cannot inherit the parent's vendor claim.
            parent = temp / 'parent.cfg'
            parent.write_text('[dram.timing]\nnCL=30\nsource=vendor\n')
            rows = run_table('[meta]\nextends=parent.cfg\n[override]\nnCL=31\n')
            self.assertEqual(rows['nCL']['source'], 'research_default')

    def test_reject_duplicate_bare_keys_and_inconsistent_inputs(self):
        with tempfile.TemporaryDirectory(prefix='hbm_reject_contract_') as directory:
            cfg = Path(directory) / 'case.cfg'
            for text, error in (
                    ('requests=1\nrequests=2\n[override]\nseed=1\n', 'duplicate key'),
                    ('[meta]\nschema_version=3\n[dram.timing]\nnCL=30\nsource=unknown_source\n',
                     'invalid timing source')):
                cfg.write_text(text)
                run = subprocess.run([str(BINARY), '--config', str(cfg), '--check-config'],
                                     cwd=ROOT, text=True, capture_output=True, timeout=20)
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertIn(error, run.stderr)
            for schema in (1, 2, 3):
                for values in ('nRC=2', 'density_gb=999', 'data_rate_mbps=9000\ntCK_ps=500'):
                    prefix = '' if schema == 1 else f'[meta]\nschema_version={schema}\n[override]\n'
                    cfg.write_text(prefix + values + '\n')
                    run = subprocess.run([str(BINARY), '--config', str(cfg), '--standard', 'hbm4',
                                          '--check-config'], cwd=ROOT, text=True,
                                         capture_output=True, timeout=20)
                    self.assertNotEqual(run.returncode, 0, run.stdout)
                    self.assertNotIn('sectioned config documents require', run.stderr)

    def test_english_compact_report_across_standards(self):
        for standard in ('hbm3', 'hbm4', 'lpddr5', 'lpddr6'):
            family = 'lpddr' if standard.startswith('lpddr') else 'hbm'
            for view in ([], ['--stats-view', 'summary']):
                with self.subTest(standard=standard, view=view):
                    run = subprocess.run(
                        [str(BINARY), '--config', f'configs/{family}.cfg',
                         '--standard', standard, '--requests', '8', '--stack-count', '2', *view],
                        cwd=ROOT, capture_output=True, text=True, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertNotIn('Nominal density_gb differs', run.stdout)
                    self.assertTrue(run.stdout.isascii(), run.stdout)
                    self.assertLessEqual(len(run.stdout.splitlines()), 38)
                    for label in ('Model / Standard', 'Organization', 'Capacity',
                                  'Run Status', 'Simulation Time', 'Bandwidth / Utilization',
                                  'Read Transaction Latency', 'Validation', 'Completed_Reads'):
                        self.assertIn(label, run.stdout)
                    with tempfile.TemporaryDirectory(prefix='hbm_english_report_') as temp:
                        report = Path(temp) / 'summary.txt'
                        report.write_text(run.stdout)
                        with self.assertRaisesRegex(ValueError, 'stats-json'):
                            read_result(report)

    def test_tool_helpers_do_not_collect_diagnostics_by_default(self):
        command = [str(BINARY), '--standard', 'hbm4', '--requests', '8']
        public, _ = run_simulator(command, cwd=ROOT, timeout=20)
        self.assertIn('row_hit_pct', public)
        self.assertNotIn('row_hits', public)
        diagnostic, _ = run_simulator(command, cwd=ROOT, timeout=20, diagnostic=True)
        self.assertIn('row_hits', diagnostic)
        for key in ('completed_reads', 'system_cycles', 'avg_read_latency_ns'):
            self.assertEqual(public[key], diagnostic[key])
        disabled, _ = run_simulator(command + ['--power-model', 'false',
                                               '--thermal-model', 'false'], cwd=ROOT, timeout=20)
        self.assertNotIn('power_energy_pJ', disabled)
        self.assertNotIn('thermal_peak_temp_C', disabled)

    def test_direct_standard_edits_and_derived_capacity_are_visible(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_changes_") as temp:
            temp = Path(temp)
            content = (ROOT / "configs/hbm.cfg").read_text()
            start = content.index('[standard.hbm3.organization]')
            end = content.index('\n[', start + 1)
            section = content[start:end]
            self.assertIn('columns = 32', section)
            edited = section.replace('columns = 32', 'columns = 16')
            cfg = temp / 'copied.cfg'
            cfg.write_text(content[:start] + edited + content[end:])
            result = temp / 'result.json'
            run = subprocess.run([str(BINARY), '--config', str(cfg), '--standard', 'hbm3',
                                  '--requests', '8', '--read-ratio', '0',
                                  '--stats-json', str(result)], cwd=ROOT,
                                 capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)
            raw = json.loads(result.read_text())
            changes = {c['key']: c for c in raw['changes']}
            self.assertEqual(changes['architecture.columns']['baseline'], '32')
            self.assertEqual(changes['architecture.columns']['value'], '16')
            self.assertIn('architecture.density_gb', changes)
            model = raw['model']
            capacity = 1
            for key in ('channels', 'pseudo_channels', 'sids', 'bank_groups',
                        'banks_per_group', 'rows', 'columns', 'dram_transaction_bytes'):
                capacity *= model[key]
            self.assertEqual(model['capacity_per_instance_bytes'], capacity)
            self.assertEqual(model['aggregate_capacity_bytes'], capacity * model['stack_count'])
            self.assertIsNone(raw['metrics']['avg_read_latency_ns'])
            self.assertIn('N/A ns', run.stdout)
            self.assertIn('built-in HBM3', raw['comparison_baseline'])

    def test_diagnostic_opt_in_and_public_units(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_units_") as temp:
            result = Path(temp) / 'result.json'
            for standard in ('hbm3', 'hbm4', 'lpddr5', 'lpddr6'):
                command = [str(BINARY), '--standard', standard, '--requests', '16',
                           '--stack-count', '2', '--stats-view', 'diagnostic',
                           '--stats-json', str(result)]
                run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                raw = json.loads(result.read_text())
                d, m = raw['diagnostics'], raw['metrics']
                ns = raw['parameters']['tick_duration_ps'] / 1000
                self.assertAlmostEqual(m['simulation_time_ns'], d['system_cycles'] * ns)
                self.assertAlmostEqual(m['avg_read_latency_ns'], d['avg_read_latency'] * ns)
                denominator = d['row_hits'] + d['row_misses'] + d['row_conflicts']
                if denominator:
                    self.assertAlmostEqual(m['row_hit_pct'], 100 * d['row_hits'] / denominator)
                self.assertEqual(len(raw['stacks']), 2)
                self.assertEqual(sum(s['completed_reads'] for s in raw['stacks']), m['completed_reads'])
                self.assertNotIn('read_bytes_per_cycle', d)
                self.assertNotIn('read_queue_len_avg', d)
                self.assertNotIn('timing_profile_file', d)
                if standard.startswith('lpddr'):
                    self.assertNotIn('sids', raw['model'])
                    self.assertIn('lpddr_wck_ratio', raw['parameters'])
                else:
                    self.assertNotIn('ranks', raw['model'])
                    self.assertNotIn('lpddr_wck_ratio', raw['parameters'])

    def test_old_results_and_compact_input_error(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_legacy_") as temp:
            path = Path(temp) / 'stats.txt'
            path.write_text('completed_reads: 9007199254740993\n')
            self.assertEqual(count(read_result(path), 'completed_reads'), 9007199254740993)
            path.write_text(json.dumps({'schema_version': 1, 'run_status': 'completed',
                                       'metrics': {'completed_reads': 9007199254740993}}))
            self.assertEqual(count(read_result(path), 'completed_reads'), 9007199254740993)
            for marker in ('# ===== 模型 / MODEL =====', '# ===== MODEL ====='):
                path.write_text(marker + '\n')
                with self.assertRaisesRegex(ValueError, 'stats-json'):
                    read_result(path)

    def test_visualization_example_keeps_complete_results(self):
        with tempfile.TemporaryDirectory(prefix="hbm_visual_example_") as temp:
            env = dict(os.environ, HBM_SIM_BIN=str(BINARY), HBM_SIM_SOURCE_DIR=str(ROOT),
                       HBM_SIM_VIS_OUT=temp)
            run = subprocess.run(["bash", str(ROOT / "tools/visualize_example.sh")],
                                 cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = read_result(Path(temp) / "result.json", require_completed=True)
            self.assertEqual(result["cmd_validation"], "pass")
            self.assertEqual(result["dfi_validation"], "pass")
            self.assertIn("tick_multiplier", result)
            self.assertIn("# ===== MODEL =====", (Path(temp) / "stats.txt").read_text())
            self.assertTrue((Path(temp) / "dashboard.html").is_file())

    def test_single_stack_output_marker(self):
        with tempfile.TemporaryDirectory(prefix="hbm_single_marker_") as temp:
            temp = Path(temp)
            run = subprocess.run([str(BINARY), "--config", "configs/hbm.cfg", "--requests", "2",
                                  "--dump-thermal-map", str(temp / "thermal_{stack}.txt"),
                                  "--dump-memory-image", str(temp / "final_{stack}.txt")],
                                 cwd=ROOT, capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertTrue((temp / "thermal_0.txt").is_file())
            self.assertTrue((temp / "final_0.txt").is_file())
            self.assertFalse((temp / "thermal_{stack}.txt").exists())

    def test_small_geometry_payload_backend_phy_matrix(self):
        with tempfile.TemporaryDirectory(prefix="hbm_geometry_matrix_") as temp:
            temp = Path(temp)
            for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
                family = "lpddr" if standard.startswith("lpddr") else "hbm"
                config = temp / f"{standard}.cfg"
                config.write_text((ROOT / "configs" / f"{family}.cfg").read_text() +
                    "\nchannels=1\nsids=1\nbank_groups=2\nbanks_per_group=2\n"
                    "rows=16\ncolumns=16\nsupports_refresh=false\nsupports_rfm=false\n")
                golden = None
                for backend in ("sparse", "mmap_sparse", "chunk_file"):
                    for phy in ("direct", "behavioral"):
                        case = temp / f"{standard}_{backend}_{phy}"
                        case.mkdir()
                        final, result = case / "final.txt", case / "result.json"
                        cmd = [str(BINARY), "--config", str(config), "--standard", standard,
                               "--requests", "0", "--trace", "examples/data_check.trace",
                               "--mem-phy", phy, "--memory-backend", backend,
                               "--memory-data-file", str(case / "data.bin"),
                               "--memory-chunk-size", "4096", "--memory-chunk-cache-entries", "2",
                               "--dump-memory-image", str(final), "--stats-json", str(result),
                               "--validate-cmd-trace", "--validate-dfi-trace", "--max-cycles", "10000"]
                        run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=20)
                        self.assertEqual(run.returncode, 0, (standard, backend, phy, run.stderr))
                        metrics = read_result(result, require_completed=True)
                        self.assertGreater(count(metrics, "data_checked_reads"), 0)
                        self.assertEqual(metrics["cmd_validation"], "pass")
                        self.assertEqual(metrics["dfi_validation"], "pass")
                        # PHY modes deliberately differ in commit latency. Keep
                        # all payload/init/ECC checks but exclude that timestamp.
                        image = re.sub(r"last_write_cycle=\d+", "last_write_cycle=<mode-dependent>",
                                       final.read_text())
                        if golden is None:
                            golden = image
                        self.assertEqual(image, golden, (standard, backend, phy))

    def test_schema3_geometry_and_resolved_replay(self):
        with tempfile.TemporaryDirectory(prefix="hbm_config_replay_") as temp:
            temp = Path(temp)
            for standard in ("hbm3", "hbm4", "lpddr5", "lpddr6"):
                family = "lpddr" if standard.startswith("lpddr") else "hbm"
                # A copied standalone master is the supported user workflow.
                content = (ROOT / "configs" / f"{family}.cfg").read_text()
                content += "\ncolumns = 16  # smaller research geometry\n"
                config = temp / f"{standard}.cfg"
                config.write_text(content)
                snapshot = temp / f"{standard}_resolved.cfg"
                result = temp / "result.json"
                command = [str(BINARY), "--config", str(config), "--standard", standard,
                           "--requests", "8", "--stats-json", str(result),
                           "--dump-resolved-config", str(snapshot),
                           "--validate-cmd-trace", "--validate-dfi-trace"]
                run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                first = read_result(result, require_completed=True)
                require_valid_run(first)
                self.assertEqual(count(first, "columns"), 16)
                replay = subprocess.run([str(BINARY), "--config", str(snapshot)], cwd=ROOT,
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(replay.returncode, 0, replay.stderr)
                second = read_result(result, require_completed=True)
                for key in ("aggregate_capacity_bytes", "density_gb", "tCK_ps", "system_cycles",
                            "completed_reads", "completed_writes", "cmd_validation", "dfi_validation"):
                    self.assertEqual(first[key], second[key], (standard, key))

    def test_golden_metrics_are_in_machine_result(self):
        with tempfile.TemporaryDirectory(prefix="hbm_golden_result_") as temp:
            image, result = Path(temp) / "memory.txt", Path(temp) / "result.json"
            setup = subprocess.run([str(BINARY), "--standard", "hbm4", "--requests", "0",
                                    "--trace", "examples/data_check.trace",
                                    "--dump-memory-image", str(image)], cwd=ROOT,
                                   capture_output=True, text=True, timeout=20)
            self.assertEqual(setup.returncode, 0, setup.stderr)
            for preload in (True, False):
                cmd = [str(BINARY), "--standard", "hbm4", "--requests", "0",
                       "--verify-golden", str(image), "--stats-json", str(result)]
                if preload:
                    cmd += ["--memory-image", str(image)]
                run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=20)
                fields = read_result(result)
                self.assertEqual(run.returncode == 0, preload)
                self.assertEqual(count(fields, "golden_mismatches") == 0, preload)
                self.assertEqual(fields["run_status"], "completed" if preload else "failed")

    def test_views_and_failed_rerun(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_test_") as temp:
            result = Path(temp) / "result.json"
            base = [str(BINARY), "--config", "configs/hbm.cfg", "--requests", "16",
                    "--stats-json", str(result)]
            reports = []
            for view in ([], ["--stats-view", "summary"]):
                run = subprocess.run([*base, *view], cwd=ROOT,
                                     capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)
                report = read_result(result, require_completed=True)
                require_valid_run(report)
                self.assertIn("row_hit_pct", report)
                self.assertNotIn("row_hits", report)
                raw = json.loads(result.read_text())
                self.assertEqual(raw["schema_version"], 2)
                self.assertEqual(raw["validation"]["dfi_validation"], "not_run")
                self.assertNotIn("diagnostics", raw)
                self.assertLessEqual(len(raw["metrics"]), 24)
                self.assertLessEqual(len(run.stdout.splitlines()), 34)
                for section in ("# ===== MODEL =====", "# ===== PARAMETERS =====", "# ===== RESULTS ====="):
                    self.assertIn(section, run.stdout)
                self.assertNotIn("row_hits", run.stdout)
                reports.append(report)
            self.assertEqual(*reports)
            truncated = subprocess.run([*base, "--max-cycles", "1"], cwd=ROOT,
                                       capture_output=True, timeout=20)
            self.assertEqual(truncated.returncode, 2)
            self.assertEqual(read_result(result)["run_status"], "truncated")
            with self.assertRaises(ValueError):
                read_result(result, require_completed=True)
            failed = subprocess.run([*base, "--read-ratio", "101"], cwd=ROOT,
                                    capture_output=True, timeout=20)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(read_result(result)["run_status"], "failed")

    def test_metrics_fail_closed(self):
        for value in ("nan", "inf", "not-a-number"):
            with self.assertRaises(ValueError):
                number({"metric": value}, "metric")
        with self.assertRaises(ValueError):
            number({}, "metric")
        for value in ("", "-1", "1.5", "false"):
            with self.assertRaises(ValueError):
                count({"counter": value}, "counter")
        with self.assertRaises(ValueError):
            require_valid_run({"hit_cycle_limit": "false", "system_cycles": "10"})

    def test_no_input_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="hbm_result_input_") as temp:
            config = Path(temp) / "model.cfg"
            config.write_text("standard = hbm4\nrequests = 1\n")
            original = config.read_bytes()
            run = subprocess.run([str(BINARY), "--config", str(config),
                                  "--stats-json", str(config)], cwd=ROOT,
                                 capture_output=True, timeout=20)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(config.read_bytes(), original)
            data = Path(temp) / "data_0.bin"
            data.write_bytes(b"existing storage must not become result JSON")
            original_data = data.read_bytes()
            run = subprocess.run([str(BINARY), "--standard", "hbm4", "--stack-count", "2",
                                  "--memory-backend", "mmap_sparse", "--memory-data-file",
                                  str(Path(temp) / "data_{stack}.bin"),
                                  "--stats-json", str(data)], cwd=ROOT,
                                 capture_output=True, timeout=20)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(data.read_bytes(), original_data)


if __name__ == "__main__":
    unittest.main()
