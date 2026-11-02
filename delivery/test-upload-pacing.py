"""Check API-window bounds and interrupted-upload resume without network calls."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('upload_release', Path(__file__).with_name('upload-release.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Clock:
    def __init__(self):
        self.now = 1000.0

    def time(self):
        return self.now

    def wait(self, seconds):
        self.now += seconds


class PacingTests(unittest.TestCase):
    def setUp(self):
        quiet = patch('builtins.print')
        quiet.start()
        self.addCleanup(quiet.stop)

    def test_rolling_window_and_minimum_spacing(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = Clock()
            pacer = module.UploadPacer(Path(directory)/'requests.json', 3, clock.time, clock.wait)
            starts = []
            for _ in range(10):
                pacer.reserve()
                starts.append(clock.now)
            for instant in starts:
                self.assertLessEqual(sum(instant-pacer.window < t <= instant for t in starts), 3)
            self.assertTrue(all(b-a >= 1.1-1e-6 for a,b in zip(starts,starts[1:])))

    def test_resume_retains_the_existing_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = Clock(); journal = Path(directory)/'requests.json'
            first = module.UploadPacer(journal, 2, clock.time, clock.wait)
            first.reserve(); first.reserve()
            previous = json.loads(journal.read_text())
            resumed = module.UploadPacer(journal, 2, clock.time, clock.wait)
            resumed.reserve()
            self.assertGreaterEqual(clock.now-previous[0], first.window)

    def test_cancelled_wait_does_not_consume_a_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            pacer = module.UploadPacer(Path(directory)/'requests.json')
            pacer.stopped.set()
            with self.assertRaisesRegex(RuntimeError, 'cancelled'):
                pacer.reserve()
            self.assertFalse(pacer.journal.exists())


if __name__ == '__main__':
    unittest.main()
