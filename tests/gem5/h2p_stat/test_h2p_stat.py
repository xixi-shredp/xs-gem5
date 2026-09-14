import re

from testlib import *


config_file = joinpath(absdirpath(__file__), "config_smoke.py")

gem5_verify_config(
    name="h2p-stat-default-disabled",
    fixtures=(),
    verifiers=(verifier.MatchRegex(re.compile(r"h2pStat=false")),),
    config=config_file,
    config_args=(),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
)

gem5_verify_config(
    name="h2p-stat-explicit-enabled",
    fixtures=(),
    verifiers=(verifier.MatchRegex(re.compile(r"h2pStat=true")),),
    config=config_file,
    config_args=("--h2p-stat",),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
)

gem5_verify_config(
    name="h2p-stat-rejects-multicore",
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(
            re.compile(r"h2pConfigRejected=.*supports exactly one CPU")
        ),
    ),
    config=config_file,
    config_args=("--h2p-stat", "--num-cpus", "2", "--expect-rejection"),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
)

gem5_verify_config(
    name="h2p-stat-rejects-other-predictor",
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(
            re.compile(r"h2pConfigRejected=.*requires.*DecoupledBPUWithBTB")
        ),
    ),
    config=config_file,
    config_args=(
        "--h2p-stat",
        "--bp-type",
        "LocalBP",
        "--expect-rejection",
    ),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
)
