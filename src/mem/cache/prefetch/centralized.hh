#ifndef __MEM_CACHE_PREFETCH_CENTRALIZED_HH__
#define __MEM_CACHE_PREFETCH_CENTRALIZED_HH__

#include <deque>
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
    uint64_t sequence;
    CentralizedCandidateKey key;
    CentralizedPhysicalKey physicalKey;
    Base::PFtriggerInfo trigger;

    CentralizedPrefetchCandidate(
        Addr vaddr, int32_t priority, int depth, PrefetchSourceType source,
        uint32_t distance, uint8_t preferred_level, uint64_t sequence,
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
    explicit CentralizedPrefetcherEndpoint(
        const CentralizedPrefetcherEndpointParams &p);

    void notify(const PacketPtr &, const PrefetchInfo &) override {}
    void calculatePrefetch(const PrefetchInfo &,
                           std::vector<AddrPriority> &) override {}

    void registerEngine(CentralizedDataPrefetcher *owner);
    bool canAccept(const CentralizedDataPrefetcher *owner, Addr paddr) const;
    bool contains(Addr paddr, bool secure) const;
    bool enqueue(const CentralizedPrefetchCandidate &candidate,
                 CentralizedDataPrefetcher *owner);
    PacketPtr getPacket() override;
    void notifyPrefetchResult(const PacketPtr &pkt, bool admitted) override;
    void startup() override;

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
    void arbitrate();
    void scheduleArbitration();
    OwnerIngress *selectIngress();
    size_t totalOccupancy() const;

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

    explicit CentralizedDataPrefetcher(
        const CentralizedDataPrefetcherParams &p);

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late,
        PrefetchSourceType source, bool miss_repeat) override;
    bool GetPFRequestsFromBuffer(
        std::vector<AddrPriority> &addresses) override;
    PacketPtr getPacket() override;
    void notifyPrefetchResult(const PacketPtr &pkt, bool admitted) override;
    void startup() override;

    unsigned coreId() const { return coreIdentifier; }
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
    bool resolvePaddr(CentralizedPrefetchCandidate &candidate);
    bool startTranslation(const CentralizedPrefetchCandidate &candidate);
    void translationFinished();
    void processTranslations();
    bool injectLocal(CentralizedPrefetchCandidate &candidate);
    void recordDispatch(const CentralizedPrefetchCandidate &candidate);
    void dropCandidate(const CentralizedPrefetchCandidate &candidate,
                       DropReason reason, bool erase_pending);
    void failCompletion(const CentralizedCandidateKey &key,
                        const CentralizedPhysicalKey &physical_key,
                        PrefetchSourceType source, DropReason reason);
    CentralizedCandidateKey keyFromPfi(const PrefetchInfo &pfi,
                                       Addr addr) const;
    CentralizedCandidateKey keyFromPacket(const PacketPtr &pkt) const;
    CentralizedPhysicalKey physicalKey(Addr paddr, bool secure) const;
    bool registerPhysical(CentralizedPrefetchCandidate &candidate);
    void deferPhysicalRelease(const CentralizedPhysicalKey &key);
    void processPhysicalReleases();
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
    CandidateQueue candidates;
    std::unordered_set<CentralizedCandidateKey,
                       CentralizedCandidateKeyHash> pending;
    std::unordered_set<CentralizedPhysicalKey,
                       CentralizedPhysicalKeyHash> pendingPhysical;
    std::unordered_map<CentralizedPhysicalKey, CentralizedCompletion,
                       CentralizedPhysicalKeyHash> localCompletions;
    std::list<std::unique_ptr<Translation>> translations;
    std::deque<std::pair<Tick, CentralizedPhysicalKey>> physicalReleases;
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
        statistics::Vector issuedBySource;
        statistics::Vector preferredLevel;
        statistics::Vector actualLevel;
        statistics::Vector dropReason;
        statistics::Vector distanceBucket;
        statistics::Vector2d preferredToActual;
        statistics::Distribution queueOccupancy;
    } centralStats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_CENTRALIZED_HH__
