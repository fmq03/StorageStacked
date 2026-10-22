"""Validate the supported model presets and their coupled address maps."""
import copy
import unittest
import tempfile
import os
from pathlib import Path
from configure import load_config,validate,memory_layout
from paths import ROOT
from rtl_build import variant_name,model_name,copy_artifact

class RtlConfigTest(unittest.TestCase):
    def test_legacy_and_all_variants(self):
        c=load_config(ROOT/'user/smoke/config.json')
        legacy=copy.deepcopy(c);legacy['npu']={'clock_mhz':500}
        self.assertEqual(validate(legacy),c)
        names=set()
        for itcm,dtcm in ((8,32),(512,512),(1024,1024)):
            for l0 in (False,True):
                x=copy.deepcopy(c);x['npu'].update(itcm_kib=itcm,dtcm_kib=dtcm,fetch_l0_enabled=l0)
                validate(x);names.add(variant_name(x));layout=memory_layout(x)
                self.assertIn('L0On' if l0 else 'L0Off',model_name(x))
                self.assertEqual(layout['dtcm_base'],0x10000 if itcm==8 else 0x100000)
                self.assertEqual(layout['csr_base'],0x30000 if itcm==8 else 0x200000)
        self.assertEqual(len(names),6)
    def test_reject_unimplemented_combinations(self):
        c=load_config(ROOT/'user/smoke/config.json')
        for fields in ({'itcm_kib':64},{'dtcm_kib':512},{'fetch_l0_enabled':1},{'fetch_l0_enabled':'true'},{'itcm_kib':True}):
            x=copy.deepcopy(c);x['npu'].update(fields)
            with self.subTest(fields=fields),self.assertRaises(ValueError):validate(x)

    def test_read_only_artifact_replacement_preserves_linked_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'built.so';destination=root/'cached.so';snapshot=root/'offline.so'
            source.write_bytes(b'new native model');source.chmod(0o444)
            destination.write_bytes(b'old native model');destination.chmod(0o444)
            os.link(destination,snapshot)
            copy_artifact(source,destination)
            self.assertEqual(destination.read_bytes(),b'new native model')
            self.assertEqual(snapshot.read_bytes(),b'old native model')
