#ifndef __CPU_PRED_BTB_H2P_INDIRECT_STATS_HH__
#define __CPU_PRED_BTB_H2P_INDIRECT_STATS_HH__

#include <algorithm>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class H2PIndirectStats
{
  public:
    static bool eligibleIndirect(bool is_indirect, bool is_return) { return is_indirect && !is_return; }

    // Match the final fetch prediction to this dynamic branch. A sequential
    // fetch or a prediction for another PC supplies no indirect target.
    static std::optional<Addr>
    selectFinalTarget(Addr pc, bool predicted_taken, Addr predicted_branch_pc,
                      Addr predicted_target)
    {
        return predicted_taken && pc == predicted_branch_pc ?
            std::optional<Addr>(predicted_target) : std::nullopt;
    }

    void record(Addr pc, Addr actual_target,
                const std::optional<Addr> &final_target)
    {
        auto &record = records[pc];
        ++record.executions;
        if (!final_target)
            ++record.absent;
        else if (*final_target != actual_target)
            ++record.wrong;
        auto &target_misses = record.targets[actual_target];
        if (!final_target || *final_target != actual_target) {
            ++record.misses;
            ++target_misses;
        }
    }

    void clear() { records.clear(); }

    void writeSummary(std::ostream &out) const
    {
        uint64_t executions = 0, absent = 0, wrong = 0;
        for (const auto &[pc, record] : records) {
            executions += record.executions;
            absent += record.absent;
            wrong += record.wrong;
        }
        out << "{\"format\":\"committed-nonreturn-indirect-targets\","
            << "\"version\":1,\"executions\":" << executions
            << ",\"absent_target\":" << absent
            << ",\"valid_wrong_target\":" << wrong
            << ",\"target_misses\":" << absent + wrong
            << ",\"pcs\":[";
        bool first = true;
        for (const auto &[pc, record] : records) {
            if (!first) out << ',';
            first = false;
            out << "{\"pc\":\"0x" << std::hex << pc << std::dec
                << "\",\"executions\":" << record.executions
                << ",\"absent_target\":" << record.absent
                << ",\"valid_wrong_target\":" << record.wrong
                << ",\"target_misses\":" << record.misses << '}';
        }
        out << "]}\n";
    }

    void write(std::ostream &out) const
    {
        for (const auto &[pc, record] : records) {
            if (record.misses == 0) {
                continue;
            }
            out << "0x" << std::hex << pc << std::dec << ", " << record.misses;
            for (const auto &[target, misses] : record.targets) {
                out << ", 0x" << std::hex << target << std::dec
                    << "(" << misses << ")";
            }
            out << '\n';
        }
    }

  private:
    struct Record
    {
        uint64_t misses = 0;
        uint64_t executions = 0, absent = 0, wrong = 0;
        std::map<Addr, uint64_t> targets;
    };

    std::map<Addr, Record> records;
};

// Shared by the predictor and the in-binary test driver. This object owns
// only measurement data and output files; it never resets predictor state.
class H2PIndirectStatsOutput
{
  public:
    H2PIndirectStatsOutput(bool enabled, uint64_t interval);

    void setMeasurementActive(bool active) { measurementActive = active; }

    void record(Addr pc, Addr target, const std::optional<Addr> &prediction)
    {
        if (!enabled || !measurementActive) {
            return;
        }
        total.record(pc, target, prediction);
        if (interval) {
            window.record(pc, target, prediction);
        }
    }

    void notifyCommit(uint64_t units)
    {
        if (!enabled || !measurementActive || !interval) {
            return;
        }
        // A branch belongs to the retirement group's first instruction.
        // Subsequent fused instructions may advance further windows.
        while (units) {
            const uint64_t step = std::min(units, interval - windowInsts);
            windowInsts += step;
            units -= step;
            if (windowInsts == interval) {
                dumpWindow();
            }
        }
    }

    void reset();


    void dump();


  private:
    void dumpWindow();


    const bool enabled;
    const uint64_t interval;
    bool measurementActive = true;
    bool totalWritten = false;
    uint64_t windowStart = 0;
    uint64_t windowInsts = 0;
    std::vector<std::string> windowFiles;
    H2PIndirectStats total;
    H2PIndirectStats window;
};

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_BTB_H2P_INDIRECT_STATS_HH__
