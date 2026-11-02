"""Layout instances do not affect each other and route conflicts are rejected."""
import unittest
from result_core import Layout

class LayoutTest(unittest.TestCase):
    def test_conflicting_routes_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Conflicting result route'):
            Layout.merged({'config.ini': 'config/config.ini'},
                          {'config.ini': 'traces/vortex/config.ini'})

    def test_instance_routes_are_isolated(self):
        first = Layout({'requests.csv': 'traces/first/requests.csv'})
        second = Layout({'requests.csv': 'traces/second/requests.csv'})
        self.assertEqual(str(first.location('requests.csv')), 'traces/first/requests.csv')
        self.assertEqual(str(second.location('requests.csv')), 'traces/second/requests.csv')
