#!/usr/bin/env python3
"""Validate CAM58 production helpers/loaders in final gem5.opt on ps1/ps2.

No standalone/gtest build. Run after scons has fully exited. Negative cases
assert both the exit status and the production-loader diagnostic.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--outdir', type=Path, required=True)
    parser.add_argument('--remote-alias', choices=('ps1', 'ps2'), required=True)
    args = parser.parse_args()
    args.gem5 = args.gem5.resolve()
    args.outdir = args.outdir.resolve()
    args.outdir.mkdir(parents=True, exist_ok=False)
    here = Path(__file__).resolve().parent
    config = here / 'hint_indirect_test_config.py'
    fixtures = args.outdir / 'fixtures'
    fixtures.mkdir()
    available_memory = next(int(line.split()[1]) * 1024 for line in
                            Path('/proc/meminfo').read_text().splitlines()
                            if line.startswith('MemAvailable:'))
    cpu_headroom = max(0, len(os.sched_getaffinity(0)) - int(os.getloadavg()[0]))
    workers = min(4, cpu_headroom, available_memory // (512 * 1024 * 1024))
    if workers < 1:
        raise SystemExit('Insufficient remote CPU/memory headroom for one helper')
    report = {'schema': 'hint-cam58-final-binary-validation', 'version': 1,
              'hostname': socket.gethostname(), 'ssh_alias': args.remote_alias,
              'gem5': str(args.gem5), 'gem5_sha256': sha(args.gem5),
              'source_sha256': {str(p): sha(p) for p in
                                (Path(__file__), config, here / 'hint_indirect_test.cc')},
              'resources_before': {'mem_available': available_memory,
                                   'cpu_headroom': cpu_headroom, 'workers': workers},
              'cases': [], 'passed': False}
    cases = [('production-helper', None, False, None)]

    def add(name, image, collect, diagnostic=None):
        path = fixtures / (name + '.bin')
        path.write_bytes(image)
        cases.append((name, path, collect, diagnostic))

    for collect in (False, True):
        label = 'cam' if collect else 'load'
        image = bytearray(144 if collect else 16784)
        image[:8] = b'H2PCAM58' if collect else b'H2PLD058'
        row = (0x1012 >> 1) | (1 << 51) | (16 << 52) | (1 << 57)
        struct.pack_into('<Q', image, 16 + 7 * 8, row)
        add(label + '-valid-native-position', image, collect)
        add(label + '-truncated', image[:-1], collect, 'incorrect raw Hint image length')
        bad = bytearray(image); bad[8] = 1
        add(label + '-header-reserved', bad, collect, 'raw Hint header reserved bytes must be zero')
        bad = bytearray(image); bad[:8] = b'H2PHINTP' if collect else b'H2PHINTL'
        add(label + '-reject-old-header', bad, collect,
            'finite Hint requires a CAM58 H2PCAM58 image' if collect else
            'finite Hint requires a CAM58 H2PLD058 image')
        bad = bytearray(image); struct.pack_into('<Q', bad, 16 + 7 * 8, row | (1 << 58))
        add(label + '-reserved-bit58', bad, collect, 'CAM58 reserved bits must be zero')
        bad = bytearray(image); struct.pack_into('<Q', bad, 16, 1 << 52)
        add(label + '-invalid-row-payload', bad, collect, 'invalid raw CAM row must be zero')
        bad = bytearray(image); struct.pack_into('<Q', bad, 16, row)
        add(label + '-duplicate-window', bad, collect, 'duplicate raw CAM startPC')
        for suffix, pc, position in [('position-before-start', 0x1012, 0),
                                      ('position-after-page', 0x1ff0, 16)]:
            bad = bytearray(image)
            struct.pack_into('<Q', bad, 16 + 7 * 8,
                             (pc >> 1) | (1 << 51) | (position << 52))
            add(label + '-' + suffix, bad, collect,
                'CAM58 CFI position is outside its window')
        add(label + '-reject-legacy-json', b'{"version":7}', collect,
            'finite Hint requires a CAM58 H2PCAM58 image' if collect else
            'finite Hint requires a CAM58 H2PLD058 image')

    def run(case):
        name, fixture, collect, diagnostic = case
        command = [str(args.gem5), '--outdir', str(args.outdir / ('gem5-' + name)), str(config)]
        if fixture:
            command += ['--candidates' if collect else '--metadata', str(fixture)]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True)
        stdout, stderr = args.outdir / (name + '.stdout'), args.outdir / (name + '.stderr')
        stdout.write_text(result.stdout); stderr.write_text(result.stderr)
        output = result.stdout + result.stderr
        passed = (result.returncode != 0 and diagnostic in output) if diagnostic else (
            result.returncode == 0 and 'Hint self-test passed' in output)
        return {'name': name, 'command': command, 'returncode': result.returncode,
                'expected_diagnostic': diagnostic, 'passed': passed,
                'seconds': round(time.monotonic() - started, 3),
                'stdout': str(stdout), 'stderr': str(stderr)}

    # The main helper creates its own independent fixtures; loader cases run in
    # disjoint output directories and use bounded available remote resources.
    with ThreadPoolExecutor(max_workers=workers) as pool:
        report['cases'] = list(pool.map(run, cases))
    report['passed'] = all(case['passed'] for case in report['cases'])
    report['fixture_sha256'] = {p.name: sha(p) for p in fixtures.iterdir()}
    destination = args.outdir / 'validation.json'
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'cases': len(cases), 'report': str(destination)}))
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
