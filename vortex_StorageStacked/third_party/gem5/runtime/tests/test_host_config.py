"""Host ISA validation and cache separation across compiled UCIe variants."""
import copy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build as platform
from configure import build_inputs, load_config, validate
from paths import ROOT, dump


class HostConfigTest(unittest.TestCase):
    def setUp(self):
        self.config = load_config(ROOT / 'user/smoke/config.json')

    def test_legacy_and_supported_isas(self):
        legacy = copy.deepcopy(self.config)
        del legacy['host']['isa']
        self.assertEqual(validate(legacy)['host']['isa'], 'x86_64')
        for isa in ('x86_64', 'aarch64'):
            with self.subTest(isa=isa):
                config = copy.deepcopy(self.config)
                config['host']['isa'] = isa
                self.assertEqual(validate(config)['host']['isa'], isa)

    def test_reject_unsupported_isas(self):
        for isa in ('armhf', 'riscv64', 'arm', '', None, True, 123, [], {}):
            with self.subTest(isa=isa):
                config = copy.deepcopy(self.config)
                config['host']['isa'] = isa
                with self.assertRaises(ValueError):
                    validate(config)

    def test_cached_platforms_require_their_own_ucie_and_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            config_path = base / 'config.json'
            config = copy.deepcopy(self.config)
            old = {'device': build_inputs(config), 'ucie': config['ucie']}
            config['ucie'] = {**config['ucie'], 'lanes': config['ucie']['lanes'] + 1}
            selected = {'device': build_inputs(config), 'ucie': config['ucie']}
            dump(base / 'platform.json', selected)
            device = base / 'vortex/sim/simx/libvortex-gem5.so'
            device.parent.mkdir(parents=True)
            device.touch()
            for isa in ('x86_64', 'aarch64'):
                runtime = base / isa
                runtime.mkdir()
                for name in ('gem5.opt', 'libvortex.so', f'libvortex-gem5-{isa}.so'):
                    (runtime / name).touch()
                variant = selected if isa == 'aarch64' else old
                dump(base / f'host-{isa}.json', {'isa': isa, **variant})
            with patch.multiple(platform, BUILD=base,
                                gem5_binary=lambda isa: base / isa / 'gem5.opt',
                                host_runtime=lambda isa: base / isa):
                def ready(isa):
                    config['host']['isa'] = isa
                    dump(config_path, config)
                    return platform.build(config_path, ready=True)

                self.assertTrue(ready('aarch64'))
                self.assertFalse(ready('x86_64'))
                dump(base / 'host-x86_64.json', {'isa': 'x86_64', **selected})
                self.assertTrue(ready('x86_64'))
                (base / 'aarch64/libvortex.so').unlink()
                self.assertFalse(ready('aarch64'))
                self.assertTrue(ready('x86_64'))


if __name__ == '__main__':
    unittest.main()
