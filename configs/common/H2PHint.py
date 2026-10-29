"""Configuration of finite and unlimited frozen H2P Hint predictors."""

from pathlib import Path


GEOMETRY = {
    "meta_entries": ("hintMetaEntries", 16),
    "cam_entries": ("hintCamEntries", 16),
    "context_entries": ("hintContextEntries", 4096),
    "ways": ("hintWays", 2),
    "slot_entries": ("hintSlotEntries", 32),
    "conf_queue_depth": ("hintConfQueueDepth", 32),
}


def add_h2p_hint_options(parser):
    parser.add_argument(
        "--h2p-hint-mode", choices=("off", "discover", "detail", "collect", "replay"), default="off"
    )
    parser.add_argument(
        "--h2p-hint-profile-context", choices=("prediction", "commit-pre"),
        default="prediction",
        help="Offline profiling history source; online queries stay speculative",
    )
    parser.add_argument(
        "--ideal-hint-ibp",
        action="store_true",
        help="Remove Hint table capacities; keep hash and timing",
    )
    parser.add_argument(
        "--h2p-hint-candidates",
        default="",
        help="Frozen 144B SPEC CAM image, or legacy candidate JSON",
    )
    parser.add_argument(
        "--h2p-hint-metadata",
        default="",
        help="Frozen 16784B SPEC metadata image, or legacy table JSON",
    )
    for name, (_, default) in GEOMETRY.items():
        parser.add_argument(
            "--h2p-hint-" + name.replace("_", "-"), type=int, default=default
        )


def validate_h2p_hint_config(args):
    if getattr(args, "h2p_hint_meta_entries", 16) not in (16, 32):
        return "Hint metadata queue supports 16 or 32 entries"
    mode = getattr(args, "h2p_hint_mode", "off")
    if mode not in ("off", "discover", "detail", "collect", "replay"):
        return "Invalid H2P Hint mode"
    if getattr(args, "h2p_hint_profile_context", "prediction") not in ("prediction", "commit-pre"):
        return "Hint profile context must be prediction or commit-pre"
    ideal = getattr(args, "ideal_hint_ibp", False)
    candidates = getattr(args, "h2p_hint_candidates", "")
    metadata = getattr(args, "h2p_hint_metadata", "")
    if mode == "off":
        if ideal:
            return (
                "--ideal-hint-ibp requires --h2p-hint-mode=collect or replay"
            )
        if candidates or metadata:
            return "Hint candidate/metadata files require --h2p-hint-mode"
        return None
    if getattr(args, "num_cpus", 1) != 1 or getattr(args, "smt", False):
        return "H2P Hint requires exactly one CPU and one hardware thread"
    if getattr(args, "bp_type", None) != "DecoupledBPUWithBTB":
        return "H2P Hint requires --bp-type=DecoupledBPUWithBTB"
    if getattr(args, "ideal_2_taken", False):
        return "H2P Hint does not support --ideal-2-taken"
    if mode in ("discover", "detail"):
        if ideal or getattr(args, "h2p_hint_profile_context", "prediction") != "prediction":
            return "SPEC profiling requires finite prediction snapshots"
        if not getattr(args, "h2p_stat", False):
            return "SPEC profiling requires --h2p-stat for exact measured phase control"
    if mode == "discover":
        if candidates or metadata:
            return "Discovery keeps Hint Off and does not accept input tables"
        return None
    if mode in ("detail", "collect") and not candidates:
        return "Hint Detail/collect requires --h2p-hint-candidates"
    if mode in ("detail", "collect") and metadata:
        return "Hint Detail/collect does not accept replay metadata"
    if mode == "replay" and not metadata:
        return "Hint replay requires --h2p-hint-metadata"
    for name, path in (("candidates", candidates), ("metadata", metadata)):
        if path and not Path(path).is_file():
            return f"Hint {name} file does not exist: {path}"
    values = {
        name: getattr(args, "h2p_hint_" + name, default)
        for name, (_, default) in GEOMETRY.items()
    }
    if any(value < 1 for value in values.values()):
        return "Hint capacities and confidence queue depth must be positive"
    if ideal:
        for name, (_, default) in GEOMETRY.items():
            if name != "conf_queue_depth" and values[name] != default:
                option = "--h2p-hint-" + name.replace("_", "-")
                return f"--ideal-hint-ibp does not accept changing {option}"
        return None
    if values["cam_entries"] != 16:
        return (
            "Finite v3 Hint requires exactly 16 CAM entries"
        )
    entries, ways = values["context_entries"], values["ways"]
    sets = entries // ways
    if (entries, ways, values["slot_entries"],
            values["conf_queue_depth"]) != (4096, 2, 32, 32):
        return "Finite v3 Hint requires 4096 Context, two ways, 32 Slots and a 32-entry confidence FIFO"
    return None


def h2p_hint_params(args):
    params = {
        "hintMode": getattr(args, "h2p_hint_mode", "off"),
        "hintProfileContext": getattr(args, "h2p_hint_profile_context", "prediction"),
        "idealHintIBP": getattr(args, "ideal_hint_ibp", False),
        "hintCandidates": getattr(args, "h2p_hint_candidates", ""),
        "hintMetadata": getattr(args, "h2p_hint_metadata", ""),
    }
    params.update(
        {
            param: getattr(args, "h2p_hint_" + name, default)
            for name, (param, default) in GEOMETRY.items()
        }
    )
    if getattr(args, "ideal_hint_ibp", False) and params["hintConfQueueDepth"] == 32:
        params["hintConfQueueDepth"] = 8
    return params
