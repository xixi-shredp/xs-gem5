"""Single-core BackendV2, scalar FP, no RVV execution and no LLC.

Off/collect/replay use the fork Hint-v5 options. The entry locks structural
parameters so every experiment phase instantiates the same baseline machine.
"""
import hashlib
import json
import os
# New local reference must validate this build's actual register ABI.
os.environ["GEM5_REQUIRE_DIFFTEST_ABI"] = "1"
from pathlib import Path
import sys

import m5
from m5.objects import *
from m5.util import addToPath, fatal

addToPath('../')
addToPath('../../')
from common import Simulation
from common.BackendV2NoRVV import (
    BPU_PARAMS, CPU_PARAMS, CORE_FREQUENCY_HZ, TICK_FREQUENCY_HZ,
    RESPONSE_PADDING_CYCLES, RESIDUAL_DIFFERENCES,
)
from common.FUScheduler import BackendV2NoRVVScheduler
from common.LSQBankConflict import set_lsq_bank_conflict_cache_params
from common.xiangshan import (
    xiangshan_system_init, build_xiangshan_system,
    configure_xiangshan_linux_workload,
)
from kmhv3 import setPtwLevelLimitParams

DRAM_INI = Path(__file__).resolve().parents[1] / 'common/DDR4_3200_2ch.ini'
DRAM_INI_SHA256 = '0cf98cd29657a9365f772ff6d8dfaf3f2aa795593f0b16cc9e52dc5654e79fc6'


def lock_arguments(args):
    if (args.num_cpus != 1 or args.smt or args.ruby or
            args.external_memory_system or args.xiangshan_ecore):
        fatal('BackendV2NoRVVNoLLC requires one classic-cache scalar core')
    if args.restore_rvv_cpt or '--enable-riscv-vector' in sys.argv:
        fatal('This experiment accepts no-RVV checkpoints only')
    # The common kmhv3 initializer enables RVV as its final default. Override
    # it before system construction, alongside the ISA and decoder gates.
    args.enable_riscv_vector = False
    if hashlib.sha256(DRAM_INI.read_bytes()).hexdigest() != DRAM_INI_SHA256:
        fatal('Frozen DDR4_3200_2ch.ini identity changed')
    args.cpu_clock = args.sys_clock = '3GHz'
    args.bp_type = 'DecoupledBPUWithBTB'
    args.kmh_align = True
    args.classic_l2 = False
    args.l2_size, args.l2_assoc, args.l2_slices = '2MB', 8, 4
    args.l1i_size = args.l1d_size = '64kB'
    args.l1i_assoc = args.l1d_assoc = 4
    args.cacheline_size = 64
    args.no_l3cache = True
    args.l3cache = args.l2_to_l3_pf_hint = False
    args.mem_type = 'DRAMsim3'
    # DRAMsim3's ini owns both physical channels. Two gem5 objects would
    # duplicate that channel topology and controller queue capacity.
    args.mem_channels = 1
    args.dramsim3_ini = str(DRAM_INI)
    args.cdp_use_dynamic_degree = False
    args.cdp_accuracy_threshold = 0.05
    args.cdp_use_accuracy_dependent_alignment = False
    args.cdp_use_sv48 = True
    if any(getattr(args, flag, False) for flag in
           ('ideal_2_taken', 'ideal_cache', 'no_pf', 'btb_tage_upper_bound')):
        fatal('Ideal predictors/caches or disabled prefetching violate this contract')


def set_backend_v2_params(args, system):
    for cpu in system.cpu:
        for name, value in CPU_PARAMS.items():
            setattr(cpu, name, value)
        cpu.robWalkPolicy = args.rob_walk_policy
        if args.warmup_insts_no_switch is None or not args.maxinsts:
            fatal('This entry requires explicit warmup and maxinsts windows')
        measure_inst_count = args.maxinsts - args.warmup_insts_no_switch
        if measure_inst_count <= 0:
            fatal('Measured instruction window must be positive')
        cpu.measureInstCount = measure_inst_count
        cpu.dumpWarmupStats = True
        cpu.scheduler = BackendV2NoRVVScheduler()
        for isa in cpu.isa:
            isa.enable_rvv = False
        for decoder in cpu.decoder:
            decoder.enable_rvv = False
        for tlb in (cpu.mmu.itb, cpu.mmu.dtb):
            tlb.enable_l1_direct_compression = args.enable_l1_direct_compression
            setPtwLevelLimitParams(args, tlb)
        for component, params in BPU_PARAMS.items():
            predictor = getattr(cpu.branchPred, component) if component else cpu.branchPred
            for name, value in params.items():
                setattr(predictor, name, value)
        for cache in (cpu.icache, cpu.dcache):
            cache.size, cache.assoc = '64kB', 4
            cache.replacement_policy = TreePLRURP()
        cpu.icache.mshrs = 4
        cpu.dcache.mshrs = 16
        cpu.dcache.tag_load_read_ports = 100
        cpu.dcache.do_fast_writeline = True
        cpu.dcache.simulate_dcache_refill = True
        cpu.dcache.prefetch_can_offload = False
        set_lsq_bank_conflict_cache_params(cpu, system)
    for i, wrapper in enumerate(system.l2_wrappers):
        wrapper.data_sram_banks = wrapper.dir_sram_banks = 1
        wrapper.pipe_dir_write_stage = 3
        wrapper.dir_read_bypass = False
        for cache_slice in wrapper.slices:
            cache = cache_slice.inner_cache
            cache.wpu = NULL
            cache.do_fast_writeline = True
            cache.prefetch_can_offload = False
            cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=1024)
        pf = wrapper.prefetcher
        pf.enable_bop, pf.enable_cdp, pf.enable_despacito_stream = True, True, False
        pf.enable_cmc = False
        bus = system.tol2bus_list[i]
        bus.forward_latency = bus.response_latency = 3
        bus.hint_wakeup_ahead_cycles = 1
        bus.layer_bandwidth_configs = [
            LayerBandwidthConfig(direction='req', port_index=0, max_per_cycle=2),
            LayerBandwidthConfig(direction='resp', port_index=1, max_per_cycle=2),
        ]
    assert not args.l3cache and not args.l2_to_l3_pf_hint
    for memory in system.mem_ctrls:
        assert isinstance(memory, DRAMsim3)
        memory.clk_domain = system.cpu_clk_domain
        memory.response_padding = RESPONSE_PADDING_CYCLES
        memory.defer_write_ack = True
        # Match the finite RTL NCB/AXI owner envelope; the ini still controls
        # independent native transaction queues of 32 in each of two channels.
        memory.max_pending_packets = 64
        memory.filePath = str(Path(m5.options.outdir).resolve())
    assert len(system.mem_ctrls) == 1
    # Supplement config.ini with stable names/units and known model gaps.
    record = {
        'format': 'backend-v2-norvv-nol3-effective-parameters',
        'tick_frequency_hz': TICK_FREQUENCY_HZ,
        'core_and_system_frequency_hz': CORE_FREQUENCY_HZ,
        'cpu': CPU_PARAMS, 'bpu': BPU_PARAMS,
        'scheduler': 'BackendV2NoRVVScheduler',
        'rvv_enabled': False, 'llc_enabled': False,
        'dram_ini': str(DRAM_INI), 'dram_ini_sha256': DRAM_INI_SHA256,
        'dram_response_padding_core_cycles': RESPONSE_PADDING_CYCLES,
        'deferred_write_ack': True, 'residual_differences': RESIDUAL_DIFFERENCES,
        'dram_controller_objects': 1, 'dram_ini_internal_channels': 2,
        'core_clock_ticks': 1000, 'dram_tck_ticks': 1890,
        'dram_padding_ticks': 66000,
        'dram_max_pending_packets': 64,
        'dram_native_transaction_queue_per_channel': 32,
    }
    out = Path(m5.options.outdir)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'backend-v2-effective-parameters.json').write_text(
        json.dumps(record, indent=2) + '\n')


if __name__ == '__m5_main__':
    m5.ticks.setGlobalFrequency(TICK_FREQUENCY_HZ)
    args = xiangshan_system_init()
    lock_arguments(args)
    test_sys = build_xiangshan_system(args)
    if args.raw_cpt and os.path.basename(args.generic_rv_cpt) == 'linux.bin':
        configure_xiangshan_linux_workload(test_sys, args)
    set_backend_v2_params(args, test_sys)
    root = Root(full_system=True, system=test_sys)
    Simulation.run_vanilla(args, root, test_sys, None)
