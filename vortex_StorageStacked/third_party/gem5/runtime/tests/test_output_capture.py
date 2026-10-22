"""A diagnostic must not corrupt the exact returned-byte evidence."""
import tempfile
import unittest
from pathlib import Path

from paths import dump
from session import capture
from verify_compute import readback


class OutputCaptureTest(unittest.TestCase):
    def test_complete_long_readback_survives_simulator_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            data = bytes(range(256)) * 16
            base = 0x90000000
            line = f'READBACK {base:08x} {data.hex()}\n'
            warning = 'src/sim/syscall_emul.cc: warn: ignoring syscall madvise(...)\n'
            dump(output / 'build/readback.json',
                 {'base': base, 'ranges': [{'offset': 0, 'bytes': len(data)}]})
            (output / 'memsim_image.csv').write_text(
                f'address,data\n{base},{data.hex()}\n')
            (output / 'run.log').write_text(line[:6000] + warning + line[6000:])
            with self.assertRaises(AssertionError):
                readback(output)
            (output / 'run.log').write_text(warning)
            (output / 'host_stdout.log').write_text(line)
            (output / 'host_stderr.log').write_text('')
            capture(output)
            self.assertEqual(readback(output), dict(enumerate(data, base)))
            self.assertEqual((output / 'host_stdout.log').read_text(), line)


if __name__ == '__main__':
    unittest.main()
