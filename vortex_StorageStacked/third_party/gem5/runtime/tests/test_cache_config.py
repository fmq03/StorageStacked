"""Check legacy defaults, derived latencies and invalid cache geometry."""
import copy
import unittest
from configure import load_config,validate,resolved_parts,build_inputs,simx_flags
from paths import ROOT

class CacheConfigTest(unittest.TestCase):
    def setUp(self):self.c=load_config(ROOT/'user/smoke/config.json')
    def test_defaults_and_native_capacity_scaling(self):
        c=copy.deepcopy(self.c)
        for group in ('gpu','host'):
            for key in list(c[group]):
                if key.endswith(('_ways','_latency_cycles')):del c[group][key]
        a,_=resolved_parts(validate(c))
        self.assertEqual([a['gpu'][x+'_latency_cycles'] for x in ('l1i','l1d','l2')],[2,2,4])
        c['gpu']['l1i_bytes']*=4;c['gpu']['l1d_latency_cycles']=9
        a,_=resolved_parts(validate(c))
        self.assertEqual(a['gpu']['l1i_latency_cycles'],4)
        self.assertEqual(a['gpu']['l1d_latency_cycles'],9)
        self.assertIn('-DVX_CFG_DCACHE_LATENCY=9',simx_flags(c))
    def test_geometry_is_validated_and_gpu_changes_invalidate_build(self):
        before=build_inputs(self.c)
        for group in ('host','gpu'):
            for key,value in (('l1i_ways',3),('l1d_ways',True),('l2_latency_cycles',0),('l1i_latency_cycles',False)):
                c=copy.deepcopy(self.c);c[group][key]=value
                with self.subTest(group=group,key=key),self.assertRaises(ValueError):validate(c)
        c=copy.deepcopy(self.c);c['host'].update(l1i='1KiB',l1i_ways=32)
        with self.assertRaises(ValueError):validate(c)
        c=copy.deepcopy(self.c);c['gpu'].update(threads=32,l1d_bytes=4096,l1d_ways=64)
        with self.assertRaises(ValueError):validate(c)
        c=copy.deepcopy(self.c);c['gpu']['l1i_ways']=2
        self.assertNotEqual(before,build_inputs(c))
        c=copy.deepcopy(self.c);c['host']['l1i_ways']=2
        self.assertEqual(before,build_inputs(c))
