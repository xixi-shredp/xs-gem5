"""TEST_ONLY: final-binary driver for production Hint helpers."""
from m5.SimObject import SimObject
from m5.params import Param


class HintIndirectTest(SimObject):
    type = "HintIndirectTest"
    cxx_class = "gem5::branch_prediction::btb_pred::HintIndirectTest"
    cxx_header = "cpu/pred/btb/test/hint_indirect_test.hh"
    metadata = Param.String("", "Optional external metadata validation case")
    candidates = Param.String(
        "", "Optional external candidate validation case"
    )
    ideal = Param.Bool(False, "Validate an unlimited Hint metadata image")
    range_port_reuse = Param.Bool(
        False, "Negative test: retry a wrapped-range query in the same cycle"
    )
