"""TEST_ONLY: white-box driver for production H2P statistics helpers."""
from m5.SimObject import SimObject


class H2PStatTest(SimObject):
    type = "H2PStatTest"
    cxx_class = "gem5::branch_prediction::btb_pred::H2PStatTest"
    cxx_header = "cpu/pred/btb/test/h2p_stat_test.hh"
