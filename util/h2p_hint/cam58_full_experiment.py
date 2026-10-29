"""Run the explicitly authorized 100M/200M model experiment on ps1."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'results/model/full-test' / os.environ.get('CAM58_FULL_RUN_ID', 'cam58-100m200m-20261023')

def main():
    smoke.require_experiment_host()
    OUT.mkdir(parents=True, exist_ok=True)
    binary = ROOT / 'results/model/smoke-test/cam58-20261023-v2/gem5-cam58.opt'
    expected = smoke.read(binary.parent / 'binary.json')
    ref = Path(os.environ.get('CAM58_FULL_REF', str(ROOT / 'deps/NEMU-gem5/build/riscv64-nemu-interpreter-so')))
    smoke.need(smoke.identity(binary) == expected, 'Frozen CAM58 binary changed')
    manifest = OUT / 'full-test.json'
    inputs = [smoke.identity(p) for p in [manifest, Path(__file__),
              Path(smoke.__file__), ROOT / 'xs-gem5/util/h2p_hint/cam58.py',
              ROOT / 'xs-gem5/util/h2p_hint/spec_trace.py', ref]]
    smoke.write(OUT / 'inputs.json', {'binary': expected, 'sources': inputs,
                'docs': [smoke.identity(p) for p in (ROOT / 'docs').glob('*.md')],
                'host': smoke.execution_host(), 'warmup': 100000000, 'measure': 200000000})
    args = [sys.executable, str(Path(smoke.__file__)), '--full-test', '--manifest', str(manifest),
            '--workload-manifest', str(ROOT / 'workload/cpython-norvv-uart16550-100m200m-heavy.json'),
            '--input-root', '/home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m',
            '--ref-so', str(ref),
            '--gem5', str(binary), '--output-root', str(OUT / 'pipeline'),
            '--hint-meta-entries', '16', '--reserve-gib', '32']
    for stage in smoke.STAGES:
        # Trace validation is in the parent process; keep the large offline
        # joins serial and capture+validate at two slices concurrently.
        jobs, memory = ('auto', '12') if stage in ('baseline', 'replay', 'select') else (
                        ('2', '64') if stage in ('discovery', 'detail') else ('1', '96'))
        command = args + ['--stage', stage, '--jobs', jobs, '--memory-per-job-gib', memory]
        smoke.write(OUT / f'{stage}-command.json', {'argv': command, 'started': time.time()})
        smoke.write(OUT / 'status.json', {'status': 'RUNNING', 'stage': stage, 'time': time.time()})
        print('START', stage, flush=True)
        with (OUT / f'{stage}.log').open('w') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    cwd=ROOT / 'xs-gem5')
        smoke.need(result.returncode == 0, f'{stage} failed: {result.returncode}')
        print('PASS', stage, flush=True)
    summary = smoke.read(OUT / 'pipeline/summary.json')
    smoke.need(summary['complete'] and len(summary['slices']) == 10, 'Incomplete full test')
    for item in inputs:
        smoke.need(smoke.identity(item['path']) == item, 'Input drift')
    aggregates = {}
    for key in ('baseline', 'best_hint_candidate'):
        runs = [r[key] for r in summary['slices']]
        smoke.need(all(r['difftest_enabled'] and r['window']['measure_instructions'] == 200000000
                   and r['window']['warmup_instructions'] == 100000000 for r in runs), 'Window/difftest drift')
        cycles = sum(r['window']['measure_cycles'] for r in runs)
        aggregates[key] = {'ipc': 2000000000 / cycles, 'cycles': cycles,
          'nonreturn_misses': sum(r['prediction_metrics']['nonreturn_ittage_plus_hint']['misses'] for r in runs)}
    smoke.write(OUT / 'aggregate.json', aggregates)
    smoke.write(OUT / 'status.json', {'status': 'PASS', 'time': time.time(), 'aggregate': aggregates})
    print('FULL PIPELINE PASS', json.dumps(aggregates), flush=True)

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        smoke.write(OUT / 'status.json', {'status': 'FAIL', 'error': str(error), 'time': time.time()})
        raise
