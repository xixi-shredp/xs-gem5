"""Run the authorized 16/32 Hint metadata capacity experiment on ps1/ps2."""
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'results/model/smoke-test/meta32-20261022'
GEM5 = ROOT / 'xs-gem5/build/RISCV/gem5.opt'

def main():
    smoke.require_experiment_host()
    OUT.mkdir(parents=True, exist_ok=True)
    old_report = smoke.read(ROOT / 'results/model-smoke-test.json')
    smoke.write(OUT / 'previous-model-report.json', old_report)
    old_elf = OUT / 'gem5-meta16-original.opt'
    if not old_elf.exists():
        shutil.copy2(GEM5, old_elf)
    smoke.write(OUT / 'old-binary.json', smoke.identity(old_elf))
    budget = smoke.resource_snapshot(memory_per_job_gib=4)
    smoke.need(budget['job_capacity'] > 0, 'No build resource budget')
    cpus = sorted(os.sched_getaffinity(0))[:budget['job_capacity']]
    affinity = ','.join(map(str, cpus))
    smoke.write(OUT / 'build-resources.json', {**budget, 'selected_cpus': cpus})
    files = ['src/cpu/pred/btb/hint_indirect.hh', 'src/cpu/pred/btb/hint_indirect.cc',
             'src/cpu/pred/btb/decoupled_bpred.cc', 'src/cpu/pred/BranchPredictor.py',
             'configs/common/H2PHint.py', 'util/h2p_hint/spec_smoke.py',
             'util/h2p_hint/meta_capacity_experiment.py']
    identities = [smoke.identity(ROOT / 'xs-gem5' / f) for f in files]
    smoke.write(OUT / 'sources.json', identities)
    command = ['taskset', '-c', affinity, 'bash', '-c', 'scons -j$(nproc) build/RISCV/gem5.opt']
    smoke.write(OUT / 'build-command.json', {'argv': command, 'cwd': str(ROOT / 'xs-gem5'), 'host': smoke.execution_host()})
    print('BUILD START', len(cpus), 'CPU slots', flush=True)
    build_env = os.environ.copy()
    build_env['PATH'] = str(ROOT / 'deps/build-env/bin') + os.pathsep + build_env['PATH']
    smoke.write(OUT / 'compiler-wrapper.json', smoke.identity(ROOT / 'deps/build-env/bin/g++'))
    with (OUT / 'build.log').open('w') as log:
        result = subprocess.run(command, cwd=ROOT / 'xs-gem5', env=build_env, stdout=log, stderr=subprocess.STDOUT)
    smoke.write(OUT / 'build-exit.json', {'returncode': result.returncode})
    smoke.need(result.returncode == 0, 'scons failed; see build.log')
    for identity in identities:
        smoke.need(smoke.identity(identity['path']) == identity, 'Source changed during build')
    with (OUT / 'binary-check.log').open('w') as log:
        subprocess.run(['file', str(GEM5)], stdout=log, check=True)
        subprocess.run(['ls', '-l', str(GEM5)], stdout=log, check=True)
    smoke.need(os.access(GEM5, os.X_OK) and GEM5.open('rb').read(4) == b'\x7fELF', 'Not an executable ELF')
    new_elf = OUT / 'gem5-meta16-32.opt'
    shutil.copy2(GEM5, new_elf)
    smoke.write(OUT / 'new-binary.json', smoke.identity(new_elf))
    args = ['--manifest', str(ROOT / 'manifest/smoke-test.json'),
            '--workload-manifest', str(ROOT / 'workload/cpython-norvv-uart16550-100m200m-heavy.json'),
            '--input-root', '/home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m/',
            '--ref-so', str(ROOT / 'deps/NEMU-gem5/build/riscv64-nemu-interpreter-so'),
            '--gem5', str(new_elf), '--output-root', str(OUT / 'pipeline32'),
            '--hint-meta-entries', '32', '--stage', 'all', '--jobs', 'auto']
    print('BUILD PASS; PIPELINE32 START', flush=True)
    with (OUT / 'pipeline32.log').open('w') as log:
        result = subprocess.run([sys.executable, str(Path(smoke.__file__)), *args], stdout=log, stderr=subprocess.STDOUT)
    smoke.need(result.returncode == 0, '32-entry pipeline failed; see pipeline32.log')
    finish(old_report, new_elf)

def finish(old_report, new_elf):
    smoke.need(smoke.identity(new_elf) == smoke.read(OUT / 'new-binary.json'), 'Frozen binary identity differs')
    print('PIPELINE32 PASS; VERIFY16 START', flush=True)
    prepared = smoke.read(OUT / 'pipeline32/prepared.json')
    from types import SimpleNamespace
    runner = smoke.Runner(SimpleNamespace(output_root=OUT / 'verify16', manifest=ROOT / 'manifest/smoke-test.json',
        workload_manifest=ROOT / 'workload/cpython-norvv-uart16550-100m200m-heavy.json',
        input_root=Path('/home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m/'),
        gem5=new_elf, ref_so=ROOT / 'deps/NEMU-gem5/build/riscv64-nemu-interpreter-so',
        import_baseline_root=None, memory_per_job_gib=12, jobs='auto', hint_meta_entries=16))
    runner.plan_resources('verify16', 10)
    def verify(cpt):
        directory = OUT / 'verify16' / cpt['bench'] / cpt['cpt_simpoint_id']
        previous = cpt['new_results']
        smoke.need(smoke.identity(cpt['checkpoint']['path']) == cpt['checkpoint'], 'Checkpoint identity differs')
        expected_sha = previous.get('sha256')
        if expected_sha is None:
            expected_sha = next(r['image_identity']['sha256'] for r in old_report['serial_iteration']['results']
                                if r['image_identity']['path'] == previous['image'])
        smoke.need(smoke.sha(previous['image']) == expected_sha, 'Old selected image SHA differs')
        runner.check_runtime_inputs()
        command, env = smoke.render_command(runner.manifest, cpt['checkpoint']['path'], directory,
            runner.ref, new_elf, 'replay', image=previous['image'], meta_entries=16)
        if not (directory / 'report.json').exists():
            runner.invoke(command, directory, env)
        report = runner.audit_simulation(directory)
        smoke.need(report['window'] == previous['window'], '16-entry regression: retirement window or IPC differs')
        smoke.need(report['prediction_metrics'] == previous['prediction_metrics'], '16-entry regression: prediction counters differ')
        smoke.write(directory / 'report.json', report)
        fixed32_directory = OUT / 'fixed32' / cpt['bench'] / cpt['cpt_simpoint_id']
        command32, env32 = smoke.render_command(runner.manifest, cpt['checkpoint']['path'], fixed32_directory,
            runner.ref, new_elf, 'replay', image=previous['image'], meta_entries=32)
        if not (fixed32_directory / 'report.json').exists():
            runner.invoke(command32, fixed32_directory, env32)
        auditor32 = object.__new__(smoke.Runner)
        auditor32.args = SimpleNamespace(hint_meta_entries=32)
        auditor32.common = runner.common
        auditor32.ref = runner.ref
        fixed32 = auditor32.audit_simulation(fixed32_directory)
        smoke.write(fixed32_directory / 'report.json', fixed32)
        return {'fixed32': fixed32, 'tag': cpt['bench'] + '/' + cpt['cpt_simpoint_id'], 'bit_exact_metrics': True, 'report': report}
    with concurrent.futures.ThreadPoolExecutor(max_workers=runner.max_jobs) as pool:
        verified = list(pool.map(verify, old_report['cpts']))
    smoke.write(OUT / 'verify16-summary.json', {'status': 'PASS', 'slices': verified})
    result32 = smoke.read(OUT / 'pipeline32/summary.json')
    rows = []
    for cpt, revised in zip(old_report['cpts'], result32['slices']):
        smoke.need(revised['tag'] == cpt['bench'] + '/' + cpt['cpt_simpoint_id'], 'Slice mismatch')
        old = cpt['new_results']; new = revised['best_hint_candidate']; off = revised['baseline']
        smoke.need(off['window'] == cpt['baseline_results']['window'], 'Hint Off regression')
        key = 'nonreturn_ittage_plus_hint'
        rows.append({'tag': revised['tag'], 'baseline_ipc': off['ipc'], 'ipc16': old['ipc'], 'ipc32': new['ipc'],
            'speedup32_over16': new['ipc']/old['ipc'], 'speedup32_over_off': new['ipc']/off['ipc'],
            'miss16': old['prediction_metrics'][key]['misses'], 'miss32': new['prediction_metrics'][key]['misses'],
            'candidate16': cpt['selected_candidate'], 'candidate32': new['label'],
            'image16': old['image'], 'image32': new['image'],
            'image16_sha256': smoke.sha(old['image']), 'image32_sha256': smoke.sha(new['image']),
            'fixed32_ipc': verified[len(rows)]['fixed32']['ipc'],
            'fixed32_miss': verified[len(rows)]['fixed32']['prediction_metrics'][key]['misses'],
            'fixed32_cycles': verified[len(rows)]['fixed32']['window']['measure_cycles'],
            'fixed32_queue': verified[len(rows)]['fixed32']['prediction_metrics']['hint_public_queue'],
            'cycles16': old['window']['measure_cycles'], 'cycles32': new['window']['measure_cycles'],
            'queue16': old['prediction_metrics']['hint_public_queue'], 'queue32': new['prediction_metrics']['hint_public_queue']})
    instructions = 10_000_000
    ipc16 = instructions / sum(r['cycles16'] for r in rows)
    ipc32 = instructions / sum(r['cycles32'] for r in rows)
    summary = {'status': 'PASS', 'scope': '32-entry full reprofiling and three static candidates per slice; previous16 includes four serial candidates, no new serial iteration',
        'host': smoke.execution_host(), 'warmup': 1_000_000, 'measure': 1_000_000, 'difftest': True,
        'capacity16_regression': 'PASS: all 10 old selected images reproduce exact windows and prediction metrics',
        'slices': rows, 'aggregate': {'ipc16': ipc16, 'ipc32': ipc32, 'speedup32_over16': ipc32/ipc16,
            'miss16': sum(r['miss16'] for r in rows), 'miss32': sum(r['miss32'] for r in rows),
            'fixed32_ipc': instructions / sum(r['fixed32_cycles'] for r in rows),
            'fixed32_miss': sum(r['fixed32_miss'] for r in rows)}}
    smoke.write(OUT / 'comparison.json', summary)
    print(json.dumps(summary['aggregate']), flush=True)
    print('EXPERIMENT PASS', flush=True)

if __name__ == '__main__':
    try:
        if '--resume-analysis' in sys.argv:
            finish(smoke.read(OUT / 'previous-model-report.json'), OUT / 'gem5-meta16-32.opt')
        else:
            main()
    except Exception as error:
        smoke.write(OUT / 'error.json', {'error': str(error), 'time': time.time()})
        raise
