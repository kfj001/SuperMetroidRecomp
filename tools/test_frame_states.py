#!/usr/bin/env python3
"""Real-ROM current-build state, continuation and rejection regression gate."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import tempfile
import zlib

from sm_progression_qa import Session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True, help='trace/TCP-enabled current build')
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='sm-frame-states-', dir=args.artifacts.resolve()))
    report = {'passed': False, 'profile': str(root), 'invalid_cases': []}
    source = Session(args.exe, args.rom, root/'source', floor='hle', wide=True, capture_profile=False)
    try:
        source.boot(); source.land()
        source.step(8, 'start'); source.step(90)
        assert source.state()['state'] == 15
        source.step(8, 'start'); source.step(120)
        assert source.state()['state'] == 8
        report['same_process'] = source.save_roundtrip(7)
    finally:
        source.close()
    state = (root/'source/saves/save7.sav').read_bytes()
    assert state[:8] == b'RSGUARD\1', 'current states must carry build identity and CRC'
    def checksum(data):
        data = bytearray(data)
        struct.pack_into('<I', data, len(data)-4, zlib.crc32(data[:-4]))
        return bytes(data)
    corrupt = bytearray(state); corrupt[1000] ^= 1
    foreign = bytearray(state); foreign[16] ^= 1
    schema = bytearray(state)
    extension = schema.find(b'SFMS')
    assert extension >= 48
    struct.pack_into('<I', schema, extension + 4, 0xffffffff)
    invalid = {'truncated': state[:-10], 'bad-crc': bytes(corrupt),
               'foreign-build': checksum(foreign), 'unwrapped-legacy': state[48:-4],
               'bad-title-schema': checksum(schema)}
    fresh = Session(args.exe, args.rom, root/'fresh', floor='hle', wide=True, capture_profile=False)
    try:
        shutil.copyfile(root/'source/saves/save7.sav', root/'fresh/saves/save7.sav')
        fresh.step(2)
        def anchor():
            fresh.command('loadstate 7'); fresh.step(2)
            fresh.step(30, 'right')
        anchor()
        report['fresh_process'] = fresh.state()
        assert report['fresh_process']['room'] == 0x91f8
        assert report['fresh_process']['state'] == 8 and report['fresh_process']['health'] == 99
        assert report['fresh_process']['x'] != report['same_process']['saved']['x']
        fresh.step(2)
        expected = fresh.read(0, 0x20000)
        for name, data in invalid.items():
            (root/'fresh/saves/save10.sav').write_bytes(data)
            anchor()
            fresh.command('loadstate 10'); fresh.step(2)
            actual = fresh.read(0, 0x20000)
            assert actual == expected, f'{name}: rejected input changed guest RAM'
            report['invalid_cases'].append({'case': name, 'guest_unchanged': True})
            print(name + ': rejected before guest mutation', flush=True)
        fresh.step(30, 'right')
        assert fresh.state()['x'] != report['fresh_process']['x']
        report['passed'] = True
    finally:
        fresh.close()
        (root/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS: pause/unpause, current-build and fresh-process load, movement, invalid-input isolation', flush=True)


if __name__ == '__main__':
    main()
