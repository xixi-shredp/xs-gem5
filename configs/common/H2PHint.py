"""Configuration of the finite, frozen H2P Hint predictor."""

from pathlib import Path


GEOMETRY = {
    "cam_entries": ("hintCamEntries", 16),
    "context_entries": ("hintContextEntries", 4096),
    "ways": ("hintWays", 2),
    "slot_entries": ("hintSlotEntries", 32),
    "conf_queue_depth": ("hintConfQueueDepth", 8),
}


def add_h2p_hint_options(parser):
    parser.add_argument("--h2p-hint-mode", choices=("off", "collect", "replay"),
                        default="off")
    parser.add_argument("--h2p-hint-candidates", default="",
                        help="Version 2 ranked candidate JSON (at most 20 PCs)")
    parser.add_argument("--h2p-hint-metadata", default="",
                        help="Version 2 frozen table JSON for replay")
    for name, (_, default) in GEOMETRY.items():
        parser.add_argument("--h2p-hint-" + name.replace("_", "-"),
                            type=int, default=default)


def validate_h2p_hint_config(args):
    mode = getattr(args, "h2p_hint_mode", "off")
    candidates = getattr(args, "h2p_hint_candidates", "")
    metadata = getattr(args, "h2p_hint_metadata", "")
    if mode == "off":
        if candidates or metadata:
            return "Hint candidate/metadata files require --h2p-hint-mode"
        return None
    if getattr(args, "num_cpus", 1) != 1 or getattr(args, "smt", False):
        return "H2P Hint requires exactly one CPU and one hardware thread"
    if getattr(args, "bp_type", None) != "DecoupledBPUWithBTB":
        return "H2P Hint requires --bp-type=DecoupledBPUWithBTB"
    if getattr(args, "ideal_2_taken", False):
        return "H2P Hint does not support --ideal-2-taken"
    if mode == "collect" and not candidates:
        return "Hint collect requires --h2p-hint-candidates"
    if mode == "collect" and metadata:
        return "Hint collect does not accept replay metadata"
    if mode == "replay" and not metadata:
        return "Hint replay requires --h2p-hint-metadata"
    for name, path in (("candidates", candidates), ("metadata", metadata)):
        if path and not Path(path).is_file():
            return f"Hint {name} file does not exist: {path}"
    values = {name: getattr(args, "h2p_hint_" + name, default)
              for name, (_, default) in GEOMETRY.items()}
    if any(value < 1 for value in values.values()):
        return "Hint capacities and confidence queue depth must be positive"
    if values["cam_entries"] > 20:
        return "Hint CAM capacity cannot exceed the 20-candidate profiling limit"
    entries, ways = values["context_entries"], values["ways"]
    sets = entries // ways
    if entries % ways or not sets or sets & (sets - 1) or sets >= 1 << 28:
        return "Hint context entries / ways must be a power of two below 2^28"
    return None


def h2p_hint_params(args):
    params = {
        "hintMode": getattr(args, "h2p_hint_mode", "off"),
        "hintCandidates": getattr(args, "h2p_hint_candidates", ""),
        "hintMetadata": getattr(args, "h2p_hint_metadata", ""),
    }
    params.update({param: getattr(args, "h2p_hint_" + name, default)
                   for name, (param, default) in GEOMETRY.items()})
    return params
