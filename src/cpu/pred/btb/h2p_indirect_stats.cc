#include "cpu/pred/btb/h2p_indirect_stats.hh"

#include <filesystem>

#include "base/logging.hh"
#include "base/output.hh"

namespace gem5::branch_prediction::btb_pred
{

H2PIndirectStatsOutput::H2PIndirectStatsOutput(bool enabled, uint64_t interval)
    : enabled(enabled), interval(interval)
{
    fatal_if(interval && !enabled, "h2pStatInterval requires h2pStat");
    if (!enabled) {
        return;
    }
    const auto total_path = simout.resolve("h2p-indirect.txt");
    const auto window_path = simout.resolve("h2p-indirec");
    fatal_if(std::filesystem::exists(total_path) ||
             (std::filesystem::exists(window_path) &&
              (!std::filesystem::is_directory(window_path) ||
               !std::filesystem::is_empty(window_path))),
             "H2P output already exists; use a fresh output directory");
    if (interval) {
        simout.createSubdirectory("h2p-indirec");
    }
}

void
H2PIndirectStatsOutput::reset()
{
    total.clear();
    window.clear();
    windowStart = 0;
    windowInsts = 0;
    for (const auto &name : windowFiles) {
        simout.remove(name);
    }
    windowFiles.clear();
    // A test driver may explicitly dump before resetting. Normal runs
    // write the aggregate only on exit.
    if (totalWritten) {
        simout.remove("h2p-indirect.txt");
        simout.remove("h2p-target-summary.json");
        totalWritten = false;
    }
}

void
H2PIndirectStatsOutput::dump()
{
    if (!enabled) {
        return;
    }
    if (interval && windowInsts) {
        dumpWindow();
    }
    auto out = simout.create("h2p-indirect.txt", false, true);
    total.write(*out->stream());
    simout.close(out);
    auto summary = simout.create("h2p-target-summary.json", false, true);
    total.writeSummary(*summary->stream());
    simout.close(summary);
    totalWritten = true;
}

void
H2PIndirectStatsOutput::dumpWindow()
{
    const std::string name = "h2p-indirec/" +
        std::to_string(windowStart) + ".txt";
    auto out = simout.create(name, false, true);
    window.write(*out->stream());
    simout.close(out);
    windowFiles.push_back(name);
    windowStart += windowInsts;
    windowInsts = 0;
    window.clear();
}

} // namespace gem5::branch_prediction::btb_pred
