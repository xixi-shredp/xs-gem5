#ifndef __MEM_CACHE_PREFETCH_CENTRALIZED_HH__
#define __MEM_CACHE_PREFETCH_CENTRALIZED_HH__

#include <array>
#include <deque>

#include <boost/compute/detail/lru_cache.hpp>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/prefetch/sms.hh"
#include "params/CentralizedDataPrefetcher.hh"
#include "params/CentralizedPrefetcherEndpoint.hh"

namespace gem5
{
namespace prefetch
{

class CentralizedDataPrefetcher;

struct CentralizedCandidateKey
{
    Addr block;
    ContextID context;
    bool contextValid;
    bool secure;

    bool operator==(const CentralizedCandidateKey &other) const
    {
        return block == other.block && context == other.context &&
               contextValid == other.contextValid && secure == other.secure;
    }
};

struct CentralizedCandidateKeyHash
{
    size_t operator()(const CentralizedCandidateKey &key) const;
};

struct CentralizedPhysicalKey
{
    Addr block;
    bool secure;

    bool operator==(const CentralizedPhysicalKey &other) const
    {
        return block == other.block && secure == other.secure;
    }
};

struct CentralizedPhysicalKeyHash
{
    size_t operator()(const CentralizedPhysicalKey &key) const;
};

struct CentralizedSourceRecentKey
{
    Addr block;
    ContextID context;
    bool contextValid;
    bool secure;
    uint8_t source;
    uint8_t distanceBucket;
    uint16_t triggerPcHash;

    bool operator==(const CentralizedSourceRecentKey &other) const
    {
        return block == other.block && context == other.context &&
               contextValid == other.contextValid && secure == other.secure &&
               source == other.source &&
               distanceBucket == other.distanceBucket &&
               triggerPcHash == other.triggerPcHash;
    }
};

struct CentralizedSourceRecentKeyHash
{
    size_t operator()(const CentralizedSourceRecentKey &key) const;
};

struct CentralizedPrefetchCandidate
{
    Addr vaddr;
    Addr paddr;
    bool paddrValid;
    bool physicalRegistered;
    int32_t priority;
    int depth;
    PrefetchSourceType source;
    uint32_t distance;
    uint8_t preferredLevel;
    uint8_t actualLevel;
    bool crossPage;
    bool triggerPcValid;
    uint16_t triggerPcHash;
    uint16_t centralizedCoreId;
    uint64_t sequence;
    Tick enqueueTick;
    CentralizedCandidateKey key;
    CentralizedPhysicalKey physicalKey;
    Base::PFtriggerInfo trigger;

    CentralizedPrefetchCandidate(
        Addr vaddr, int32_t priority, int depth, PrefetchSourceType source,
        uint32_t distance, uint8_t preferred_level, uint64_t sequence,
        bool cross_page, Tick enqueue_tick,
        const CentralizedCandidateKey &key,
        const Base::PFtriggerInfo &trigger);
};

struct CentralizedCandidatePriority
{
    bool operator()(const CentralizedPrefetchCandidate &lhs,
                    const CentralizedPrefetchCandidate &rhs) const;
};

struct CentralizedCompletion
{
    CentralizedDataPrefetcher *owner;
    CentralizedPrefetchCandidate candidate;
    CentralizedCandidateKey key;
    CentralizedPhysicalKey physicalKey;
    PrefetchSourceType source;
    uint8_t preferredLevel;
    uint8_t actualLevel;

    CentralizedCompletion(CentralizedDataPrefetcher *owner,
                          const CentralizedPrefetchCandidate &candidate);
};

class CentralizedPrefetcherEndpoint : public Queued
{
  public:
    enum RejectReason : unsigned
    {
        InvalidCandidate,
        TotalQueueFull,
        PerCoreQueueFull,
        EnqueueCredit,
        EnqueueDuplicate,
        ArbitrationInsert,
        IssueCredit,
        IssueDuplicate,
        CacheAdmission,
        NumRejectReasons
    };

    explicit CentralizedPrefetcherEndpoint(
        const CentralizedPrefetcherEndpointParams &p);

    void notify(const PacketPtr &, const PrefetchInfo &) override {}
    void calculatePrefetch(const PrefetchInfo &,
                           std::vector<AddrPriority> &) override {}

    void registerEngine(CentralizedDataPrefetcher *owner);
    bool canAccept(const CentralizedDataPrefetcher *owner, Addr paddr,
                   PrefetchSourceType source, bool countReject = true);
    bool contains(Addr paddr, bool secure) const;

    enum CoverKind : unsigned
    {
        NoCover,
        EndpointPendingCover,
        CacheDemandCover,
        CachePrefetchSameSourceCover,
        CachePrefetchOtherSourceCover,
        MissQueueDemandCover,
        MissQueuePrefetchSameSourceCover,
        MissQueuePrefetchOtherSourceCover,
        MissQueueUnknownCover,
        WriteQueueCover
    };
    CoverKind coverKind(Addr paddr, bool secure,
                        PrefetchSourceType source) const;
    bool cacheFillInfo(Addr paddr, bool secure, Tick &fill_tick,
                       bool &had_demand) const;

    bool enqueue(const CentralizedPrefetchCandidate &candidate,
                 CentralizedDataPrefetcher *owner);
    PacketPtr getPacket() override;
    void notifyPrefetchResult(const PacketPtr &pkt, bool admitted) override;
    void startup() override;
    void prefetchUnused(Addr paddr,
                        const Request::XsMetadata &metadata) override;
    void prefetchLate(const Request::XsMetadata &metadata,
                      bool firstDemandMerge = true) override;
    void recordPrefetchAdmitted(
        const Request::XsMetadata &metadata) override;
    void recordPrefetchFill(const Request::XsMetadata &metadata) override;
    void recordUpperPrefetchConsumed(
        const Request::XsMetadata &metadata) override;
    void recordPrefetchFillVictim(const Request::XsMetadata &metadata,
                                  bool dirty,
                                  bool demandTouched) override;
    void recordPrefetchUseful(const Request::XsMetadata &metadata,
                              bool miss) override;

  protected:
    void addToQueue(std::list<DeferredPacket> &queue,
                    DeferredPacket &dpp) override;

  private:
    struct OwnerIngress
    {
        CentralizedDataPrefetcher *owner;
        std::deque<CentralizedPrefetchCandidate> queue;

        explicit OwnerIngress(CentralizedDataPrefetcher *owner = nullptr)
          : owner(owner)
        {}
    };

    CentralizedPhysicalKey physicalKey(Addr paddr, bool secure) const;
    bool insertReady(const CentralizedPrefetchCandidate &candidate);
    void failCompletion(const CentralizedPhysicalKey &key,
                        bool issue_credit_drop, bool duplicate_drop);
    void recordReject(PrefetchSourceType source, RejectReason reason);
    void arbitrate();
    void scheduleArbitration();
    OwnerIngress *selectIngress();
    size_t totalOccupancy() const;
    void routeQualityFeedback(const Request::XsMetadata &metadata,
                              unsigned event);

    const unsigned cacheLevel;
    const unsigned minMshrCredits;
    const unsigned perCoreQueueSize;
    const unsigned arbitrationWidth;
    std::map<unsigned, OwnerIngress> ingress;
    std::vector<unsigned> rrCoreIds;
    size_t rrCursor;
    size_t ingressOccupancy;
    EventFunctionWrapper arbitrationEvent;
    std::unordered_map<CentralizedPhysicalKey, CentralizedCompletion,
                       CentralizedPhysicalKeyHash> completions;
    std::unordered_set<CentralizedPhysicalKey,
                       CentralizedPhysicalKeyHash> handedOffIndex;

    struct EndpointStats : public statistics::Group
    {
        EndpointStats(statistics::Group *parent, unsigned queue_size);
        statistics::Scalar accepted;
        statistics::Scalar issued;
        statistics::Scalar rejectedFull;
        statistics::Scalar rejectedPerCoreFull;
        statistics::Scalar rejectedCredit;
        statistics::Scalar rejectedDuplicate;
        statistics::Scalar rejectedInsert;
        statistics::Scalar issueCreditDrop;
        statistics::Scalar rrSelections;
        statistics::Vector acceptedBySource;
        statistics::Vector admittedBySource;
        statistics::Vector2d rejectedBySourceAndReason;
        statistics::Distribution queueOccupancy;
    } endpointStats;
};

class CentralizedDataPrefetcher : public XSCompositePrefetcher
{
  public:
    enum DropReason : unsigned
    {
        Duplicate,
        CentralQueueFull,
        MissingTrigger,
        TranslationUnavailable,
        TranslationFailed,
        PhysicalDuplicate,
        NoAdmission,
        EndpointRejected,
        IssueCreditDrop,
        NumDropReasons
    };

    enum PhysicalDuplicateCoverReason : unsigned
    {
        PendingPhysicalDuplicate,
        L1CacheDemand,
        L1CachePrefetchSameSource,
        L1CachePrefetchOtherSource,
        L1MissQueueDemand,
        L1MissQueuePrefetchSameSource,
        L1MissQueuePrefetchOtherSource,
        L1MissQueueUnknown,
        L1WriteQueue,
        L2EndpointPending,
        L2CacheDemand,
        L2CachePrefetchSameSource,
        L2CachePrefetchOtherSource,
        L2MissQueueDemand,
        L2MissQueuePrefetchSameSource,
        L2MissQueuePrefetchOtherSource,
        L2MissQueueUnknown,
        L2WriteQueue,
        L3EndpointPending,
        L3CacheDemand,
        L3CachePrefetchSameSource,
        L3CachePrefetchOtherSource,
        L3MissQueueDemand,
        L3MissQueuePrefetchSameSource,
        L3MissQueuePrefetchOtherSource,
        L3MissQueueUnknown,
        L3WriteQueue,
        NumPhysicalDuplicateCoverReasons
    };

    enum CacheDemandDuplicateTimingReason : unsigned
    {
        L1ResidentBeforeCandidate,
        L1DemandFillAfterCandidate,
        L1NonDemandFillAfterCandidate,
        L1FillTimeUnknown,
        L2ResidentBeforeCandidate,
        L2DemandFillAfterCandidate,
        L2NonDemandFillAfterCandidate,
        L2FillTimeUnknown,
        L3ResidentBeforeCandidate,
        L3DemandFillAfterCandidate,
        L3NonDemandFillAfterCandidate,
        L3FillTimeUnknown,
        NumCacheDemandDuplicateTimingReasons
    };

    enum CoveredPromotionReason : unsigned
    {
        L2HitPromoteToL1,
        L2HitL1MshrFull,
        L3HitPromoteToL1,
        L3HitPromoteToL2,
        L3HitL1L2MshrFull,
        NumCoveredPromotionReasons
    };

    enum QualityFeedbackEvent : unsigned
    {
        QualityAdmitted,
        QualityFilled,
        QualityUseful,
        QualityUpperConsumed,
        QualityUnused,
        QualityLate,
        QualityPollutionVictim,
        QualityDuplicateDemand,
        NumQualityFeedbackEvents
    };

    explicit CentralizedDataPrefetcher(
        const CentralizedDataPrefetcherParams &p);

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                       unsigned blk_size) override;
    void addTLB(BaseTLB *tlb, bool functional) override;
    void notifyIns(int ins_num) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late,
        PrefetchSourceType source, bool miss_repeat) override;
    bool GetPFRequestsFromBuffer(
        std::vector<AddrPriority> &addresses) override;
    PacketPtr getPacket() override;
    void notifyPrefetchResult(const PacketPtr &pkt, bool admitted) override;
    void startup() override;
    void prefetchUnused(Addr paddr,
                        const Request::XsMetadata &metadata) override;
    void prefetchLate(const Request::XsMetadata &metadata,
                      bool firstDemandMerge = true) override;
    void recordPrefetchAdmitted(
        const Request::XsMetadata &metadata) override;
    void recordPrefetchFill(const Request::XsMetadata &metadata) override;
    void recordUpperPrefetchConsumed(
        const Request::XsMetadata &metadata) override;
    void recordPrefetchFillVictim(const Request::XsMetadata &metadata,
                                  bool dirty,
                                  bool demandTouched) override;
    void recordPrefetchUseful(const Request::XsMetadata &metadata,
                              bool miss) override;

    unsigned coreId() const { return coreIdentifier; }
    void recordQualityFeedback(const Request::XsMetadata &metadata,
                               QualityFeedbackEvent event);
    void endpointAccepted(const CentralizedCompletion &completion);
    void endpointFailed(const CentralizedCompletion &completion,
                        DropReason reason);

  protected:
    void addToQueue(std::list<DeferredPacket> &queue,
                    DeferredPacket &dpp) override;

  private:
    class Translation : public BaseMMU::Translation
    {
      public:
        CentralizedDataPrefetcher *owner;
        CentralizedPrefetchCandidate candidate;
        RequestPtr request;
        ThreadContext *tc;
        bool completed;
        bool failed;
        Tick startTick;
        Tick completionTick;

        Translation(CentralizedDataPrefetcher *owner,
                    const CentralizedPrefetchCandidate &candidate,
                    const RequestPtr &request, ThreadContext *tc);
        void markDelayed() override {}
        void finish(const Fault &fault, const RequestPtr &req,
                    ThreadContext *tc, BaseMMU::Mode mode) override;
    };

    using CandidateQueue = std::multiset<CentralizedPrefetchCandidate,
                                         CentralizedCandidatePriority>;

    void captureCandidates(std::vector<AddrPriority> &addresses,
                           const Base::PFtriggerInfo *fallback);
    void enqueueCandidate(const AddrPriority &command,
                          const Base::PFtriggerInfo &trigger);
    uint8_t preferredLevel(uint32_t distance) const;
    void dispatch();
    bool dispatchCandidate(CentralizedPrefetchCandidate &candidate,
                           unsigned first_level = 0);
    bool dispatchCandidateDynamic(CentralizedPrefetchCandidate &candidate);
    bool resolvePaddr(CentralizedPrefetchCandidate &candidate);
    bool startTranslation(const CentralizedPrefetchCandidate &candidate);
    void translationFinished();
    void processTranslations();
    bool injectLocal(CentralizedPrefetchCandidate &candidate);
    bool canPromoteToL1(const CentralizedPrefetchCandidate &candidate) const;
    bool canPromoteToL2(const CentralizedPrefetchCandidate &candidate);
    bool injectAtLevel(CentralizedPrefetchCandidate &candidate,
                       unsigned level);
    bool canInjectAtLevel(const CentralizedPrefetchCandidate &candidate,
                          unsigned level);
    bool handleCoveredPromotion(
        CentralizedPrefetchCandidate &candidate,
        PhysicalDuplicateCoverReason cover_reason);
    Request::XsMetadata metadataForCandidate(
        const CentralizedPrefetchCandidate &candidate) const;
    void recordDispatch(const CentralizedPrefetchCandidate &candidate);
    void recordCoveredPromotion(
        const CentralizedPrefetchCandidate &candidate,
        CoveredPromotionReason reason);
    void recordPhysicalDuplicate(
        const CentralizedPrefetchCandidate &candidate,
        PhysicalDuplicateCoverReason reason,
        PrefetchSourceType covering_source = PF_NONE);
    void recordCacheDemandDuplicateTiming(
        const CentralizedPrefetchCandidate &candidate,
        PhysicalDuplicateCoverReason reason);
    void dropCandidate(const CentralizedPrefetchCandidate &candidate,
                       DropReason reason, bool erase_pending);
    void failCompletion(const CentralizedCandidateKey &key,
                        const CentralizedPhysicalKey &physical_key,
                        PrefetchSourceType source, DropReason reason,
                        bool cross_page);
    CentralizedCandidateKey keyFromPfi(const PrefetchInfo &pfi,
                                       Addr addr) const;
    CentralizedCandidateKey keyFromPacket(const PacketPtr &pkt) const;
    CentralizedPhysicalKey physicalKey(Addr paddr, bool secure) const;
    bool registerPhysical(CentralizedPrefetchCandidate &candidate);
    PhysicalDuplicateCoverReason physicalDuplicateCoverReason(
        const CentralizedPrefetchCandidate &candidate) const;
    PhysicalDuplicateCoverReason cacheCoverReason(
        const CacheAccessor *cache, Addr paddr, bool secure,
        PrefetchSourceType source, PhysicalDuplicateCoverReason demand_reason,
        PhysicalDuplicateCoverReason same_source_reason,
        PhysicalDuplicateCoverReason other_source_reason,
        PhysicalDuplicateCoverReason miss_queue_demand_reason,
        PhysicalDuplicateCoverReason miss_queue_same_source_reason,
        PhysicalDuplicateCoverReason miss_queue_other_source_reason,
        PhysicalDuplicateCoverReason miss_queue_unknown_reason,
        PhysicalDuplicateCoverReason write_queue_reason) const;
    PhysicalDuplicateCoverReason endpointCoverReason(
        const CentralizedPrefetcherEndpoint *endpoint, Addr paddr,
        bool secure, PrefetchSourceType source,
        PhysicalDuplicateCoverReason pending_reason,
        PhysicalDuplicateCoverReason demand_reason,
        PhysicalDuplicateCoverReason same_source_reason,
        PhysicalDuplicateCoverReason other_source_reason,
        PhysicalDuplicateCoverReason miss_queue_demand_reason,
        PhysicalDuplicateCoverReason miss_queue_same_source_reason,
        PhysicalDuplicateCoverReason miss_queue_other_source_reason,
        PhysicalDuplicateCoverReason miss_queue_unknown_reason,
        PhysicalDuplicateCoverReason write_queue_reason) const;
    struct QualityKey
    {
        uint8_t source = 0;
        uint8_t distanceBucket = 0;
        uint8_t preferredLevel = 0;
        uint16_t triggerPcHash = 0;

        bool operator==(const QualityKey &other) const
        {
            return source == other.source &&
                   distanceBucket == other.distanceBucket &&
                   preferredLevel == other.preferredLevel &&
                   triggerPcHash == other.triggerPcHash;
        }
    };

    struct QualityEntry
    {
        bool valid = false;
        QualityKey key;
        int16_t score = 0;
        uint16_t admitted = 0;
        uint16_t useful = 0;
        uint16_t unused = 0;
        uint16_t late = 0;
        uint16_t pollution = 0;
        uint16_t duplicateDemand = 0;
        uint16_t hotness = 0;
        uint32_t hotnessEpoch = 0;
        uint32_t age = 0;
    };

    void deferPhysicalRelease(const CentralizedPhysicalKey &key);
    void processPhysicalReleases();
    QualityKey qualityKeyFromCandidate(
        const CentralizedPrefetchCandidate &candidate) const;
    QualityKey qualityKeyFromMetadata(
        const Request::XsMetadata &metadata) const;
    size_t qualitySetIndex(const QualityKey &key) const;
    QualityEntry &qualityEntryForUpdate(const QualityKey &key);
    const QualityEntry *findQualityEntry(const QualityKey &key);
    void updateQuality(const QualityKey &key, QualityFeedbackEvent event,
                       uint8_t level);
    void decayHotness(QualityEntry &entry);
    void bumpHotness(QualityEntry &entry, unsigned amount);
    void observeQualityHotness(const QualityKey &key, unsigned amount);
    unsigned qualityHotness(const QualityEntry *entry) const;
    unsigned hotnessPreferredLevel(const CentralizedPrefetchCandidate &candidate,
                                   const QualityEntry *entry,
                                   unsigned deepest) const;
    int qualityScore(const CentralizedPrefetchCandidate &candidate);
    int levelAdmissionThreshold(unsigned level) const;
    bool dynamicL1Blocked(int score) const;
    bool dynamicL1Eligible(const CentralizedPrefetchCandidate &candidate,
                           int score, bool cmc_near, bool hot_l1);
    bool cmcNearCandidate(const CentralizedPrefetchCandidate &candidate,
                          const QualityEntry *entry) const;
    CentralizedSourceRecentKey sourceRecentKey(
        const CentralizedPrefetchCandidate &candidate, bool physical) const;
    bool sourceRecentFilterHit(
        const CentralizedPrefetchCandidate &candidate, bool physical);
    void insertSourceRecent(
        const CentralizedSourceRecentKey &key, bool physical);
    void bindManagedChildFilter(Base *prefetcher);
    size_t centralOccupancy() const;
    size_t localQueueOccupancy() const;

    CentralizedPrefetcherEndpoint *l2Endpoint;
    CentralizedPrefetcherEndpoint *l3Endpoint;
    const unsigned centralQueueSize;
    const unsigned dispatchWidth;
    const unsigned l1DistanceThreshold;
    const unsigned l2DistanceThreshold;
    const unsigned l1MinMshrCredits;
    const unsigned coreIdentifier;
    const bool trainOnStore;
    const bool enableDynamicDispatcher;
    const unsigned qualityTableEntries;
    const unsigned qualityTableAssoc;
    const unsigned qualityTableSets;
    const int qualityInitialScore;
    const int qualityMaxScore;
    const int qualityL1Threshold;
    const int qualityL2Threshold;
    const int qualityDropThreshold;
    const int qualityUsefulWeight;
    const int qualityUnusedWeight;
    const int qualityLateWeight;
    const int qualityDuplicateDemandWeight;
    const unsigned qualityHotnessMax;
    const unsigned qualityHotnessObservationWeight;
    const unsigned qualityHotnessUsefulWeight;
    const unsigned qualityHotnessLateWeight;
    const unsigned qualityHotnessDecayPeriod;
    const unsigned qualityHotnessL1Threshold;
    const unsigned qualityHotnessL2Threshold;
    const unsigned cmcNearDistance;
    const int cmcFarL1Threshold;
    const int l1PollutionThreshold;
    const int l1PollutionBypassThreshold;
    const int l1PollutionUnusedWeight;
    const int l1PollutionUsefulWeight;
    const int l1PollutionDecay;
    const unsigned duplicateFilterEntries;
    CandidateQueue candidates;
    std::unordered_set<CentralizedCandidateKey,
                       CentralizedCandidateKeyHash> pending;
    std::unordered_set<CentralizedPhysicalKey,
                       CentralizedPhysicalKeyHash> pendingPhysical;
    std::unordered_map<CentralizedPhysicalKey, CentralizedCompletion,
                       CentralizedPhysicalKeyHash> localCompletions;
    std::list<std::unique_ptr<Translation>> translations;
    std::deque<std::pair<Tick, CentralizedPhysicalKey>> physicalReleases;
    std::vector<Base *> managedPrefetchers;
    boost::compute::detail::lru_cache<Addr, Addr> managedChildFilter;
    std::vector<QualityEntry> qualityTable;
    uint32_t qualityAgeCounter;
    uint32_t qualityHotnessCounter;
    std::unordered_set<CentralizedSourceRecentKey,
                       CentralizedSourceRecentKeyHash> recentVirtual;
    std::deque<CentralizedSourceRecentKey> recentVirtualOrder;
    std::unordered_set<CentralizedSourceRecentKey,
                       CentralizedSourceRecentKeyHash> recentPhysical;
    std::deque<CentralizedSourceRecentKey> recentPhysicalOrder;
    int32_t l1PollutionScore;
    uint64_t nextSequence;
    EventFunctionWrapper dispatchEvent;

    struct CentralStats : public statistics::Group
    {
        CentralStats(statistics::Group *parent, unsigned queue_size);
        statistics::Scalar generated;
        statistics::Scalar dispatched;
        statistics::Scalar issued;
        statistics::Scalar dropped;
        statistics::Scalar translationRequests;
        statistics::Vector generatedBySource;
        statistics::Vector dispatchedBySource;
        statistics::Vector admittedBySource;
        statistics::Vector issuedBySource;
        statistics::Vector2d droppedBySourceAndReason;
        statistics::Vector2d generatedBySourceAndPreferredLevel;
        statistics::Vector2d nativeLevelBySource;
        statistics::Vector nativeLevelMismatchBySource;
        statistics::Vector2d preferredToNativeLevel;
        statistics::Vector translationRequestsBySource;
        statistics::Vector translationSuccessBySource;
        statistics::Vector translationFailureBySource;
        statistics::Vector translationQueueWaitTicksBySource;
        statistics::Vector translationLatencyTicksBySource;
        statistics::Histogram translationQueueWait;
        statistics::Histogram translationLatency;
        statistics::Vector preferredLevel;
        statistics::Vector actualLevel;
        statistics::Vector dropReason;
        statistics::Vector distanceBucket;
        statistics::Vector2d preferredToActual;
        statistics::Vector2d crossPageGenerated;
        statistics::Vector2d crossPageDispatched;
        statistics::Vector2d crossPageIssued;
        statistics::Vector crossPageDroppedByReason;
        statistics::Vector2d physicalDuplicateBySourceAndCover;
        statistics::Vector2d physicalDuplicateCacheDemandBySourceAndTiming;
        statistics::Vector2d coveredPromotionBySourceAndReason;
        statistics::Vector qualityFeedbackByEvent;
        statistics::Vector dynamicSelectedLevel;
        statistics::Vector dynamicRejectedLevel;
        statistics::Scalar dynamicL1PollutionRejects;
        statistics::Scalar dynamicCmcNearL1;
        statistics::Scalar dynamicCmcFarDeep;
        statistics::Scalar dynamicL1QualityRejects;
        statistics::Scalar dynamicL1DistanceRejects;
        statistics::Scalar cmcGenerated;
        statistics::Scalar cmcEarlySuppressed;
        statistics::Vector cmcDynamicAdmittedLevel;
        statistics::Scalar sourceEarlyDuplicateDrop;
        statistics::Vector2d sourceEarlyDuplicateBySource;
        statistics::Scalar qualityTableLookups;
        statistics::Scalar qualityTableHits;
        statistics::Scalar qualityTableMisses;
        statistics::Scalar qualityTableReplacements;
        statistics::Vector qualityTableReplacementsBySource;
        statistics::Scalar qualityHotnessObservations;
        statistics::Scalar qualityHotnessDecays;
        statistics::Vector dynamicHotnessPreferredLevel;
        statistics::Distribution qualityHotnessSamples;
        statistics::Distribution l1PollutionScoreSamples;
        statistics::Distribution queueOccupancy;
    } centralStats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_CENTRALIZED_HH__
