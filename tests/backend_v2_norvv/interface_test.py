"""Evaluate active configuration declarations without loading any gem5 binary."""
import ast
import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[3]
GEM5 = ROOT / 'xs-gem5'


class Record:
    def __init__(self, **kwargs):
        for base in reversed(type(self).__mro__):
            for key, value in vars(base).items():
                if isinstance(value, (list, dict)):
                    setattr(self, key, copy.deepcopy(value))
        self.__dict__.update(kwargs)


class Params:
    def __getattr__(self, name):
        return lambda *args: args[0] if len(args) > 1 else None


def load_scheduler():
    # Execute the real FU and scheduler definitions, replacing only gem5's
    # unavailable generated parameter machinery with records.
    namespaces = {
        'm5': ModuleType('m5'), 'm5.SimObject': ModuleType('m5.SimObject'),
        'm5.params': ModuleType('m5.params'), 'm5.defines': ModuleType('m5.defines'),
        'm5.objects': ModuleType('m5.objects'),
        'm5.objects.FuncUnit': ModuleType('m5.objects.FuncUnit'),
        'm5.objects.FuncUnitConfig': ModuleType('m5.objects.FuncUnitConfig'),
        'm5.objects.FuncScheduler': ModuleType('m5.objects.FuncScheduler'),
    }
    namespaces['m5.SimObject'].SimObject = Record
    namespaces['m5.params'].Param = Params()
    namespaces['m5.params'].VectorParam = Params()
    namespaces['m5.defines'].buildEnv = {'TARGET_ISA': 'riscv'}
    namespaces['m5.objects.FuncUnit'].FUDesc = Record
    namespaces['m5.objects.FuncUnit'].OpDesc = Record
    previous = {key: sys.modules.get(key) for key in namespaces}
    sys.modules.update(namespaces)
    try:
        exec(compile((GEM5 / 'src/cpu/o3/FuncUnitConfig.py').read_text(),
                     'FuncUnitConfig.py', 'exec'), namespaces['m5.objects.FuncUnitConfig'].__dict__)
        exec(compile((GEM5 / 'src/cpu/o3/FuncScheduler.py').read_text(),
                     'FuncScheduler.py', 'exec'), namespaces['m5.objects.FuncScheduler'].__dict__)
        result = {}
        exec(compile((GEM5 / 'configs/common/FUScheduler.py').read_text(),
                     'FUScheduler.py', 'exec'), result)
        return result['BackendV2NoRVVScheduler']()
    finally:
        for key, value in previous.items():
            if value is None: sys.modules.pop(key, None)
            else: sys.modules[key] = value


class InterfaceTests(unittest.TestCase):
    def test_scalar_scheduler_contract(self):
        scheduler = load_scheduler()
        contract = json.loads((ROOT / 'results/v2config-norvv-nol3-1m1m/20260930/stage1/contract.json').read_text())
        scalar = contract['backend_mapping']['scalar_queues']
        queues = [row for kind in scalar.values() if isinstance(kind, list)
                  for row in kind]
        actual = {iq.name: iq for iq in scheduler.IQs}
        self.assertEqual(set(actual), {row['name'] for row in queues})
        self.assertEqual(sum(iq.size for iq in actual.values()), 246)
        self.assertEqual(sum(len(iq.oports) for iq in actual.values()), 17)
        for row in queues:
            iq = actual[row['name']]
            self.assertEqual((iq.size, iq.inports), (row['entries'], 2))
            self.assertEqual(iq.vectorSplitUnits, 0)
            self.assertEqual(len(iq.oports), len(row['issue_ports']))
            for expected, port in zip(row['issue_ports'], iq.oports):
                encoded = [((0 if rd['class'] == 'INT' else 1) << 6) |
                           rd['port'] << 2 | rd['priority'] for rd in expected['reads']]
                self.assertEqual([rp for rp in port.rp if not rp & 256], encoded)
                self.assertTrue(port.rp)
        self.assertFalse(scheduler.xbarWakeup)
        self.assertFalse(scheduler.enableMainRdpOpt)
        self.assertEqual(scheduler.intRegfileBanks, 1)
        self.assertEqual(len(scheduler.specWakeupNetwork), 4)
        classes = {op.opClass for iq in actual.values() for port in iq.oports
                   for fu in port.fu for op in fu.opList}
        self.assertFalse(any('Vector' in op or 'Simd' in op for op in classes))
        self.assertTrue({'Int2Fp', 'FloatMv', 'FloatCvt', 'FloatMisc',
                         'FloatDiv', 'FloatSqrt', 'FloatMemRead', 'FloatMemWrite'} <= classes)

    def test_config_and_clock_contract(self):
        spec = importlib.util.spec_from_file_location('backend_params', GEM5 / 'configs/common/BackendV2NoRVV.py')
        params = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(params)
        self.assertEqual(params.TICK_FREQUENCY_HZ // params.CORE_FREQUENCY_HZ, 1000)
        self.assertEqual(round(params.TICK_FREQUENCY_HZ * .63e-9), 1890)
        self.assertEqual(params.RESPONSE_PADDING_CYCLES * 1000, 66000)
        self.assertEqual(params.CPU_PARAMS['commitWidth'], 8)
        self.assertEqual(params.CPU_PARAMS['numPhysVecRegs'], 40)
        self.assertEqual(params.CPU_PARAMS['numPhysVecPredRegs'], 1)
        self.assertEqual(params.BPU_PARAMS['tage']['tableSizes'], [2048] * 8)
        text = (GEM5 / 'configs/example/backend_v2_norvv_nol3.py').read_text()
        ast.parse(text)
        self.assertNotIn('disableAllRegArb', text)
        self.assertIn('args.mem_channels = 1', text)
        self.assertIn('args.enable_riscv_vector = False', text)
        self.assertIn('isa.enable_rvv = False', text)
        self.assertIn('decoder.enable_rvv = False', text)

    def test_phase_and_packet_integration(self):
        cpu = (GEM5 / 'src/cpu/o3/cpu.cc').read_text()
        self.assertIn('measureCommitted == measureLimit', cpu)
        self.assertIn('phaseParams.measureInstCount', cpu)
        self.assertIn('reset_retired=%llu', cpu)
        reset = cpu.index('measureResetRetired = retiredPostRestore;')
        self.assertLess(reset, cpu.index('statistics::dump();', reset))
        self.assertLess(cpu.index('statistics::dump();', reset), cpu.index('statistics::reset();', reset))
        memory = (GEM5 / 'src/mem/dramsim3.cc').read_text()
        accepted = memory[memory.index('if (deferWriteAck)'):memory.index('} else {', memory.index('if (deferWriteAck)'))]
        self.assertIn('access(pkt)', accepted)
        self.assertIn('completions.accept', accepted)
        self.assertNotIn('accessAndRespond', accepted)
        self.assertIn('completions.outstanding() + responseQueue.size()', memory)
        self.assertIn('responsePadding * clockPeriod()', memory)
        self.assertIn('reschedule(completionEvent, ready)', memory)
        self.assertIn('50 * sim_clock::as_int::ns', memory)

    def test_production_logic_headers(self):
        with tempfile.TemporaryDirectory(prefix='backend-v2-test-', dir=ROOT / 'results/v2config-norvv-nol3-1m1m/20260930/stage2') as directory:
            binary = Path(directory) / 'logic-test'
            stub = Path(directory) / 'base'
            stub.mkdir()
            (stub / 'types.hh').write_text('#include <cstdint>\nnamespace gem5 { using Addr = uint64_t; }\n')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', directory, '-I', str(GEM5 / 'src'),
                            str(Path(__file__).with_name('logic_test.cc')),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_formal_runner_contract_and_independent_window(self):
        sys.path.insert(0, str(GEM5))
        from util.h2p_hint.finite_runner import (
            audit_backend_run, separate_warmup_dump, parse_args, _command,
        )
        frozen = ROOT / 'results/v2config-norvv-nol3-1m1m/20260930/stage1/contract.json'
        parsed = parse_args([
            '--gem5', str(ROOT / 'pending-gem5'),
            '--config', str(GEM5 / 'configs/example/backend_v2_norvv_nol3.py'),
            '--workload-list', str(ROOT / 'workload/cpython-norvv-heavy.json'),
            '--output-root', str(ROOT / 'pending-run'),
            '--source-manifest', str(ROOT / 'pending-source'),
            '--build-audit', str(ROOT / 'pending-build'),
            '--validation-audit', str(ROOT / 'pending-validation'),
            '--experiment-contract', str(frozen)])
        self.assertEqual(parsed.mem_type, 'DRAMsim3')
        parsed.command_prefix = []
        command = _command(parsed, {'checkpoint': {'path': 'checkpoint.zstd'}},
                           'baseline', ROOT / 'pending-run')
        self.assertIn('--mem-channels=1', command)
        self.assertIn('--no-l3cache', command)
        self.assertIn('--mem-type=DRAMsim3', command)
        marker = '---------- Begin Simulation Statistics ----------\n'
        with tempfile.TemporaryDirectory(prefix='window-test-', dir=ROOT / 'results/v2config-norvv-nol3-1m1m/20260930/stage2') as directory:
            directory = Path(directory)
            (directory / 'stats.txt').write_text(marker + 'simInsts 1000000\n' +
                                               marker + 'simInsts 1000000\n')
            separate_warmup_dump(directory)
            self.assertEqual((directory / 'stats.txt').read_text().count(marker), 1)
            effective = {'tick_frequency_hz': 3_000_000_000_000,
                         'core_and_system_frequency_hz': 3_000_000_000,
                         'rvv_enabled': False, 'llc_enabled': False,
                         'dram_controller_objects': 1, 'dram_ini_internal_channels': 2,
                         'core_clock_ticks': 1000, 'dram_tck_ticks': 1890,
                         'dram_padding_ticks': 66000, 'deferred_write_ack': True,
                         'dram_response_padding_core_cycles': 66,
                         'dram_max_pending_packets': 64,
                         'dram_native_transaction_queue_per_channel': 32,
                         'dram_ini_sha256': 'test-ini'}
            (directory / 'backend-v2-effective-parameters.json').write_text(json.dumps(effective))
            (directory / 'config.ini').write_text('enable_rvv=false\nenable_rvv=false\n'
                                                  'defer_write_ack=true\nresponse_padding=66\n'
                                                  'max_pending_packets=64\n')
            (directory / 'stderr.log').write_text('')
            summary = {'format': 'committed-nonreturn-indirect-targets',
                       'executions': 10, 'absent_target': 1, 'valid_wrong_target': 2,
                       'target_misses': 3, 'pcs': [{'executions': 10}]}
            (directory / 'h2p-target-summary.json').write_text(json.dumps(summary))
            args = SimpleNamespace(experiment_contract=frozen, warmup_insts=1000000,
                                   measure_insts=1000000,
                                   identity={'dram_ini': {'sha256': 'test-ini'}})
            def check(warmup_actual, measure_actual):
                (directory / 'stdout.log').write_text(
                    'H2P_PHASE phase=restore_complete tick=0 core_cycle=0 retired_post_restore=0 reset_retired=0\n'
                    f'H2P_PHASE phase=warmup_complete tick=1000000000 core_cycle=1000000 retired_post_restore={warmup_actual} reset_retired={warmup_actual}\n'
                    f'H2P_PHASE phase=measure_complete tick=2000000000 core_cycle=2000000 retired_post_restore={warmup_actual + measure_actual} reset_retired={warmup_actual}\n')
                return audit_backend_run(args, directory,
                                         {'result': 'pass', 'errors': [], 'final_misses': 3})
            passed = check(1000003, 1000002)
            self.assertEqual(passed['result'], 'pass', passed['errors'])
            self.assertEqual(check(1000003, 999997)['result'], 'fail')
            self.assertEqual(check(1000000, 1000008)['result'], 'fail')
            # Global owner/response capacity and native per-channel transaction
            # queues are separate resources; either mismatch fails the run.
            effective['dram_max_pending_packets'] = 32
            (directory / 'backend-v2-effective-parameters.json').write_text(json.dumps(effective))
            self.assertEqual(check(1000000, 1000000)['result'], 'fail')
            effective['dram_max_pending_packets'] = 64
            effective['dram_native_transaction_queue_per_channel'] = 64
            (directory / 'backend-v2-effective-parameters.json').write_text(json.dumps(effective))
            self.assertEqual(check(1000000, 1000000)['result'], 'fail')


if __name__ == '__main__':
    unittest.main()
