/*
 * Micro-Armed Bandit (DUCB) prefetcher manager.
 *
 * Selects among a set of "arms", where each arm is an enable-bitmask over a
 * list of sub-prefetchers. Uses the Discounted Upper Confidence Bound (DUCB)
 * algorithm from Gerogiannis & Torrellas, MICRO'23.
 */

#ifndef __MEM_CACHE_PREFETCH_BANDIT_HH__
#define __MEM_CACHE_PREFETCH_BANDIT_HH__

#include <cstdint>
#include <vector>

#include "mem/cache/prefetch/multi.hh"

namespace gem5
{

struct BanditPrefetcherParams;
class BaseCPU;

namespace prefetch
{

class Bandit : public Multi
{
  public:
    Bandit(const BanditPrefetcherParams &p);

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                       unsigned blk_size) override;

    /**
     * Override: we still forward prefetch generation to the sub-prefetchers,
     * but only those enabled by the currently selected arm may issue.
     */
    PacketPtr getPacket() override;
    bool hasPendingPacket() override;
    Tick nextPrefetchReadyTime() const override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType source,
                           bool miss_repeat) override;

    /**
     * Bandit listens on cache accesses (in addition to the sub-prefetchers'
     * own probes) to drive its bandit step counter.
     */
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

  private:
    /** Number of arms (= rows in the arm table). */
    uint32_t numArms;

    /**
     * Arm table: armMask[a] is a bitmask; bit i set => sub-prefetcher i is
     * enabled when arm a is selected.
     */
    std::vector<uint64_t> armMask;

    /** DUCB state. */
    std::vector<double> rTable; // average reward per arm
    std::vector<double> nTable; // (fractional) number of selections per arm
    double nTotal;

    /** Hyperparameters. */
    double gamma;          // DUCB discount factor (<1)
    double c;              // exploration constant
    uint64_t banditStep;   // step duration (demand accesses)
    uint64_t banditStepRR; // longer step during initial round robin

    /** Runtime state. */
    uint32_t currentArm;
    int32_t rrRemaining;   // arms still to try in initial RR phase
    uint64_t stepAccesses; // demand accesses seen in current step
    Tick stepStartTick;
    uint64_t stepStartInsts;

    BaseCPU *cpu; // source of committed insts (IPC reward)

    /** Advance the DUCB window by one demand observation. */
    void observeAccess();
    /** Select next arm using DUCB. */
    uint32_t selectNextArm();
    /** Update selections/reward after a step ends. */
    void finishStep(double reward);
    /** Read total committed instructions across threads. */
    uint64_t readTotalInsts() const;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_BANDIT_HH__
