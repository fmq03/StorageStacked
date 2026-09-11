"""Verify the actual CTest plan and completed native test count."""
import json

def passed_count(root):
    tests = json.loads((root / 'native-test-plan.json').read_text())['tests']
    names = {t['name'] for t in tests}
    required = {'sequence_tests', 'phy_tests', 'config_tests', 'timing_boundary_tests',
                'scheduler_contract_tests', 'model_config_tests', 'result_contract'}
    assert required <= names, required - names
    assert len(names) == len(tests)
    count = len(tests)
    assert f'100% tests passed, 0 tests failed out of {count}' in (root / 'native-tests.log').read_text()
    return count
