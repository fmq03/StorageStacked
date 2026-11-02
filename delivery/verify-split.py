"""Extract the real split delivery and compare every delivered file byte for byte."""
import argparse
import datetime
import filecmp
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    directory = options.directory.resolve()
    package = json.loads((directory/'PACKAGE.json').read_text())
    bundle = package['delivery_directory']
    source = directory.parent/bundle
    subprocess.run(['bash', str(directory/'extract.sh'), str(options.output.resolve())], check=True)
    restored = options.output.resolve()/bundle
    compared = []
    for item in package['files']:
        name = item['name']
        original, copy = source/name, restored/name
        if not copy.is_file() or copy.stat().st_size != item['bytes']:
            raise RuntimeError('恢复后的文件缺失或大小不符：'+name)
        if not filecmp.cmp(original, copy, shallow=False):
            raise RuntimeError('恢复后的文件内容不符：'+name)
        compared.append(item)
    delivery = json.loads((restored/'DELIVERY.json').read_text())
    if delivery.get('passed') is not True:
        raise RuntimeError('原归档尚未通过实际导入与默认验收')
    report = {
        'passed': True,
        'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'image': package['image'],
        'parts': package['parts'],
        'total_bytes': package['total_bytes'],
        'actual_extraction': True,
        'files_compared_byte_for_byte': compared,
        'archive_import_and_default_run': 'matching exported archive; see DELIVERY.json and ACCEPTANCE.json',
        'restored_package': str(restored),
        'customer_host_acceptance': 'not performed on customer host',
    }
    (directory/'SPLIT_ACCEPTANCE.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print('PASS：真实分块已解包，全部交付文件与已验收归档逐字节一致。')


if __name__ == '__main__':
    main()
