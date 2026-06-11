import importlib.util
import os
from pathlib import Path

import m5
from m5.objects import *
from common.Caches import *
from common import ObjectList
from m5.objects.Prefetcher import XSPhysicalSmallBOP, XSVirtualLargeBOP


def _get_hwp(hwp_option):
    if hwp_option == None:
        return NULL
    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()

def _set_param_if_present(obj, name, value):
    if hasattr(obj, name):
        setattr(obj, name, value)


def _prefetcher_type_name(prefetcher):
    return str(getattr(prefetcher, 'type', prefetcher.__class__.__name__))


_PF_DSE_HELPER_MODULE = None
_PF_DSE_HELPER_PATH = None


def _resolve_pf_dse_helper(config_path):
    cfg_path = Path(config_path).expanduser().resolve()
    candidates = []
    if cfg_path.parent.name == 'configs':
        candidates.append(cfg_path.parent.parent / 'gem5_py' / 'get-xs-gem5-pf-cfg.py')

    pf_dse_root = os.environ.get('PF_DSE_ROOT')
    if pf_dse_root:
        candidates.append(Path(pf_dse_root).expanduser() / 'gem5_py' / 'get-xs-gem5-pf-cfg.py')

    candidates.append(Path.home() / 'xs-env' / 'pf-dse' / 'gem5_py' / 'get-xs-gem5-pf-cfg.py')

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    searched = ', '.join(str(candidate) for candidate in candidates)
    raise RuntimeError('cannot locate pf-dse helper get-xs-gem5-pf-cfg.py; searched: ' + searched)


def _load_pf_dse_helper(config_path):
    global _PF_DSE_HELPER_MODULE, _PF_DSE_HELPER_PATH
    helper_path = _resolve_pf_dse_helper(config_path)
    if _PF_DSE_HELPER_MODULE is not None and _PF_DSE_HELPER_PATH == helper_path:
        return _PF_DSE_HELPER_MODULE

    spec = importlib.util.spec_from_file_location('pf_dse_xs_gem5_pf_cfg', str(helper_path))
    if spec is None or spec.loader is None:
        raise RuntimeError('cannot import pf-dse helper: ' + str(helper_path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _PF_DSE_HELPER_MODULE = module
    _PF_DSE_HELPER_PATH = helper_path
    return module


def _get_pf_dse_prefetcher(cpu, cache_level, options):
    helper = _load_pf_dse_helper(options.pf_dse_config)
    prefetcher = helper.create_prefetcher(
        options.pf_dse_config,
        cache_level,
        cpu=cpu,
        options=options,
    )
    return NULL if prefetcher is None else prefetcher


def create_prefetcher(cpu, cache_level, options):
    prefetcher_attr = '{}_hwp_type'.format(cache_level)
    prefetcher_name = ''
    prefetcher = NULL
    pf_buffer_enabled = getattr(options, 'enable_pf_buffer', False)
    pf_dse_config_used = False
    if getattr(options, 'pf_dse_config', None) and cache_level in ('l1d', 'l2', 'l2_wrapper'):
        prefetcher = _get_pf_dse_prefetcher(cpu, cache_level, options)
        pf_dse_config_used = True
        if prefetcher != NULL:
            prefetcher_name = _prefetcher_type_name(prefetcher)
            print(f"create_prefetcher at {cache_level}: {prefetcher_name} (pf-dse config)")
    elif hasattr(options, prefetcher_attr):
        prefetcher_name = getattr(options, prefetcher_attr)
        prefetcher = _get_hwp(prefetcher_name)
        print(f"create_prefetcher at {cache_level}: {prefetcher_name}")

    if prefetcher != NULL and pf_buffer_enabled:
        if hasattr(prefetcher, 'use_pf_buffer'):
            prefetcher.use_pf_buffer = True

    if prefetcher == NULL:
        return NULL

    if not pf_dse_config_used and prefetcher_name == 'IPOPMultiPrefetcher':
        prefetcher.prefetchers = [
            BOPPrefetcher(is_sub_prefetcher=True),
            StridePrefetcher(is_sub_prefetcher=True),
        ]

    if not pf_dse_config_used and prefetcher_name == 'BanditPrefetcher':
        prefetcher.prefetchers = [
            BOPPrefetcher(is_sub_prefetcher=True),
            StridePrefetcher(is_sub_prefetcher=True),
        ]
        prefetcher.arm_masks = [1, 2, 3]

    if not pf_dse_config_used and prefetcher_name == 'SandboxMultiPrefetchers':
        prefetcher.prefetchers = [
            BOPPrefetcher(is_sub_prefetcher=True),
            StridePrefetcher(is_sub_prefetcher=True),
        ]

    if not pf_dse_config_used and prefetcher_name == "ReSemblePrefetcher":
        prefetcher.prefetchers = [
            BOPPrefetcher(is_sub_prefetcher=True),
            StridePrefetcher(is_sub_prefetcher=True),
        ]
        prefetcher.prediction_types = ["spatial", "temporal"]

    if not pf_dse_config_used and prefetcher_name == "AMDRegionStreamPrefetchers":
        prefetcher.stream_prefetcher = AMDContiguousStreamPrefetcher(
            is_sub_prefetcher=True
        )
        prefetcher.region_prefetcher = AMDRIPRegionPrefetcher(
            is_sub_prefetcher=True
        )

    if cpu != NULL:
        prefetcher.registerTLB(cpu.mmu.dtb, cpu.mmu.functional)

    if not pf_dse_config_used and prefetcher_name == 'XSCompositePrefetcher':
        if options.l1d_enable_spp:
            prefetcher.enable_spp = True
        if options.l1d_enable_cplx:
            prefetcher.enable_cplx = True
        prefetcher.pht_pf_level = 2 if options.kmh_align else options.pht_pf_level
        prefetcher.short_stride_thres = options.short_stride_thres
        prefetcher.enable_temporal = not options.kmh_align
        prefetcher.fuzzy_stride_matching = False
        prefetcher.stream_pf_ahead = True

        prefetcher.enable_bop = False
        prefetcher.bop_large.delay_queue_enable = True
        prefetcher.bop_large.bad_score = 10
        prefetcher.bop_small.delay_queue_enable = True
        prefetcher.bop_small.bad_score = 5

        prefetcher.queue_size = 128
        prefetcher.max_prefetch_requests_with_pending_translation = 128
        prefetcher.region_size = 64*16  # 64B * blocks per region

        prefetcher.berti.use_byte_addr = True
        prefetcher.berti.aggressive_pf = False
        prefetcher.berti.trigger_pht = True

        if options.ideal_cache:
            prefetcher.stream_pf_ahead = False
        if options.kmh_align:
            prefetcher.enable_berti = False
            prefetcher.enable_sstride = True
            prefetcher.enable_activepage = False
            prefetcher.enable_pht = True
            prefetcher.enable_xsstream = True
        if hasattr(prefetcher, 'prefetch_train'):
            prefetcher.prefetch_train = not pf_buffer_enabled
        if hasattr(prefetcher, 'queue_filter'):
            prefetcher.queue_filter = not pf_buffer_enabled

    if not pf_dse_config_used and cache_level == 'l2':
        if options.classic_l2:
            if hasattr(prefetcher, 'enable_bop'):
                prefetcher.enable_bop = True
            if options.kmh_align and prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
                prefetcher.enable_cmc = True
                prefetcher.enable_bop = True
                prefetcher.enable_cdp = False
                prefetcher.enable_despacito_stream = False
                prefetcher.bop_large = XSVirtualLargeBOP(is_sub_prefetcher=True,enable_adaptoffset=False)
                prefetcher.bop_small = XSPhysicalSmallBOP(is_sub_prefetcher=True,enable_adaptoffset=False)
            if hasattr(prefetcher, 'prefetch_train'):
                prefetcher.prefetch_train = not pf_buffer_enabled
            if hasattr(prefetcher, 'queue_filter'):
                prefetcher.queue_filter = not pf_buffer_enabled
            if options.l1_to_l2_pf_hint:
                _set_param_if_present(prefetcher, 'queue_size', 64)
                _set_param_if_present(
                    prefetcher,
                    'max_prefetch_requests_with_pending_translation',
                    128)
        else:
            assert prefetcher_name == 'PrefetcherForwarder'

    if not pf_dse_config_used and cache_level == 'l2_wrapper':
        if not options.classic_l2:
            if hasattr(prefetcher, 'enable_bop'):
                prefetcher.enable_bop = True
            if options.kmh_align and prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
                prefetcher.enable_cmc = True
                prefetcher.enable_bop = True
                prefetcher.enable_cdp = False
                prefetcher.enable_despacito_stream = False
                if prefetcher.enable_despacito_stream:
                    # if you want to check despacito pattern trace, set this to True
                    prefetcher.despacito_stream.enable_despacito_db = False
                prefetcher.bop_large = XSVirtualLargeBOP(is_sub_prefetcher=True,enable_adaptoffset=False)
                prefetcher.bop_small = XSPhysicalSmallBOP(is_sub_prefetcher=True,enable_adaptoffset=False)
            if hasattr(prefetcher, 'prefetch_train'):
                prefetcher.prefetch_train = not pf_buffer_enabled
            if hasattr(prefetcher, 'queue_filter'):
                prefetcher.queue_filter = not pf_buffer_enabled
            if options.l1_to_l2_pf_hint:
                _set_param_if_present(prefetcher, 'queue_size', 32)
                _set_param_if_present(
                    prefetcher,
                    'max_prefetch_requests_with_pending_translation',
                    128)

    if not pf_dse_config_used and cache_level == 'l3':
        if options.l2_to_l3_pf_hint:
            _set_param_if_present(prefetcher, 'queue_size', 64)
            _set_param_if_present(
                prefetcher,
                'max_prefetch_requests_with_pending_translation',
                128)

    # Propagate the pf-ahead master switches to every prefetcher (all levels).
    if getattr(options, 'no_pfahead', False) and hasattr(prefetcher, 'no_pfahead'):
        prefetcher.no_pfahead = True
    if getattr(options, 'no_pfahead_reserved', False) and \
            hasattr(prefetcher, 'no_pfahead_reserved'):
        prefetcher.no_pfahead_reserved = True

    return prefetcher
