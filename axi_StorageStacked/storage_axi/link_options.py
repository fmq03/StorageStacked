"""Optional per-build UCIe parameters for gem5's SCons integration."""
import json
import math
import os


def definitions():
    path = os.environ.get('STORAGE_LINK_CONFIG')
    if not path:
        return []
    with open(path) as source:
        config = json.load(source)
    fields = {'lanes': (1, 256), 'rate_gtps': (0, 1000),
              'bits_per_symbol': (1, 2), 'tat_ns': (0, 1000000)}
    if set(config) != set(fields):
        raise ValueError('UCIe configuration must contain ' + ', '.join(fields))
    for key, (low, high) in fields.items():
        value = config[key]
        if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError('Invalid UCIe ' + key)
        if key in ('lanes', 'bits_per_symbol') and type(value) is not int:
            raise ValueError(key + ' must be an integer')
    if config['rate_gtps'] == 0:
        raise ValueError('rate_gtps must be positive')
    return [('AOU_LINK_' + key.upper(), str(value)) for key, value in config.items()]
