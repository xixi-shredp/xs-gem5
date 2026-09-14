#ifndef __CPU_PRED_BTB_TEST_H2P_STAT_TEST_HH__
#define __CPU_PRED_BTB_TEST_H2P_STAT_TEST_HH__

#include "params/H2PStatTest.hh"
#include "sim/sim_object.hh"

namespace gem5::branch_prediction::btb_pred
{

class H2PStatTest : public SimObject
{
  public:
    explicit H2PStatTest(const H2PStatTestParams &p) : SimObject(p) {}
    void startup() override;
};

} // namespace gem5::branch_prediction::btb_pred

#endif
