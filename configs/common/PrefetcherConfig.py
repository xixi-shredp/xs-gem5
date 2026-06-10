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

def create_prefetcher(cpu, cache_level, options):
    prefetcher_attr = '{}_hwp_type'.format(cache_level)
    prefetcher_name = ''
    prefetcher = NULL
    pf_buffer_enabled = getattr(options, 'enable_pf_buffer', False)
    if hasattr(options, prefetcher_attr):
        prefetcher_name = getattr(options, prefetcher_attr)
        prefetcher = _get_hwp(prefetcher_name)
        print(f"create_prefetcher at {cache_level}: {prefetcher_name}")

    if prefetcher != NULL and pf_buffer_enabled:
        if hasattr(prefetcher, 'use_pf_buffer'):
            prefetcher.use_pf_buffer = True

    if prefetcher == NULL:
        return NULL

    if prefetcher_name == 'IPOPMultiPrefetcher':
        prefetcher.prefetchers = [
            BOPPrefetcher(is_sub_prefetcher=True),
            StridePrefetcher(is_sub_prefetcher=True),
        ]

    if cpu != NULL:
        prefetcher.registerTLB(cpu.mmu.dtb, cpu.mmu.functional)

    if prefetcher_name == 'XSCompositePrefetcher':
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

    if cache_level == 'l2':
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

    if cache_level == 'l2_wrapper':
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

    if cache_level == 'l3':
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
