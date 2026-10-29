"""Build and validate the updated CAM58 model on the authorized server."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'results/model/smoke-test' / os.environ.get('CAM58_RUN_ID', 'cam58-20261023')
GEM5 = ROOT / 'xs-gem5/build/RISCV/gem5.opt'

def run(argv, logfile, cwd=ROOT / 'xs-gem5', env=None):
    smoke.require_experiment_host()
    with logfile.open('w') as log:
        result = subprocess.run(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
    smoke.need(result.returncode == 0, f'Command failed {result.returncode}; see {logfile}')

def main():
    smoke.require_experiment_host()
    OUT.mkdir(parents=True, exist_ok=True)
    smoke.write(OUT / 'previous-model-report.json', smoke.read(ROOT / 'results/model-smoke-test.json'))
    if not (OUT / 'old-binary.json').exists():
        frozen_old = OUT / 'gem5-before-cam58.opt'
        shutil.copy2(GEM5, frozen_old)
        smoke.write(OUT / 'old-binary.json', smoke.identity(frozen_old))
    files = ['src/cpu/pred/btb/common.hh', 'src/cpu/pred/btb/decoupled_bpred.cc',
        'src/cpu/pred/btb/decoupled_bpred.hh', 'src/cpu/pred/btb/decoupled_bpred_stats.cc',
        'src/cpu/pred/btb/btb_ittage.cc', 'src/cpu/pred/btb/hint_indirect.cc',
        'src/cpu/pred/btb/hint_indirect.hh', 'src/cpu/pred/btb/hint_history.cc',
        'src/cpu/pred/btb/hint_history.hh', 'src/cpu/pred/btb/test/hint_indirect_test.cc',
        'src/cpu/o3/fetch.cc', 'src/cpu/o3/cpu.cc', 'src/cpu/o3/cpu.hh', 'src/cpu/o3/iew.cc',
        'util/h2p_hint/cam58.py', 'util/h2p_hint/cam58_test.py', 'util/h2p_hint/spec_smoke.py',
        'util/h2p_hint/cam58_experiment.py', 'src/cpu/pred/btb/test/run_hint_cam58_tests.py']
    identities = [smoke.identity(ROOT / 'xs-gem5' / f) for f in files]
    smoke.write(OUT / 'sources.json', identities)
    smoke.write(OUT / 'docs.json', [smoke.identity(p) for p in (ROOT / 'docs').glob('*.md')])
    budget = smoke.resource_snapshot(memory_per_job_gib=4)
    smoke.need(budget['job_capacity'] > 0, 'No build resource budget')
    cpus = sorted(os.sched_getaffinity(0))[:budget['job_capacity']]
    command = ['taskset', '-c', ','.join(map(str,cpus)), 'bash', '-c', 'scons -j$(nproc) build/RISCV/gem5.opt']
    smoke.write(OUT / 'build-command.json', {'argv': command, 'budget': budget, 'cpus': cpus,
        'cwd': str(ROOT / 'xs-gem5'), 'wrapper': smoke.identity(ROOT / 'deps/build-env/bin/g++')})
    env = os.environ.copy()
    env['PATH'] = str(ROOT / 'deps/build-env/bin') + os.pathsep + env['PATH']
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    print('BUILD START', len(cpus), flush=True)
    run(command, OUT / 'build.log', env=env)
    smoke.write(OUT / 'build-exit.json', {'returncode': 0})
    for item in identities:
        smoke.need(smoke.identity(item['path']) == item, 'Source drift during build')
    run(['file', str(GEM5)], OUT / 'file.log')
    run(['ls', '-l', str(GEM5)], OUT / 'permissions.log')
    smoke.need(os.access(GEM5,os.X_OK) and GEM5.open('rb').read(4)==b'\x7fELF','Not executable ELF')
    frozen = OUT / 'gem5-cam58.opt'
    shutil.copy2(GEM5,frozen)
    smoke.write(OUT / 'binary.json', smoke.identity(frozen))
    validate(frozen, env)

def validate(frozen, env):
    smoke.need(smoke.identity(frozen) == smoke.read(OUT / 'binary.json'), 'Frozen ELF changed')
    print('BUILD PASS; DIRECTED START', flush=True)
    run([sys.executable, 'util/h2p_hint/cam58_test.py'], OUT / 'adapter-test.log', env=env)
    run([sys.executable, 'src/cpu/pred/btb/test/run_hint_cam58_tests.py', '--gem5', str(frozen),
         '--outdir', str(OUT / 'directed'), '--remote-alias', 'ps1'], OUT / 'directed.log', env=env)
    print('DIRECTED PASS; PIPELINE START', flush=True)
    args = [sys.executable, 'util/h2p_hint/spec_smoke.py', '--manifest', str(ROOT / 'manifest/smoke-test.json'),
        '--workload-manifest', str(ROOT / 'workload/cpython-norvv-uart16550-100m200m-heavy.json'),
        '--input-root', '/home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m/',
        '--ref-so', str(ROOT / 'deps/NEMU-gem5/build/riscv64-nemu-interpreter-so'),
        '--gem5', str(frozen), '--output-root', str(OUT / 'pipeline'),
        '--hint-meta-entries', '16', '--stage', 'all', '--jobs', 'auto']
    run(args, OUT / 'pipeline.log', env=env)
    print('PIPELINE PASS', flush=True)

if __name__ == '__main__':
    try:
        if '--resume-validation' in sys.argv:
            env = os.environ.copy()
            env['PYTHONDONTWRITEBYTECODE'] = '1'
            validate(OUT / 'gem5-cam58.opt', env)
        else:
            main()
    except Exception as error:
        smoke.write(OUT / 'error.json', {'error':str(error),'time':time.time()})
        raise
