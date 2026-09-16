#ifndef __CPU_PRED_BTB_TEST_HINT_INDIRECT_TEST_HH__
#define __CPU_PRED_BTB_TEST_HINT_INDIRECT_TEST_HH__

#include "params/HintIndirectTest.hh"
#include "sim/sim_object.hh"

namespace gem5::branch_prediction::btb_pred
{

class HintIndirectTest : public SimObject
{
  public:
    explicit HintIndirectTest(const HintIndirectTestParams &p)
        : SimObject(p), metadata(p.metadata), candidates(p.candidates),
          ideal(p.ideal), rangePortReuse(p.range_port_reuse) {}
    void startup() override;

  private:
    const std::string metadata;
    const std::string candidates;
    const bool ideal;
    const bool rangePortReuse;
};

} // namespace gem5::branch_prediction::btb_pred

#endif // __CPU_PRED_BTB_TEST_HINT_INDIRECT_TEST_HH__
