#include "cpu/pred/btb/test/h2p_stat_test.hh"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/pred/btb/h2p_indirect_stats.hh"
#include "sim/sim_exit.hh"

namespace gem5::branch_prediction::btb_pred
{

namespace
{
std::string
readOutput(const std::string &name)
{
    std::ifstream input(simout.resolve(name));
    fatal_if(!input, "Missing H2P test output %s", name);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void
expectOutput(const std::string &name, const std::string &expected)
{
    fatal_if(readOutput(name) != expected,
             "H2P test output mismatch for %s", name);
}

void
expectAbsent(const std::string &name)
{
    fatal_if(std::filesystem::exists(simout.resolve(name)),
             "Unexpected H2P test output %s", name);
}
} // anonymous namespace

void
H2PStatTest::startup()
{
    SimObject::startup();
    using Stats = H2PIndirectStats;
    fatal_if(!Stats::eligibleIndirect(true, false) ||
             Stats::eligibleIndirect(true, true) ||
             Stats::eligibleIndirect(false, false),
             "H2P branch eligibility mismatch");

    // The final stream prediction is authoritative regardless of provider.
    // Correct BTB fallback and a correct target without ITTAGE metadata must
    // remain correct; target selection does not require ITTAGE metadata.
    const auto correct = Stats::selectFinalTarget(0x10, true, 0x10, 0x20);
    const auto wrong = Stats::selectFinalTarget(0x10, true, 0x10, 0x30);
    const auto absent = Stats::selectFinalTarget(0x10, false, 0x10, 0x20);
    const auto other_pc = Stats::selectFinalTarget(0x10, true, 0x18, 0x20);
    fatal_if(correct != std::optional<Addr>(0x20) ||
             wrong != std::optional<Addr>(0x30) || absent || other_pc,
             "H2P final target selection mismatch");
    Stats data;
    data.record(0x10, 0x20, correct); // ITTAGE correct.
    data.record(0x10, 0x20, correct); // BTB fallback correct.
    data.record(0x10, 0x20, correct); // No ITTAGE metadata, final correct.
    std::ostringstream correct_text;
    data.write(correct_text);
    fatal_if(!correct_text.str().empty(),
             "Correct final targets must not create H2P output");
    data.record(0x10, 0x20, wrong); // ITTAGE wrong.
    data.record(0x10, 0x20, wrong); // BTB fallback wrong.
    data.record(0x10, 0x30, absent); // No selected target.
    data.record(0x10, 0x30, other_pc); // Stream predicts another branch.
    data.record(0x10, 0x40, 0x40); // Correct target retained with zero miss.
    std::ostringstream text;
    data.write(text);
    fatal_if(text.str() != "0x10, 4, 0x20(2), 0x30(2), 0x40(0)\n",
             "H2P per-target miss accounting mismatch");

    H2PIndirectStatsOutput disabled(false, 0);
    disabled.record(0x10, 0x20, std::nullopt);
    disabled.notifyCommit(1000);
    disabled.dump();
    expectAbsent("h2p-indirect.txt");
    expectAbsent("h2p-indirec");

    H2PIndirectStatsOutput output(true, 1000);
    output.setMeasurementActive(false);
    output.record(0xdead, 0xbeef, std::nullopt);
    output.notifyCommit(1000000);
    output.reset(); // A reset must not enable warm-up collection.
    output.record(0xdead, 0xbeef, std::nullopt);
    output.notifyCommit(1000000);
    output.dump();
    expectOutput("h2p-indirect.txt", "");
    expectAbsent("h2p-indirec/0.txt");
    output.reset();
    output.setMeasurementActive(true);
    output.record(0x20, 0x10, 0x10);
    output.notifyCommit(999);
    output.record(0x20, 0x20, std::nullopt);
    output.notifyCommit(3); // Branch owns unit 1000; fusion spans boundary.
    expectOutput("h2p-indirec/0.txt", "0x20, 1, 0x10(0), 0x20(1)\n");
    output.record(0x10, 0x30, std::nullopt);
    output.notifyCommit(998);
    expectOutput("h2p-indirec/1000.txt", "0x10, 1, 0x30(1)\n");
    output.dump();
    expectAbsent("h2p-indirec/2000.txt");
    expectOutput("h2p-indirect.txt",
                 "0x10, 1, 0x30(1)\n0x20, 1, 0x10(0), 0x20(1)\n");
    output.reset();
    expectAbsent("h2p-indirec/0.txt");
    expectAbsent("h2p-indirec/1000.txt");
    expectAbsent("h2p-indirect.txt");

    output.notifyCommit(1000); // Full empty window.
    expectOutput("h2p-indirec/0.txt", "");
    output.record(0x30, 0x80, std::nullopt);
    output.notifyCommit(1);
    output.dump(); // Partial tail window.
    expectOutput("h2p-indirec/1000.txt", "0x30, 1, 0x80(1)\n");
    output.reset();

    H2PIndirectStatsOutput aggregate(true, 0);
    aggregate.record(0x10, 0x20, std::nullopt);
    aggregate.notifyCommit(2000);
    aggregate.dump();
    expectOutput("h2p-indirect.txt", "0x10, 1, 0x20(1)\n");
    expectAbsent("h2p-indirec/0.txt");
    exitSimLoop("H2P self-test passed");
}

} // namespace gem5::branch_prediction::btb_pred
