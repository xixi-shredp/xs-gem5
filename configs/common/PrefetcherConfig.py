import importlib.util
import json
import os
from pathlib import Path

import m5
from m5.objects import *
from common.Caches import *
from common import ObjectList
from m5.objects.Prefetcher import XSPhysicalSmallBOP, XSVirtualLargeBOP


CENTRAL_PREFETCHER_COMMON_PARAMS = (
    "on_miss",
    "on_read",
    "on_write",
    "on_data",
    "on_inst",
    "prefetch_on_access",
    "prefetch_on_pf_hit",
    "use_virtual_addresses",
    "tag_prefetch",
    "latency",
    "queue_size",
    "queue_squash",
    "queue_filter",
    "cache_snoop",
    "throttle_control_percentage",
)

CENTRAL_PREFETCHER_RESERVED_KEYS = frozenset((
    "type",
    "name",
    "enabled",
    "enable",
    "pf_list",
    "prefetchers",
    "algorithms",
    "pfs_scheme",
    "scheme_param",
    "inherit_defaults",
    "central",
    "large",
    "small",
    "learned",
    "children",
    "stream_prefetcher",
    "region_prefetcher",
    "prediction_types",
    "arm_masks",
))


def _get_hwp(hwp_option):
    if hwp_option is None:
        return NULL
    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()

def _prefetcher_type_name(prefetcher):
    return str(getattr(prefetcher, 'type', prefetcher.__class__.__name__))


_PF_DSE_HELPER_MODULE = None
_PF_DSE_HELPER_PATH = None


def _resolve_pf_dse_helper(config_path):
    cfg_path = Path(config_path).expanduser().resolve()
    candidates = []
    if cfg_path.parent.name == 'configs':
        candidates.append(
            cfg_path.parent.parent / 'gem5_py' / 'get-xs-gem5-pf-cfg.py')

    pf_dse_root = os.environ.get('PF_DSE_ROOT')
    if pf_dse_root:
        candidates.append(
            Path(pf_dse_root).expanduser() / 'gem5_py' /
            'get-xs-gem5-pf-cfg.py')

    candidates.append(
        Path.home() / 'sim' / 'workbench' / 'pf-dse' / 'gem5_py' /
        'get-xs-gem5-pf-cfg.py')
    candidates.append(
        Path.home() / 'xs-env' / 'pf-dse' / 'gem5_py' /
        'get-xs-gem5-pf-cfg.py')

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    searched = ', '.join(str(candidate) for candidate in candidates)
    raise RuntimeError(
        'cannot locate pf-dse helper get-xs-gem5-pf-cfg.py; searched: ' +
        searched)


def _load_pf_dse_helper(config_path):
    global _PF_DSE_HELPER_MODULE, _PF_DSE_HELPER_PATH
    helper_path = _resolve_pf_dse_helper(config_path)
    if (_PF_DSE_HELPER_MODULE is not None and
            _PF_DSE_HELPER_PATH == helper_path):
        return _PF_DSE_HELPER_MODULE

    spec = importlib.util.spec_from_file_location(
        'pf_dse_xs_gem5_pf_cfg', str(helper_path))
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


def is_pf_buffer_enabled(options):
    # Enabled by default; --disable-pf-buffer is the only CLI override.
    return getattr(options, 'enable_pf_buffer', True)

def _configure_pf_buffer(prefetcher, pf_buffer_enabled):
    if prefetcher != NULL and hasattr(prefetcher, 'use_pf_buffer'):
        prefetcher.use_pf_buffer = pf_buffer_enabled

def _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled):
    # These controls are disabled when the pf-buffer handles filtering.
    if hasattr(prefetcher, 'prefetch_train'):
        prefetcher.prefetch_train = not pf_buffer_enabled
    if hasattr(prefetcher, 'queue_filter'):
        prefetcher.queue_filter = not pf_buffer_enabled

def _register_prefetcher_tlb(prefetcher, cpu):
    if cpu != NULL:
        prefetcher.registerTLB(cpu.mmu.dtb, cpu.mmu.functional)

def _load_central_prefetcher_config(config_path):
    if not config_path:
        return None
    expanded = os.path.expanduser(config_path)
    with open(expanded, "r", encoding="utf-8") as fp:
        config = json.load(fp)
    if not isinstance(config, dict):
        m5.fatal("centralized prefetcher config must be a JSON object")
    # pf-dse has per-level keys such as l1d_pf/l2_pf.  The centralized
    # framework owns one algorithm set, so accept only a root config or an
    # explicit centralized_pf wrapper, never l1/l2 split keys.
    if "centralized_pf" in config:
        config = config["centralized_pf"]
    for level_key in ("l1d_pf", "l2_pf", "l3_pf"):
        if level_key in config:
            m5.fatal("centralized prefetcher JSON must not use %s", level_key)
    return config


def _apply_simobject_params(obj, config, reserved=CENTRAL_PREFETCHER_RESERVED_KEYS):
    for key, value in config.items():
        if key in reserved:
            continue
        if hasattr(obj, key):
            setattr(obj, key, value)
    return obj


def _inherit_central_common_params(parent_config, child_config):
    merged = dict(child_config)
    for param in CENTRAL_PREFETCHER_COMMON_PARAMS:
        if param in parent_config and param not in merged:
            merged[param] = parent_config[param]
    return merged


def _central_prefetcher_type(config):
    pf_type = config.get("type", config.get("name", "none"))
    return str(pf_type).replace("-", "_").lower()


CENTRAL_IMPORTED_PREFETCHER_TYPES = {
    "multi": "MultiPrefetcher",
    "multiprefetcher": "MultiPrefetcher",
    "ipopmulti": "IPOPMultiPrefetcher",
    "ipop_multi": "IPOPMultiPrefetcher",
    "ipopmultiprefetcher": "IPOPMultiPrefetcher",
    "bandit": "BanditPrefetcher",
    "banditprefetcher": "BanditPrefetcher",
    "sandbox": "SandboxMultiPrefetchers",
    "sandboxmulti": "SandboxMultiPrefetchers",
    "sandbox_multi": "SandboxMultiPrefetchers",
    "sandboxmultiprefetchers": "SandboxMultiPrefetchers",
    "resemble": "ReSemblePrefetcher",
    "resembleprefetcher": "ReSemblePrefetcher",
    "amd_contiguous_stream": "AMDContiguousStreamPrefetcher",
    "amdcontiguousstream": "AMDContiguousStreamPrefetcher",
    "amdcontiguousstreamprefetcher": "AMDContiguousStreamPrefetcher",
    "amd_rip_region": "AMDRIPRegionPrefetcher",
    "amdripregion": "AMDRIPRegionPrefetcher",
    "amdripregionprefetcher": "AMDRIPRegionPrefetcher",
    "amd_region_stream": "AMDRegionStreamPrefetchers",
    "amdregionstream": "AMDRegionStreamPrefetchers",
    "amdregionstreamprefetchers": "AMDRegionStreamPrefetchers",
    "amd_region_type": "AMDRegionTypePrefetcher",
    "amdregiontype": "AMDRegionTypePrefetcher",
    "amdregiontypeprefetcher": "AMDRegionTypePrefetcher",
    "amd_aop": "AMDAOPPrefetcher",
    "amdaop": "AMDAOPPrefetcher",
    "amdaopprefetcher": "AMDAOPPrefetcher",
    "apple_ampm": "AppleAMPMPrefetcher",
    "appleampm": "AppleAMPMPrefetcher",
    "appleampmprefetcher": "AppleAMPMPrefetcher",
    "arm_hint": "ARMHintPrefetcher",
    "armhint": "ARMHintPrefetcher",
    "armhintprefetcher": "ARMHintPrefetcher",
    "arm_offset_based_pointer": "ARMOffsetBasedPointerPrefetcher",
    "armoffsetbasedpointer": "ARMOffsetBasedPointerPrefetcher",
    "armoffsetbasedpointerprefetcher": "ARMOffsetBasedPointerPrefetcher",
    "dspatch": "DSPatchPrefetcher",
    "dspatchprefetcher": "DSPatchPrefetcher",
    "pattern_merging": "PatternMergingPrefetcher",
    "patternmerging": "PatternMergingPrefetcher",
    "patternmergingprefetcher": "PatternMergingPrefetcher",
    "kairos": "KairosPrefetcher",
    "kairosprefetcher": "KairosPrefetcher",
    "streamline": "StreamlinePrefetcher",
    "streamlineprefetcher": "StreamlinePrefetcher",
    "small_bop": "SmallBOPPrefetcher",
    "smallbop": "SmallBOPPrefetcher",
    "smallbopprefetcher": "SmallBOPPrefetcher",
    "despacito_stream": "DespacitoStreamPrefetcher",
    "despacitostream": "DespacitoStreamPrefetcher",
    "despacitostreamprefetcher": "DespacitoStreamPrefetcher",
    "bingo": "BingoPrefetcher",
    "bingoprefetcher": "BingoPrefetcher",
    "l2_worker_slot": "L2WorkerSlotPrefetcher",
    "l2workerslot": "L2WorkerSlotPrefetcher",
    "l2workerslotprefetcher": "L2WorkerSlotPrefetcher",
}

CENTRAL_NORMAL_PREFETCHER_TYPES = {
    "BOPPrefetcher",
    "StridePrefetcher",
    "XSStridePrefetcher",
    "XsStreamPrefetcher",
    "CMCPrefetcher",
    "IPCPrefetcher",
    "SignaturePathPrefetcher",
    "BertiPrefetcher",
    "OptPrefetcher",
}


def _central_prefetcher_class_name(config):
    pf_type = _central_prefetcher_type(config)
    imported = CENTRAL_IMPORTED_PREFETCHER_TYPES.get(pf_type)
    if imported:
        return imported
    raw = str(config.get("type", config.get("name", "")))
    if raw in CENTRAL_NORMAL_PREFETCHER_TYPES:
        return raw
    return None


def _default_central_children_for(class_name):
    if class_name in ("IPOPMultiPrefetcher", "BanditPrefetcher",
                      "SandboxMultiPrefetchers", "ReSemblePrefetcher"):
        return [
            {"type": "BOPPrefetcher"},
            {"type": "StridePrefetcher"},
        ]
    return []


def _new_central_managed_prefetcher(config):
    canonical = _canonical_central_algorithm(_central_prefetcher_type(config))
    if canonical is not None and canonical not in ("none", "pht",
                                                   "activepage", "bop"):
        return _new_central_child(canonical, config)

    class_name = _central_prefetcher_class_name(config)
    if class_name is None:
        return None
    cls = globals().get(class_name)
    if cls is None:
        m5.fatal("centralized prefetcher type %s is not available",
                 class_name)
    child = cls()
    if hasattr(child, "is_sub_prefetcher"):
        child.is_sub_prefetcher = True

    child_cfgs = config.get("children", config.get("prefetchers", None))
    if child_cfgs is None:
        child_cfgs = _default_central_children_for(class_name)
    if child_cfgs:
        if not isinstance(child_cfgs, list):
            m5.fatal("centralized manager children/prefetchers must be a list")
        child.prefetchers = [
            _new_central_managed_prefetcher(dict(child_cfg))
            for child_cfg in child_cfgs
        ]
    if (class_name == "ReSemblePrefetcher" and child_cfgs and
            "prediction_types" not in config):
        child.prediction_types = [
            str(child_cfg.get("prediction_type", child_cfg.get("name",
                child_cfg.get("type", "child"))))
            for child_cfg in child_cfgs
        ]
    if (class_name == "BanditPrefetcher" and child_cfgs and
            "arm_masks" not in config):
        child.arm_masks = [(1 << i) for i in range(len(child_cfgs))]
    if class_name == "AMDRegionStreamPrefetchers":
        if "stream_prefetcher" in config:
            child.stream_prefetcher = _new_central_managed_prefetcher(
                dict(config["stream_prefetcher"]))
        if "region_prefetcher" in config:
            child.region_prefetcher = _new_central_managed_prefetcher(
                dict(config["region_prefetcher"]))

    return _apply_simobject_params(child, config)


def _new_central_child(canonical, config):
    if canonical == "bop_large":
        child = XSVirtualLargeBOP(is_sub_prefetcher=True,
                                  enable_adaptoffset=False)
    elif canonical == "bop_small":
        child = XSPhysicalSmallBOP(is_sub_prefetcher=True,
                                   enable_adaptoffset=False)
    elif canonical == "bop_learned":
        child = LearnedBOPPrefetcher(is_sub_prefetcher=True)
    elif canonical == "spp":
        child = SignaturePathPrefetcher(is_sub_prefetcher=True)
    elif canonical == "ipcp":
        child = IPCPrefetcher(use_rrf=False, is_sub_prefetcher=True)
    elif canonical == "cmc":
        child = CMCPrefetcher(is_sub_prefetcher=True)
    elif canonical == "berti":
        child = BertiPrefetcher(is_sub_prefetcher=True)
    elif canonical == "sstride":
        child = XSStridePrefetcher(is_sub_prefetcher=True)
    elif canonical == "opt":
        child = OptPrefetcher(is_sub_prefetcher=True)
    elif canonical == "xsstream":
        child = XsStreamPrefetcher(is_sub_prefetcher=True)
    else:
        return None
    return _apply_simobject_params(child, config)


def _canonical_central_algorithm(pf_type):
    aliases = {
        "none": "none",
        "off": "none",
        "pht": "pht",
        "sms_pht": "pht",
        "activepage": "activepage",
        "active_page": "activepage",
        "sstride": "sstride",
        "xsstride": "sstride",
        "xs_stride": "sstride",
        "stride": "sstride",
        "xsstream": "xsstream",
        "xs_stream": "xsstream",
        "stream": "xsstream",
        "bop": "bop",
        "bop_large": "bop_large",
        "large_bop": "bop_large",
        "bop_small": "bop_small",
        "small_bop": "bop_small",
        "bop_learned": "bop_learned",
        "learned_bop": "bop_learned",
        "cmc": "cmc",
        "temporal": "cmc",
        "ipcp": "ipcp",
        "cplx": "ipcp",
        "ipcp_cplx": "ipcp",
        "spp": "spp",
        "signature_path": "spp",
        "berti": "berti",
        "opt": "opt",
    }
    return aliases.get(pf_type)


def _set_central_algorithm_defaults(prefetcher, enabled):
    prefetcher.enable_activepage = enabled
    prefetcher.enable_pht = enabled
    prefetcher.enable_sstride = enabled
    prefetcher.enable_xsstream = enabled
    prefetcher.enable_bop = enabled
    prefetcher.enable_temporal = enabled
    prefetcher.enable_berti = False
    prefetcher.enable_cplx = False
    prefetcher.enable_spp = False
    prefetcher.enable_opt = False


def _apply_bop_group(prefetcher, config):
    prefetcher.enable_bop = True
    common = {k: v for k, v in config.items()
              if k not in CENTRAL_PREFETCHER_RESERVED_KEYS}
    if common:
        _apply_simobject_params(prefetcher.bop_large, common, reserved=())
        _apply_simobject_params(prefetcher.bop_small, common, reserved=())
    if "large" in config:
        large_cfg = dict(config["large"])
        large_cfg.setdefault("type", "bop_large")
        prefetcher.bop_large = _new_central_child("bop_large", large_cfg)
    if "small" in config:
        small_cfg = dict(config["small"])
        small_cfg.setdefault("type", "bop_small")
        prefetcher.bop_small = _new_central_child("bop_small", small_cfg)
    if "learned" in config:
        learned_cfg = dict(config["learned"])
        learned_cfg.setdefault("type", "bop_learned")
        prefetcher.bop_learned = _new_central_child("bop_learned", learned_cfg)


def _apply_central_algorithm(prefetcher, config, managed_names):
    if not config.get("enabled", config.get("enable", True)):
        return
    canonical = _canonical_central_algorithm(_central_prefetcher_type(config))
    if canonical is None:
        managed_child = _new_central_managed_prefetcher(config)
        if managed_child is None:
            m5.fatal("unknown centralized prefetch algorithm type: %s",
                     config.get("type", config.get("name", "")))
        prefetcher.managed_prefetchers.append(managed_child)
        managed_names.append(config.get("name",
                            _prefetcher_type_name(managed_child)))
        return
    if canonical == "none":
        return
    managed_names.append(config.get("name", canonical))
    if canonical == "pht":
        prefetcher.enable_pht = True
        _apply_simobject_params(prefetcher, config)
    elif canonical == "activepage":
        prefetcher.enable_activepage = True
        _apply_simobject_params(prefetcher, config)
    elif canonical == "bop":
        _apply_bop_group(prefetcher, config)
    elif canonical == "bop_large":
        prefetcher.enable_bop = True
        prefetcher.bop_large = _new_central_child(canonical, config)
    elif canonical == "bop_small":
        prefetcher.enable_bop = True
        prefetcher.bop_small = _new_central_child(canonical, config)
    elif canonical == "bop_learned":
        prefetcher.bop_learned = _new_central_child(canonical, config)
    elif canonical in ("cmc", "ipcp", "sstride", "xsstream",
                       "berti", "spp", "opt", "bop_learned"):
        # Under the centralized-only contract, individually configured
        # algorithms are internal candidate generators. The built-in
        # XSComposite defaults are used only when inherit_defaults enables
        # the legacy fixed composition.
        child = _new_central_managed_prefetcher(config)
        if child is None:
            child = _new_central_child(canonical, config)
        prefetcher.managed_prefetchers.append(child)


def _central_pf_list(config):
    pf_list = config.get("pf_list", config.get("prefetchers",
                   config.get("algorithms", [])))
    if isinstance(pf_list, dict):
        return [pf_list]
    if isinstance(pf_list, list):
        return pf_list
    m5.fatal("centralized prefetcher pf_list/prefetchers must be a list or object")


def _configure_centralized_manager(prefetcher, config_path):
    config = _load_central_prefetcher_config(config_path)
    if config is None:
        return
    scheme = config.get("pfs_scheme", "all")
    if scheme not in ("all", "central", "none"):
        m5.fatal("unsupported centralized prefetch manager scheme: %s", scheme)
    _apply_simobject_params(prefetcher, config)
    if hasattr(prefetcher, "managed_prefetchers"):
        prefetcher.managed_prefetchers = []
    central_params = config.get("central", {})
    if central_params:
        _apply_simobject_params(prefetcher, central_params)
    inherit_defaults = bool(config.get("inherit_defaults", False))
    _set_central_algorithm_defaults(prefetcher, inherit_defaults)
    managed_names = []
    for child in _central_pf_list(config):
        if not isinstance(child, dict):
            m5.fatal("centralized prefetcher child config must be an object")
        child_config = _inherit_central_common_params(config, child)
        _apply_central_algorithm(prefetcher, child_config, managed_names)
    if prefetcher.enable_berti and prefetcher.enable_sstride:
        m5.fatal("centralized prefetcher cannot enable berti and sstride together")
    if not managed_names and not inherit_defaults:
        print("warning: centralized prefetcher JSON enabled no algorithms")
    if hasattr(prefetcher, "managed_prefetcher_names"):
        prefetcher.managed_prefetcher_names = managed_names


def create_centralized_endpoint(cache_level, min_mshr_credits=1):
    return CentralizedPrefetcherEndpoint(
        cache_level=cache_level,
        queue_size=32,
        per_core_queue_size=8,
        arbitration_width=1,
        min_mshr_credits=min_mshr_credits,
    )

def create_centralized_engine(cpu, core_id, l2_endpoint, l3_endpoint=NULL,
                              l1_min_mshr_credits=1,
                              dynamic_arbitration=False,
                              quality_table_entries=4096,
                              quality_table_assoc=4,
                              quality_initial_score=8,
                              quality_max_score=15,
                              quality_l1_threshold=8,
                              quality_l2_threshold=4,
                              quality_drop_threshold=1,
                              quality_useful_weight=3,
                              quality_unused_weight=-6,
                              quality_late_weight=1,
                              quality_duplicate_demand_weight=0,
                              quality_hotness_max=31,
                              quality_hotness_observation_weight=1,
                              quality_hotness_useful_weight=4,
                              quality_hotness_late_weight=2,
                              quality_hotness_decay_period=2048,
                              quality_hotness_l1_threshold=12,
                              quality_hotness_l2_threshold=6,
                              cmc_near_distance=32,
                              cmc_far_l1_threshold=12,
                              l1_pollution_threshold=16,
                              l1_pollution_bypass_threshold=12,
                              l1_pollution_unused_weight=4,
                              l1_pollution_useful_weight=-8,
                              l1_pollution_decay=1,
                              duplicate_filter_entries=4096,
                              manager_config=None):
    prefetcher = CentralizedDataPrefetcher(
        core_id=core_id,
        l2_endpoint=l2_endpoint,
        l3_endpoint=l3_endpoint,
        central_queue_size=64,
        dispatch_width=1,
        l1_distance_threshold=32,
        l2_distance_threshold=128,
        l1_min_mshr_credits=l1_min_mshr_credits,
        dynamic_arbitration=dynamic_arbitration,
        quality_table_entries=quality_table_entries,
        quality_table_assoc=quality_table_assoc,
        quality_initial_score=quality_initial_score,
        quality_max_score=quality_max_score,
        quality_l1_threshold=quality_l1_threshold,
        quality_l2_threshold=quality_l2_threshold,
        quality_drop_threshold=quality_drop_threshold,
        quality_useful_weight=quality_useful_weight,
        quality_unused_weight=quality_unused_weight,
        quality_late_weight=quality_late_weight,
        quality_duplicate_demand_weight=quality_duplicate_demand_weight,
        quality_hotness_max=quality_hotness_max,
        quality_hotness_observation_weight=quality_hotness_observation_weight,
        quality_hotness_useful_weight=quality_hotness_useful_weight,
        quality_hotness_late_weight=quality_hotness_late_weight,
        quality_hotness_decay_period=quality_hotness_decay_period,
        quality_hotness_l1_threshold=quality_hotness_l1_threshold,
        quality_hotness_l2_threshold=quality_hotness_l2_threshold,
        cmc_near_distance=cmc_near_distance,
        cmc_far_l1_threshold=cmc_far_l1_threshold,
        l1_pollution_threshold=l1_pollution_threshold,
        l1_pollution_bypass_threshold=l1_pollution_bypass_threshold,
        l1_pollution_unused_weight=l1_pollution_unused_weight,
        l1_pollution_useful_weight=l1_pollution_useful_weight,
        l1_pollution_decay=l1_pollution_decay,
        duplicate_filter_entries=duplicate_filter_entries,
        train_on_store=False,
    )
    _configure_centralized_manager(prefetcher, manager_config)
    _register_prefetcher_tlb(prefetcher, cpu)
    return prefetcher

def configure_centralized_prefetch_translation(cpu):
    cpu.mmu.dtb.data_prefetch_pte_buffer_size = 16
    cpu.mmu.dtb.walker.enable_data_prefetch_ptw_throttle = True
    cpu.mmu.dtb.walker.ptw_demand_reserve = 4

def _configure_xs_composite_common(prefetcher, options):
    # Keep only option/profile-dependent overrides here. Stable model defaults
    # belong to XSCompositePrefetcher in Prefetcher.py.
    prefetcher.short_stride_thres = getattr(options, "short_stride_thres", 0)

    if options.ideal_cache:
        prefetcher.stream_pf_ahead = False

def _configure_xs_composite_default(prefetcher, options):
    prefetcher.enable_activepage = True
    prefetcher.enable_pht = True
    prefetcher.enable_berti = True
    prefetcher.enable_bop = False
    prefetcher.enable_temporal = True
    prefetcher.enable_sstride = False
    prefetcher.enable_xsstream = False
    prefetcher.enable_opt = False
    prefetcher.pht_pf_level = options.pht_pf_level

def _configure_xs_composite_kmh_align(prefetcher):
    prefetcher.enable_activepage = False
    prefetcher.enable_pht = True
    prefetcher.enable_berti = False
    prefetcher.enable_bop = False
    prefetcher.enable_temporal = False
    prefetcher.enable_sstride = True
    prefetcher.enable_xsstream = True
    prefetcher.enable_opt = False
    prefetcher.pht_pf_level = 2

def _configure_xs_composite(prefetcher, options, pf_buffer_enabled):
    _configure_xs_composite_common(prefetcher, options)

    # Start from the selected XSComposite profile, then apply explicit overrides.
    if options.kmh_align:
        _configure_xs_composite_kmh_align(prefetcher)
    else:
        _configure_xs_composite_default(prefetcher, options)

    if options.l1d_enable_spp:
        prefetcher.enable_spp = True
    if options.l1d_enable_cplx:
        prefetcher.enable_cplx = True

    _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled)

def _configure_l2_composite_default(prefetcher):
    # Normal L2CompositeWithWorker profile.
    prefetcher.enable_bop = True
    prefetcher.enable_cdp = True
    prefetcher.enable_cmc = False
    prefetcher.enable_despacito_stream = True

def _configure_l2_composite_kmh_align(prefetcher):
    # RTL-aligned L2CompositeWithWorker profile.
    prefetcher.enable_cmc = True
    prefetcher.enable_bop = True
    prefetcher.enable_cdp = False
    prefetcher.enable_despacito_stream = False
    prefetcher.bop_large = XSVirtualLargeBOP(is_sub_prefetcher=True,
                                             enable_adaptoffset=False)
    prefetcher.bop_small = XSPhysicalSmallBOP(is_sub_prefetcher=True,
                                              enable_adaptoffset=False)

def _configure_l2_composite(prefetcher, prefetcher_name, options):
    if options.kmh_align:
        assert prefetcher_name == 'L2CompositeWithWorkerPrefetcher'
        _configure_l2_composite_kmh_align(prefetcher)
    elif prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
        _configure_l2_composite_default(prefetcher)

def _configure_l2_prefetcher(prefetcher, prefetcher_name, options,
                             pf_buffer_enabled):
    # classic_l2 attaches the real L2 prefetcher directly to the L2 cache.
    # Aligned L2 uses this level only as a forwarder to l2_wrapper.
    if options.classic_l2:
        _configure_l2_composite(prefetcher, prefetcher_name, options)
        _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled)
        if options.l1_to_l2_pf_hint:
            prefetcher.queue_size = 64
            prefetcher.max_prefetch_requests_with_pending_translation = 128
    else:
        assert prefetcher_name == 'PrefetcherForwarder'

def _configure_l2_wrapper_prefetcher(prefetcher, prefetcher_name, options,
                                     pf_buffer_enabled):
    # Aligned L2 attaches the real L2 prefetcher to l2_wrapper.
    # Classic L2 has no wrapper-level real prefetcher.
    if not options.classic_l2:
        _configure_l2_composite(prefetcher, prefetcher_name, options)
        _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled)
        if options.l1_to_l2_pf_hint:
            prefetcher.queue_size = 32
            prefetcher.max_prefetch_requests_with_pending_translation = 128

def _configure_l3_prefetcher(prefetcher, options):
    if options.l2_to_l3_pf_hint:
        prefetcher.queue_size = 64
        prefetcher.max_prefetch_requests_with_pending_translation = 128

def create_prefetcher(cpu, cache_level, options):
    prefetcher_attr = '{}_hwp_type'.format(cache_level)
    prefetcher_name = ''
    prefetcher = NULL
    pf_buffer_enabled = is_pf_buffer_enabled(options)
    pf_dse_config_used = False
    if (getattr(options, 'pf_dse_config', None) and
            cache_level in ('l1d', 'l2', 'l2_wrapper')):
        prefetcher = _get_pf_dse_prefetcher(cpu, cache_level, options)
        pf_dse_config_used = True
        if prefetcher != NULL:
            prefetcher_name = _prefetcher_type_name(prefetcher)
            print(f"create_prefetcher at {cache_level}: "
                  f"{prefetcher_name} (pf-dse config)")
    elif hasattr(options, prefetcher_attr):
        prefetcher_name = getattr(options, prefetcher_attr)
        prefetcher = _get_hwp(prefetcher_name)
        print(f"create_prefetcher at {cache_level}: {prefetcher_name}")

    _configure_pf_buffer(prefetcher, pf_buffer_enabled)

    if prefetcher == NULL:
        return NULL

    centralized_only_types = set(CENTRAL_IMPORTED_PREFETCHER_TYPES.values())
    if prefetcher_name in centralized_only_types:
        m5.fatal("%s must be configured inside --centralized-prefetcher-config; "
                 "ordinary cache-level manager/algorithm placement is disabled",
                 prefetcher_name)

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

    if not pf_dse_config_used and prefetcher_name == 'ReSemblePrefetcher':
        prefetcher.prefetchers = [
            BOPPrefetcher(is_sub_prefetcher=True),
            StridePrefetcher(is_sub_prefetcher=True),
        ]
        prefetcher.prediction_types = ['spatial', 'temporal']

    if not pf_dse_config_used and prefetcher_name == 'AMDRegionStreamPrefetchers':
        prefetcher.stream_prefetcher = AMDContiguousStreamPrefetcher(
            is_sub_prefetcher=True)
        prefetcher.region_prefetcher = AMDRIPRegionPrefetcher(
            is_sub_prefetcher=True)

    _register_prefetcher_tlb(prefetcher, cpu)

    if not pf_dse_config_used and prefetcher_name == 'XSCompositePrefetcher':
        _configure_xs_composite(prefetcher, options, pf_buffer_enabled)

    if not pf_dse_config_used and cache_level == 'l2':
        _configure_l2_prefetcher(prefetcher, prefetcher_name, options,
                                 pf_buffer_enabled)

    if not pf_dse_config_used and cache_level == 'l2_wrapper':
        _configure_l2_wrapper_prefetcher(prefetcher, prefetcher_name, options,
                                         pf_buffer_enabled)

    if not pf_dse_config_used and cache_level == 'l3':
        _configure_l3_prefetcher(prefetcher, options)

    if getattr(options, 'no_pfahead', False) and hasattr(prefetcher, 'no_pfahead'):
        prefetcher.no_pfahead = True
    if (getattr(options, 'no_pfahead_reserved', False) and
            hasattr(prefetcher, 'no_pfahead_reserved')):
        prefetcher.no_pfahead_reserved = True

    return prefetcher
