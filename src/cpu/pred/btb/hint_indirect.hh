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
class HintFiniteHistory;

// This object follows one prediction into the FTQ. The physical SRAM entry
// and target are snapshots, not a request to repeat a lookup at retirement.
struct HintQuery
{
    std::shared_ptr<const HintSnapshot> snapshot;
    Addr blockStart = 0;
    Addr blockEnd = 0;
    uint64_t queryCycle = 0;
    bool hasCandidate = false;
    bool admitted = false;
    bool canceled = false;
    bool localWrite = false;
    uint64_t s3Cycle = 0;
    Addr pc = 0;
    unsigned mask = 0;
    unsigned cfiPosition = 0;
    bool isCall = false;
    Addr candidatePC() const { return (blockStart & ~Addr(31)) + 2 * cfiPosition; }
    int camIndex = -1;
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
    uint64_t ftqId = 0;
    bool resolved = false;
};

class HintIndirect
{
  public:
    struct Config
    {
        std::string mode = "off";
        std::string profileContext = "prediction";
        bool ideal = false;
        std::string candidatePath;
        std::string metadataPath;
        unsigned camEntries = 16;
        unsigned contextEntries = 4096;
        unsigned ways = 2;
        unsigned slotEntries = 32;
        unsigned confQueueDepth = 8;
        unsigned metaEntries = 16;
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
        uint64_t overridesMisses = 0;
        uint64_t tagHitMisses = 0;
    };

    struct QueueCounts
    {
        uint64_t enqueued = 0;
        uint64_t applied = 0;
        uint64_t dropped = 0;
        uint64_t carriedIn = 0;
        uint64_t zeroTransitions = 0;
        uint64_t recoveries = 0;
        uint64_t canceled = 0;
        uint64_t reverted = 0;
        uint64_t rollbackWrites = 0;
        uint64_t rollbackFallbacks = 0;
        uint64_t rollbackReadConflicts = 0;
        uint64_t maxOccupancy = 0;
        uint64_t occupancySamples = 0;
        uint64_t occupancySum = 0;
        // Failed prediction-enqueue attempts in the measured interval.
        uint64_t historyWindowStalls = 0;
        uint64_t metaQueueStalls = 0;
        uint64_t lhrStalls = 0;
    };

    enum class Table { Cam, Context, Slots };
    enum class StallReason { None, HistoryWindow, MetaQueue, Lhr };

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
    bool needsCommitProfileContext() const {
        return isCollect() && config.profileContext == "commit-pre";
    }
    bool isFiniteV3() const { return enabled() && !config.ideal; }
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
        bool is_return, bool btb_identified, unsigned selected_size = 2);

    // The caller invokes this only for committed non-return indirect branches.
    // It must include non-candidate PCs so global final-miss counts are exact.
    void commit(const std::shared_ptr<HintQuery> &query, uint64_t sequence,
        Addr pc, Addr actual, bool final_valid, Addr final_target,
        uint64_t cycle, const HintSnapshot *profile_snapshot = nullptr,
        uint64_t commit_event_ordinal = 0, Addr query_start_pc = MaxAddr,
        unsigned instruction_size = 2, bool actual_call = false,
        bool actual_return = false);

    // Diagnostic stream of all retired controls, including warmup. This never
    // changes prediction state or modeled timing.
    uint64_t recordCommittedControl(const HintFiniteHistory &history,
        uint64_t sequence, uint64_t cycle, Addr start_pc, Addr actual_pc,
        Addr target, bool indirect, bool call, bool is_return, bool taken,
        int local_selector);

    bool canReserve(Addr pc) const;
    StallReason reserveBlockReason(Addr pc) const;
    void recordStall(StallReason reason);
    void reserve(const std::shared_ptr<HintQuery> &query, Addr pc,
                 uint8_t local_phase, const HintFiniteHistory *history = nullptr);
    void bind(const std::shared_ptr<HintQuery> &query, uint64_t ftq_id,
              unsigned ftq_capacity = 64);
    void resolve(const std::shared_ptr<HintQuery> &query, Addr pc,
                 Addr actual, uint64_t sequence, uint64_t cycle,
                 std::optional<bool> measure_at_resolve = std::nullopt,
                 bool actual_taken = true, bool actual_indirect = true,
                 bool actual_return = false, unsigned instruction_size = 2);
    void resolveByFTQId(uint64_t ftq_id, Addr pc, Addr actual,
                        uint64_t sequence, uint64_t cycle,
                        bool measure_at_resolve, bool actual_taken = true, bool actual_indirect = true,
                 bool actual_return = false, unsigned instruction_size = 2);
    void redirect(HintFiniteHistory &history, uint64_t ftq_id, Addr squash_pc,
                  bool actual_h2p, Addr actual, uint64_t sequence,
                  uint64_t cycle);
    void commitTarget(uint64_t ftq_id);
    void cancelPending();

    void tick(uint64_t cycle);
    void setMeasurementActive(bool active);
    void setMeasurementActive(bool active, uint64_t cycle);
    void resetStats();
    void resetStats(uint64_t cycle);
    void finish();

    const Counts &totals() const { return global; }
    const QueueCounts &queueCounts() const { return queueStats; }
    size_t pendingUpdates() const {
        return config.ideal ? updates.size() : finiteUpdates.size();
    }
    size_t inFlightHints() const { return finiteMeta.size(); }
    size_t pendingRollbacks() const;
    unsigned confidenceAt(unsigned set, unsigned way) const;
    unsigned confidenceAtEntry(size_t entry_id) const;
    int localIndex(Addr pc) const;
    Occupancy occupancy(Table table) const;

  private:
    struct CamEntry
    {
        Addr pc = 0;
        unsigned mask = 0;
        bool valid = true;
        unsigned cfiPosition = 0;
        bool isCall = false;
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
    struct FiniteMeta
    {
        std::shared_ptr<HintQuery> query;
        Addr branchPC = 0;
        uint64_t ftqId = 0;
        unsigned ftqIndex = 0;
        uint64_t ftqGeneration = 0;
        uint64_t ordinal = 0;
        int localIndex = -1;
        uint8_t localPhase = 31;
        std::array<uint8_t, 16> localPhases{};
        bool resolved = false;
        bool committed = false;
        bool scoreEligible = false;
        bool enqueued = false;
        bool applied = false;
        bool discarded = false;
        bool dropped = false;
        uint64_t sequence = 0;
        uint64_t resolveCycle = 0;
        uint64_t readyCycle = 0;
        Addr actual = 0;
        unsigned newConf = 0;
    };
    struct QueryCounts
    {
        uint64_t lookups = 0;
        uint64_t camHits = 0;
        uint64_t camContentions = 0;
        uint64_t camExtraCandidates = 0;
        uint64_t busyFallbacks = 0;
        uint64_t contextReads = 0;
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
    void loadFiniteV3Metadata();
    void loadRawCam(const std::vector<uint8_t> &image);
    void writeDetail(const std::shared_ptr<HintQuery> &query,
        Addr start_pc, Addr pc, Addr actual, bool baseline_miss,
        unsigned instruction_size, uint64_t sequence, bool actual_call, bool actual_return);
    void openOutputs();
    void writeHeader();
    void writeProfile(const std::shared_ptr<HintQuery> &query,
        uint64_t sequence, Addr pc, Addr actual, bool baseline_valid,
        Addr baseline_pc, Addr baseline_target, bool eligible,
        uint64_t cycle, const HintSnapshot *profile_snapshot,
        uint64_t commit_event_ordinal);
    void writeConfZero(const Update &update, uint64_t cycle);
    void writeStats();
    void enqueue(const std::shared_ptr<HintQuery> &query,
        uint64_t sequence, Addr actual, uint64_t cycle);
    void drainFiniteResolved();
    void releaseCommittedFinite();
    void tickFinite(uint64_t cycle);
    // Future-stage BPU abstraction: query reserves S2, result belongs to S3.
    void undoFinite(const std::shared_ptr<FiniteMeta> &record, uint64_t cycle);
    std::string outputName(const std::string &name) const;

    const Config config;
    uint64_t commitEventOrdinal = 0;
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
    std::deque<std::shared_ptr<FiniteMeta>> finiteMeta;
    std::deque<std::shared_ptr<FiniteMeta>> finiteScoreOrder;
    std::deque<std::shared_ptr<FiniteMeta>> finiteUpdates;
    uint64_t nextFiniteOrdinal = 1;
    std::optional<uint64_t> finiteServiceThrough;
    Counts global;
    std::map<Addr, Counts> perPc;
    QueueCounts queueStats;
    QueryCounts queryStats;
    bool measurementActive = false;
    bool finished = false;
    std::optional<uint64_t> lastTick;
    std::optional<uint64_t> lastQueryCycle;
    std::optional<uint64_t> lastContextReadCycle;
    uint64_t busyUntil = 0;
    std::weak_ptr<HintQuery> busyQuery;
    std::unique_ptr<Statistics> stats;
    std::unique_ptr<Output> output;
    uint64_t detailRecords = 0;
    uint64_t detailInvalidQueries = 0;
    uint64_t detailInvalidWindows = 0;
};

} // namespace branch_prediction::btb_pred
} // namespace gem5

#endif // __CPU_PRED_BTB_HINT_INDIRECT_HH__
