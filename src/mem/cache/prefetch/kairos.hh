/**
 * Kairos (croage) temporal prefetcher, ported from the ChampSim
 * reference implementation at comp-arch-research/Kairos (prefetcher/croage/).
 *
 * Reference:
 *   Kairos: Elevating Temporal Prefetching Through Instruction Correlation,
 *   MICRO 2025.
 *
 * This port follows the open-source code's actual behavior, which differs
 * from the paper on a few thresholds (see Kairos.md in the companion
 * documentation).  Notable simplifications relative to the reference:
 *   - No runtime LLC way-stealing.  The PID controller still adjusts the
 *     on-chip metadata way count, but LLC capacity is left untouched.
 *   - The commented-out BOP fallback in croage.h is omitted.
 */

#ifndef __MEM_CACHE_PREFETCH_KAIROS_HH__
#define __MEM_CACHE_PREFETCH_KAIROS_HH__

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct KairosPrefetcherParams;

namespace prefetch
{

class Kairos : public Queued
{
  private:
    // ---------- Detecting Unit (FIFO of per-PC miss counts) -------------
    struct KDEntry
    {
        Addr pc = 0;
        uint64_t issue_count = 0;
        uint64_t miss_count = 0;
    };

    class KeyDetect
    {
      public:
        explicit KeyDetect(std::size_t capacity);
        // Returns true iff the given PC currently qualifies as a key IP.
        bool update(Addr pc, bool cache_hit);

      private:
        const std::size_t capacity;
        std::list<KDEntry> fifo; // front = newest
        std::unordered_map<Addr, std::list<KDEntry>::iterator> lookup;
        uint64_t total_miss = 0;
    };

    // ---------- Training Unit (LRU of per-PC state) ---------------------
    struct TUEntry
    {
        Addr pc = 0;
        Addr last_addr = 0;
        Addr cur_addr = 0;
        uint16_t total_evict = 0;
        uint16_t useful_evict = 0;
        bool useful_tu = false;
    };

    class TrainUnit
    {
      public:
        explicit TrainUnit(std::size_t capacity);
        TUEntry *get(Addr pc);         // null if absent; LRU-touched
        TUEntry *findNoTouch(Addr pc); // for tem_up() evict accounting
        void insert(Addr pc, const TUEntry &entry);

      private:
        const std::size_t capacity;
        std::list<TUEntry> lru; // front = MRU
        std::unordered_map<Addr, std::list<TUEntry>::iterator> lookup;
    };

    // ---------- Metadata cache with clock-LFU replacement ---------------
    struct MDNode
    {
        Addr key = 0;   // last_addr
        Addr value = 0; // cur_addr
        int rpl = 0;
    };

    class CroageRepl
    {
      public:
        explicit CroageRepl(std::size_t capacity);
        Addr *get(Addr key); // returns nullptr if miss
        // Insert or update; if an entry is evicted, returns its key (0 if
        // none).
        Addr set(Addr key, Addr value, bool useful_tu);
        std::size_t
        size() const
        { return list_.size(); }
        std::size_t
        cap() const
        { return capacity; }
        void
        setCapacity(std::size_t new_cap)
        { capacity = new_cap; }
        // For resize migration
        const std::list<MDNode> &
        nodes() const
        { return list_; }

      private:
        std::size_t capacity;
        std::list<MDNode> list_; // front = MRU
        std::unordered_map<Addr, std::list<MDNode>::iterator> lookup;
        static constexpr int max_rpl = 3;
        // Clock-LFU victim selection.
        std::list<MDNode>::iterator popVictim();
    };

    // ---------- Parameters (captured from params) -----------------------
    const unsigned degreeMax;
    const std::size_t htSets;
    const std::size_t htWaysInit;
    const std::size_t htWaysMin;
    const std::size_t htWaysMax;
    const uint64_t trackingWindow;
    const double ALPHA;
    const double BETA;
    const double GAMMA;
    const double THETA_PLUS;
    const double THETA_MINUS;
    const double TAU;
    const bool enableLlcMetadata;
    const Addr metadataBaseAddr;
    const unsigned llcMetadataWaysInit;
    const unsigned llcMetadataWaysMin;
    const unsigned llcMetadataWaysMax;
    static constexpr int MAX_NO_IMPROVEMENT_WINDOWS = 2;
    static constexpr double IMPROVEMENT_THRESHOLD = 0.05;
    static constexpr double EMA_ALPHA = 0.7;
    static constexpr uint16_t EVICT_CLASSIFY_THRESHOLD = 128;
    static constexpr uint16_t USEFUL_NUM = 15; // 15/16 = 93.75%
    static constexpr uint16_t USEFUL_DEN = 16;
    static constexpr uint64_t WAY_CHUNK = 12; // resize step

    // ---------- Runtime state ------------------------------------------
    KeyDetect detect;
    TrainUnit trainer;
    std::vector<CroageRepl> metadata;          // [htSets]
    std::unordered_map<Addr, Addr> cam;        // last_addr -> pc
    std::unordered_map<Addr, bool> inMetadata; // last_addr -> useful?

    std::size_t currentWays;
    uint64_t accessCount = 0;
    uint64_t missCount = 0;
    uint64_t prefetchHitCount = 0;
    uint64_t currentWindow = 0;
    uint64_t previousMissCount = 0;
    double previousMissRate = 0.0;
    double previousDeltaMiss = 0.0;
    double smoothedMissRate = 0.0;
    int noImprovementCount = 0;

    // ---------- Stats ---------------------------------------------------
    struct KairosStats : public statistics::Group
    {
        KairosStats(statistics::Group *parent);
        statistics::Scalar metadataHits;
        statistics::Scalar metadataMisses;
        statistics::Scalar increaseEvents;
        statistics::Scalar decreaseEvents;
        statistics::Scalar maintainEvents;
        statistics::Scalar keyIpPredicts;
        statistics::Scalar chainPrefetches;
        statistics::Scalar llcMetadataReads;
        statistics::Scalar llcMetadataWrites;
    } kairosStats;

    // ---------- Helpers ------------------------------------------------
    std::size_t setIndex(Addr line_addr) const;
    std::vector<Addr> predict(Addr line_addr);
    void train(Addr pc, Addr line_addr, bool cache_hit, bool useful_prefetch);
    void temUp(Addr last_addr, Addr cur_addr, Addr pc, bool useful_tu);
    void evaluateWindow();
    void resizeMetadata(int8_t direction);

    // Compute a cache-line-aligned L3 address that stands in for the given
    // metadata key (the set index it maps to). Collisions across keys within
    // one set are intentional: they faithfully mirror the LLC set-aliasing.
    Addr metadataAddrFor(Addr last_addr) const;
    // Fire a real read/write toward the LLC to model metadata traffic.
    void emitMetadataAccess(Addr last_addr, bool is_write);

  public:
    Kairos(const KairosPrefetcherParams &p);
    ~Kairos() = default;

    void rxHint(BaseMMU::Translation *) override {}

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_KAIROS_HH__
