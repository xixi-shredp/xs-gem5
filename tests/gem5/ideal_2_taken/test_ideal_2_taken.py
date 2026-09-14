import re

from testlib import *


config_file = joinpath(absdirpath(__file__), "config_smoke.py")

gem5_verify_config(
    name="ideal-2-taken-default-disabled",
    fixtures=(),
    verifiers=(verifier.MatchRegex(re.compile(r"idealTwoTaken=false")),),
    config=config_file,
    config_args=(),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
)

gem5_verify_config(
    name="ideal-2-taken-explicit-enabled",
    fixtures=(),
    verifiers=(verifier.MatchRegex(re.compile(r"idealTwoTaken=true")),),
    config=config_file,
    config_args=("--ideal-2-taken",),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
)
