"""Command-line configuration helpers for final-target H2P statistics."""


SUPPORTED_BP_TYPE = "DecoupledBPUWithBTB"


def add_h2p_stat_option(parser):
    parser.add_argument(
        "--h2p-stat",
        action="store_true",
        default=False,
        help=(
            "Record committed indirect branches whose final prediction did not "
            "provide the correct target"
        ),
    )

    parser.add_argument(
        "--h2p-stat-interval", type=int, default=0, metavar="N",
        help="Write H2P windows every N measured committed instructions (0 disables)",
    )


def validate_h2p_stat_config(args):
    """Return an error string when an enabled H2P configuration is invalid."""
    interval = getattr(args, "h2p_stat_interval", 0)
    if interval < 0:
        return "--h2p-stat-interval must be nonnegative"
    if interval and not getattr(args, "h2p_stat", False):
        return "--h2p-stat-interval requires --h2p-stat"
    if not getattr(args, "h2p_stat", False):
        return None

    if getattr(args, "num_cpus", 1) != 1:
        return "--h2p-stat currently supports exactly one CPU"

    bp_type = getattr(args, "bp_type", None)
    if bp_type != SUPPORTED_BP_TYPE:
        return (
            "--h2p-stat requires --bp-type=DecoupledBPUWithBTB "
            f"(got --bp-type={bp_type})"
        )

    return None
