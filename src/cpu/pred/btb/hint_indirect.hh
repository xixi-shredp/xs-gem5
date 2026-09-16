#ifndef __CPU_PRED_BTB_HINT_INDIRECT_HH__
#define __CPU_PRED_BTB_HINT_INDIRECT_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace statistics { class Group; }
namespace branch_prediction::btb_pred
{

struct HintSnapshot;

// This object follows one prediction into the FTQ. The physical SRAM entry
// and target are snapshots, not a request to repeat a lookup at retirement.
struct HintQuery
{
    std::shared_ptr<const HintSnapshot> snapshot;
    Addr blockStart = 0;
    Addr blockEnd = 0;
    uint64_t queryCycle = 0;
    bool hasCandidate = false;
    Addr pc = 0;
    unsigned mask = 0;
    uint32_t key = 0;
    unsigned set = 0;
    unsigned way = 0;
    size_t entryId = std::numeric_limits<size_t>::max();
    size_t slot = 0;
    bool tagHit = false;
    unsigned conf = 0;
    Addr hintTarget = 0;
    bool completed = false;
    bool baselineValid = false;
    Addr baselinePC = 0;
    Addr baselineTarget = 0;
    bool baselineIndirect = false;
    bool baselineReturn = false;
    bool btbIdentified = false;
    bool eligible = false;
    bool overridden = false;
};

class HintIndirect
{
  public:
    struct Config
    {
        std::string mode = "off";
        bool ideal = false;
        std::string candidatePath;
        std::string metadataPath;
        unsigned camEntries = 16;
        unsigned contextEntries = 4096;
        unsigned ways = 2;
        unsigned slotEntries = 32;
        unsigned confQueueDepth = 8;
        Tick clockPeriod = 1;
        // An output subdirectory allows the final-binary driver to instantiate
        // several independent production helpers in one test process.
        std::string outputPrefix;
    };

    struct Counts
    {
        uint64_t executions = 0;
        uint64_t baselineMisses = 0;
        uint64_t finalMisses = 0;
        uint64_t corrected = 0;
        uint64_t harmed = 0;
        uint64_t eligible = 0;
        uint64_t tagHits = 0;
        uint64_t confEnabled = 0;
        uint64_t overrides = 0;
    };

    struct QueueCounts
    {
        uint64_t enqueued = 0;
        uint64_t applied = 0;
        uint64_t dropped = 0;
        uint64_t carriedIn = 0;
        uint64_t zeroTransitions = 0;
        uint64_t recoveries = 0;
        uint64_t maxOccupancy = 0;
        uint64_t occupancySamples = 0;
        uint64_t occupancySum = 0;
    };

    enum class Table { Cam, Context, Slots };

    struct Occupancy
    {
        uint64_t residentEntries = 0;
        double avgEntries = 0;
        uint64_t peakEntries = 0;
        uint64_t sampleCycles = 0;
        uint64_t finiteCapacity = 0;
    };

    HintIndirect(const Config &config, statistics::Group *parent);
    ~HintIndirect();

    bool enabled() const { return config.mode != "off"; }
    bool isCollect() const { return config.mode == "collect"; }
    bool isReplay() const { return config.mode == "replay"; }
    bool isIdeal() const { return config.ideal; }
    const std::vector<Addr> &historyPCs() const { return historyPcs; }

    // Ordered [start, end) bounds. Empty or wrapped bounds consume the normal
    // query opportunity and return no candidate, matching the original BTBs.
    std::shared_ptr<HintQuery> query(
        const std::shared_ptr<const HintSnapshot> &snapshot,
        Addr block_start, Addr block_end, uint64_t cycle);

    // Called once after original predictor components have filled the stage
    // vector. The caller applies the optional result from stage index 2.
    std::optional<Addr> complete(const std::shared_ptr<HintQuery> &query,
        Addr selected_pc, Addr selected_target, bool taken, bool indirect,
        bool is_return, bool btb_identified);

    // The caller invokes this only for committed non-return indirect branches.
    // It must include non-candidate PCs so global final-miss counts are exact.
    void commit(const std::shared_ptr<HintQuery> &query, uint64_t sequence,
        Addr pc, Addr actual, bool final_valid, Addr final_target,
        uint64_t cycle);

    void tick(uint64_t cycle);
    void setMeasurementActive(bool active);
    void setMeasurementActive(bool active, uint64_t cycle);
    void resetStats();
    void resetStats(uint64_t cycle);
    void finish();

    const Counts &totals() const { return global; }
    const QueueCounts &queueCounts() const { return queueStats; }
    size_t pendingUpdates() const { return updates.size(); }
    unsigned confidenceAt(unsigned set, unsigned way) const;
    unsigned confidenceAtEntry(size_t entry_id) const;
    Occupancy occupancy(Table table) const;

  private:
    struct CamEntry
    {
        Addr pc = 0;
        unsigned mask = 0;
    };
    struct ContextEntry
    {
        bool valid = false;
        uint32_t tag = 0;
        uint32_t key = 0;
        size_t slot = 0;
        unsigned conf = 0;
    };
    struct Update
    {
        std::shared_ptr<const HintQuery> query;
        Addr actual = 0;
        uint64_t sequence = 0;
        uint64_t commitCycle = 0;
        uint64_t readyCycle = 0;
    };
    struct QueryCounts
    {
        uint64_t lookups = 0;
        uint64_t camHits = 0;
        uint64_t camContentions = 0;
        uint64_t camExtraCandidates = 0;
        uint64_t tagHits = 0;
        uint64_t confEnabled = 0;
        uint64_t overrides = 0;
        uint64_t cancelNoBtb = 0;
        uint64_t cancelNotTaken = 0;
        uint64_t cancelNotIndirect = 0;
        uint64_t cancelReturn = 0;
        uint64_t cancelPcMismatch = 0;
    };
    struct Statistics;
    struct Output;

    void loadCandidates();
    void loadMetadata();
    void openOutputs();
    void writeHeader();
    void writeProfile(const std::shared_ptr<HintQuery> &query,
        uint64_t sequence, Addr pc, Addr actual, bool baseline_valid,
        Addr baseline_pc, Addr baseline_target, bool eligible,
        uint64_t cycle);
    void writeConfZero(const Update &update, uint64_t cycle);
    void writeStats();
    void enqueue(const std::shared_ptr<HintQuery> &query,
        uint64_t sequence, Addr actual, uint64_t cycle);
    std::string outputName(const std::string &name) const;

    const Config config;
    unsigned sets = 0;
    unsigned indexBits = 0;
    std::vector<Addr> candidates;
    std::vector<Addr> candidateIndex;
    std::vector<Addr> historyPcs;
    std::vector<CamEntry> cam;
    std::vector<Addr> slots;
    std::vector<ContextEntry> entries;
    std::unordered_map<uint32_t, size_t> idealIndex;
    std::array<uint64_t, 3> residentEntries{};
    uint64_t occupancyCycles = 0;
    std::optional<uint64_t> occupancyCycle;
    std::deque<Update> updates;
    Counts global;
    std::map<Addr, Counts> perPc;
    QueueCounts queueStats;
    QueryCounts queryStats;
    bool measurementActive = false;
    bool finished = false;
    std::optional<uint64_t> lastTick;
    std::optional<uint64_t> lastQueryCycle;
    std::unique_ptr<Statistics> stats;
    std::unique_ptr<Output> output;
};

} // namespace branch_prediction::btb_pred
} // namespace gem5

#endif // __CPU_PRED_BTB_HINT_INDIRECT_HH__
