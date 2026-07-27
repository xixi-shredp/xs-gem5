/**
 * @file
 * Bingo spatial data prefetcher (2019 HPCA, Bakhshalipour et al.).
 *
 * Port of the ChampSim reference implementation
 * (https://github.com/bakhshalipour/Bingo) to gem5's Queued prefetcher
 * framework.
 */

#ifndef __MEM_CACHE_PREFETCH_BINGO_HH__
#define __MEM_CACHE_PREFETCH_BINGO_HH__

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct BingoPrefetcherParams;

namespace prefetch
{

class Bingo : public Queued
{
  private:
    enum Event
    {
        PC_ADDRESS = 0,
        PC_OFFSET = 1,
        EV_MISS = 2
    };

    // ---- configuration ----
    const unsigned regionSize;
    const unsigned patternLen; // blocks per region
    const unsigned ftSize;
    const unsigned atSize;
    const unsigned phtSize;
    const unsigned phtWays;
    const unsigned phtSets;
    const unsigned pcWidth;
    const unsigned minAddrWidth;
    const unsigned maxAddrWidth;
    const unsigned indexLen; // log2(phtSets)
    const uint64_t minTagMask;
    const uint64_t maxTagMask;
    const double thresh;
    const bool rotatePattern;

    // ---- FilterTable (full-assoc LRU) ----
    struct FTEntry
    {
        Addr region;
        Addr pc;
        uint32_t offset;
        bool valid;
    };
    std::list<FTEntry> ftList; // MRU -> LRU
    std::unordered_map<Addr, std::list<FTEntry>::iterator> ftMap;

    // ---- AccumulationTable (full-assoc LRU) ----
    struct ATEntry
    {
        Addr region;
        Addr pc;
        uint32_t offset;
        std::vector<bool> pattern;
        bool valid;
    };
    std::list<ATEntry> atList;
    std::unordered_map<Addr, std::list<ATEntry>::iterator> atMap;

    // ---- PatternHistoryTable (set-assoc LRU) ----
    struct PHTEntry
    {
        bool valid;
        uint64_t tag;
        std::vector<bool> pattern;
        uint64_t lru;
    };
    std::vector<std::vector<PHTEntry>> pht; // [set][way]
    uint64_t lruClock;

    // Remember which event produced the pattern for each pending region,
    // to attribute coverage / overprediction later.
    std::unordered_map<Addr, Event> regionEvent;
    std::unordered_map<Addr, Event> prefetchedBlocks;

    // ---- stats ----
    struct BingoStats : public statistics::Group
    {
        BingoStats(statistics::Group *parent);
        statistics::Scalar triggerAccesses;
        statistics::Scalar ftPromotions;
        statistics::Scalar atEvictTrain;
        statistics::Scalar llcEvictTrain;
        statistics::Scalar phtMaxHits;
        statistics::Scalar phtMinHits;
        statistics::Scalar phtMisses;
        statistics::Scalar pfFromPcAddress;
        statistics::Scalar pfFromPcOffset;
        statistics::Scalar coverFromPcAddress;
        statistics::Scalar coverFromPcOffset;
        statistics::Scalar overpredFromPcAddress;
        statistics::Scalar overpredFromPcOffset;
    } bingoStats;

    // ---- internals ----
    uint64_t buildKey(Addr pc, Addr blockNumber) const;
    std::vector<bool> phtFind(Addr pc, Addr blockNumber, Event &ev);
    void phtInsert(Addr pc, Addr blockNumber,
                   const std::vector<bool> &pattern);
    static std::vector<bool> myRotate(const std::vector<bool> &x, int n);
    std::vector<bool> vote(const std::vector<std::vector<bool>> &cands) const;

    // FT helpers
    FTEntry *ftFind(Addr region);
    void ftInsert(Addr region, Addr pc, uint32_t offset);
    void ftErase(Addr region);

    // AT helpers
    ATEntry *atFind(Addr region);
    bool atSetPattern(Addr region, uint32_t offset);
    ATEntry atInsertFromFt(const FTEntry &src, uint32_t nowOffset);
    ATEntry atErase(Addr region);

  public:
    Bingo(const BingoPrefetcherParams &p);
    ~Bingo() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_BINGO_HH__
