"""Saved evidence keeps its bytes and working links across layout transitions."""
import json
from pathlib import Path
import tempfile
import unittest
from result_layout import artifact, organize, unpack, working_result


class ResultLayoutTest(unittest.TestCase):
    def test_roundtrip_and_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = {'input.json': '{"npu":{}}', 'summary.json': '{"passed":true}',
                     'axi_wave.vcd': '$timescale 1fs $end\n',
                     'npu_requests.csv': 'sequence,event\n1,request\n',
                     'run.log': 'original device log\n',
                     'memsim_check.json': '{"passed":true}',
                     'report.md': '[config](input.json) [view](memsim_view.html)\n',
                     'memsim_view.html': '<a href="axi_wave.vcd">wave</a><a href="memsim_check.json">check</a><script src="memsim_data/00000.js"></script>',
                     'axi_view.gtkw': '[dumpfile] "axi_wave.vcd"',
                     'memsim_data/00000.js': 'window.data=[];',
                     'build/program.elf': 'test ELF'}
            for name, text in files.items():
                path = root / name;path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            organize(root)
            self.assertEqual((root/'traces/axi_wave.vcd').read_text(), files['axi_wave.vcd'])
            self.assertEqual((root/'logs/run.log').read_text(), files['run.log'])
            self.assertIn('(config/input.json)', (root/'report.md').read_text())
            self.assertIn('href="../traces/axi_wave.vcd"', (root/'views/memsim_view.html').read_text())
            self.assertIn('href="../checks/memsim_check.json"', (root/'views/memsim_view.html').read_text())
            self.assertTrue((root/'views/memsim_data/00000.js').is_file())
            self.assertEqual(artifact(root,'input.json'), root/'config/input.json')
            organize(root)  # Publication is idempotent.
            unpack(root)
            for name, text in files.items():
                self.assertEqual((root/name).read_text(), text, name)

    def test_failed_revalidation_restores_layout_and_clears_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'summary.json').write_text('{"passed":true}')
            (root/'input.json').write_text('{}')
            organize(root)
            with self.assertRaises(ValueError):
                with working_result(root):
                    (root/'summary.json').write_text('{"passed":false}')
                    (root/'validation.log').write_text('failed')
                    raise ValueError('negative control')
            self.assertFalse(json.loads((root/'summary.json').read_text())['passed'])
            self.assertTrue((root/'config/input.json').is_file())
            self.assertTrue((root/'logs/validation.log').is_file())
