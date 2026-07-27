#include "mem/cache/prefetch/centralized.hh"

#include <algorithm>
#include <limits>

#include "mem/cache/prefetch/berti.hh"
#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/despacito_stream.hh"
#include "mem/cache/prefetch/ipcp.hh"
#include "mem/cache/prefetch/opt.hh"
#include "mem/cache/prefetch/xs_stream.hh"
#include "mem/cache/prefetch/xs_stride.hh"
#include "base/logging.hh"
#include "sim/system.hh"

namespace gem5
{
namespace prefetch
{

namespace
{

unsigned
sourceIndex(PrefetchSourceType source)
{
    return static_cast<unsigned>(source);
}

unsigned
distanceBucket(uint32_t distance)
{
    if (distance <= 8) {
        return 0;
    }
    if (distance <= 32) {
        return 1;
    }
    if (distance <= 128) {
        return 2;
    }
    if (distance <= 512) {
        return 3;
    }
    if (distance <= 2048) {
        return 4;
    }
    return 5;
}

uint16_t
pcHashFromTrigger(const Base::PFtriggerInfo &trigger)
{
    if (!trigger.pfi_old || !trigger.pfi_old->hasPC()) {
        return 0;
    }
    Addr pc = trigger.pfi_old->getPC();
    pc ^= pc >> 32;
    pc ^= pc >> 16;
    return static_cast<uint16_t>(pc);
}

uint16_t
saturatingInc(uint16_t value, unsigned max_value, unsigned amount = 1)
{
    return static_cast<uint16_t>(
        std::min<unsigned>(value + amount, max_value));
}

int
clampScore(int value, int max_value)
{
    return std::max(0, std::min(value, max_value));
}

} // anonymous namespace

size_t
CentralizedCandidateKeyHash::operator()(
    const CentralizedCandidateKey &key) const
{
    size_t hash = std::hash<Addr>{}(key.block);
    hash ^= std::hash<ContextID>{}(key.context) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);
    hash ^= static_cast<size_t>(key.contextValid) << 1;
    hash ^= static_cast<size_t>(key.secure);
    return hash;
}

size_t
CentralizedPhysicalKeyHash::operator()(
    const CentralizedPhysicalKey &key) const
{
    return std::hash<Addr>{}(key.block) ^ static_cast<size_t>(key.secure);
}

size_t
CentralizedSourceRecentKeyHash::operator()(
    const CentralizedSourceRecentKey &key) const
{
    size_t hash = std::hash<Addr>{}(key.block);
    hash ^= std::hash<ContextID>{}(key.context) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);
    hash ^= static_cast<size_t>(key.contextValid) << 1;
    hash ^= static_cast<size_t>(key.secure) << 2;
    hash ^= static_cast<size_t>(key.source) << 8;
    hash ^= static_cast<size_t>(key.distanceBucket) << 16;
    hash ^= static_cast<size_t>(key.triggerPcHash) << 24;
    return hash;
}

CentralizedPrefetchCandidate::CentralizedPrefetchCandidate(
    Addr vaddr, int32_t priority, int depth, PrefetchSourceType source,
    uint32_t distance, uint8_t preferred_level, uint64_t sequence,
    bool cross_page, Tick enqueue_tick,
    const CentralizedCandidateKey &key,
    const Base::PFtriggerInfo &trigger)
  : vaddr(vaddr), paddr(0), paddrValid(false), physicalRegistered(false),
    priority(priority), depth(depth), source(source), distance(distance),
    preferredLevel(preferred_level), actualLevel(0),
    crossPage(cross_page),
    triggerPcValid(trigger.pfi_old && trigger.pfi_old->hasPC()),
    triggerPcHash(pcHashFromTrigger(trigger)), centralizedCoreId(0),
    sequence(sequence),
    enqueueTick(enqueue_tick),
    key(key), physicalKey{0, false}, trigger(trigger)
{
}

bool
CentralizedCandidatePriority::operator()(
    const CentralizedPrefetchCandidate &lhs,
    const CentralizedPrefetchCandidate &rhs) const
{
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    return lhs.sequence < rhs.sequence;
}

CentralizedCompletion::CentralizedCompletion(
    CentralizedDataPrefetcher *owner,
    const CentralizedPrefetchCandidate &candidate)
  : owner(owner), candidate(candidate), key(candidate.key),
    physicalKey(candidate.physicalKey),
    source(candidate.source), preferredLevel(candidate.preferredLevel),
    actualLevel(candidate.actualLevel)
{
}

CentralizedPrefetcherEndpoint::EndpointStats::EndpointStats(
    statistics::Group *parent, unsigned queue_size)
  : statistics::Group(parent),
    ADD_STAT(accepted, statistics::units::Count::get(),
             "candidates accepted from central engines"),
    ADD_STAT(issued, statistics::units::Count::get(),
             "prefetch packets issued"),
    ADD_STAT(rejectedFull, statistics::units::Count::get(),
             "candidates rejected by total capacity"),
    ADD_STAT(rejectedPerCoreFull, statistics::units::Count::get(),
             "candidates rejected by per-core capacity"),
    ADD_STAT(rejectedCredit, statistics::units::Count::get(),
             "candidates rejected by MSHR credits"),
    ADD_STAT(rejectedDuplicate, statistics::units::Count::get(),
             "duplicate physical candidates rejected"),
    ADD_STAT(rejectedInsert, statistics::units::Count::get(),
             "candidates rejected by cache/MSHR insertion checks"),
    ADD_STAT(issueCreditDrop, statistics::units::Count::get(),
             "queued packets dropped by issue credit recheck"),
    ADD_STAT(rrSelections, statistics::units::Count::get(),
             "per-core round-robin selections"),
    ADD_STAT(acceptedBySource, statistics::units::Count::get(),
             "candidates accepted into endpoint ingress by source"),
    ADD_STAT(admittedBySource, statistics::units::Count::get(),
             "endpoint prefetches admitted by the cache by source"),
    ADD_STAT(rejectedBySourceAndReason, statistics::units::Count::get(),
             "endpoint candidate rejections by source and reason"),
    ADD_STAT(queueOccupancy, statistics::units::Count::get(),
             "total endpoint queue occupancy")
{
    using namespace statistics;
    acceptedBySource.init(NUM_PF_SOURCES).flags(total);
    admittedBySource.init(NUM_PF_SOURCES).flags(total);
    rejectedBySourceAndReason.init(NUM_PF_SOURCES, NumRejectReasons);
    rejectedBySourceAndReason
        .ysubname(InvalidCandidate, "invalid_candidate")
        .ysubname(TotalQueueFull, "total_queue_full")
        .ysubname(PerCoreQueueFull, "per_core_queue_full")
        .ysubname(EnqueueCredit, "enqueue_mshr_credit")
        .ysubname(EnqueueDuplicate, "enqueue_duplicate")
        .ysubname(ArbitrationInsert, "arbitration_insert")
        .ysubname(IssueCredit, "issue_mshr_credit")
        .ysubname(IssueDuplicate, "issue_duplicate")
        .ysubname(CacheAdmission, "cache_admission");
    queueOccupancy.init(0, queue_size, 1);
}

void
CentralizedPrefetcherEndpoint::recordReject(
    PrefetchSourceType source, RejectReason reason)
{
    const unsigned src = sourceIndex(source);
    if (src < NUM_PF_SOURCES) {
        endpointStats.rejectedBySourceAndReason[src][reason]++;
    }
}

CentralizedPrefetcherEndpoint::CentralizedPrefetcherEndpoint(
    const CentralizedPrefetcherEndpointParams &p)
  : Queued(p), cacheLevel(p.cache_level),
    minMshrCredits(p.min_mshr_credits),
    perCoreQueueSize(p.per_core_queue_size),
    arbitrationWidth(p.arbitration_width), rrCursor(0),
    ingressOccupancy(0),
    arbitrationEvent([this] { arbitrate(); }, name()),
    endpointStats(this, p.queue_size)
{
    fatal_if(cacheLevel < 2 || cacheLevel > 3,
             "%s: endpoint level must be L2 or L3", name());
    fatal_if(perCoreQueueSize == 0 || arbitrationWidth == 0,
             "%s: queue size and arbitration width must be non-zero",
             name());
}

void
CentralizedPrefetcherEndpoint::registerEngine(
    CentralizedDataPrefetcher *owner)
{
    fatal_if(!owner, "%s: cannot register a null engine", name());
    const unsigned id = owner->coreId();
    auto [it, inserted] = ingress.emplace(id, OwnerIngress(owner));
    fatal_if(!inserted && it->second.owner != owner,
             "%s: duplicate centralized-prefetch core_id %u", name(), id);
    if (inserted) {
        rrCoreIds.insert(
            std::lower_bound(rrCoreIds.begin(), rrCoreIds.end(), id), id);
        rrCursor = 0;
    }
}

size_t
CentralizedPrefetcherEndpoint::totalOccupancy() const
{
    return ingressOccupancy + pfq.size() +
           pfqMissingTranslation.size() + handedOffIndex.size();
}

bool
CentralizedPrefetcherEndpoint::canAccept(
    const CentralizedDataPrefetcher *owner, Addr paddr,
    PrefetchSourceType source, bool countReject)
{
    if (!cache || !owner) {
        if (countReject) {
            recordReject(source, InvalidCandidate);
        }
        return false;
    }
    if (totalOccupancy() >= queueSize) {
        if (countReject) {
            recordReject(source, TotalQueueFull);
        }
        return false;
    }
    if (cache->prefetchMshrCredits(paddr) < minMshrCredits) {
        if (countReject) {
            recordReject(source, EnqueueCredit);
        }
        return false;
    }
    const auto it = ingress.find(owner->coreId());
    if (it == ingress.end() || it->second.owner != owner) {
        if (countReject) {
            recordReject(source, InvalidCandidate);
        }
        return false;
    }
    if (it->second.queue.size() >= perCoreQueueSize) {
        if (countReject) {
            recordReject(source, PerCoreQueueFull);
        }
        return false;
    }
    return true;
}

CentralizedPhysicalKey
CentralizedPrefetcherEndpoint::physicalKey(Addr paddr, bool secure) const
{
    return {blockAddress(paddr), secure};
}

CentralizedPrefetcherEndpoint::CoverKind
CentralizedPrefetcherEndpoint::coverKind(
    Addr paddr, bool secure, PrefetchSourceType source) const
{
    const auto key = physicalKey(paddr, secure);
    if (completions.find(key) != completions.end() ||
        handedOffIndex.find(key) != handedOffIndex.end()) {
        return EndpointPendingCover;
    }
    if (!cache) {
        return NoCover;
    }

    Request::XsMetadata metadata;
    if (cache->getHitBlkXsMetadata(key.block, secure, metadata)) {
        if (metadata.validXsMetadata && metadata.prefetchSource != PF_NONE) {
            return metadata.prefetchSource == source ?
                CachePrefetchSameSourceCover : CachePrefetchOtherSourceCover;
        }
        return CacheDemandCover;
    }
    Request::XsMetadata mshr_metadata;
    bool mshr_has_cpu = false;
    bool mshr_has_pref = false;
    if (cache->getMissQueueXsMetadata(
            key.block, secure, mshr_metadata,
            mshr_has_cpu, mshr_has_pref)) {
        if (mshr_has_cpu) {
            return MissQueueDemandCover;
        }
        if (mshr_has_pref && mshr_metadata.validXsMetadata &&
            mshr_metadata.prefetchSource != PF_NONE) {
            return mshr_metadata.prefetchSource == source ?
                MissQueuePrefetchSameSourceCover :
                MissQueuePrefetchOtherSourceCover;
        }
        return MissQueueUnknownCover;
    }
    if (cache->inWriteQueue(key.block, secure)) {
        return WriteQueueCover;
    }
    return NoCover;
}

bool
CentralizedPrefetcherEndpoint::contains(Addr paddr, bool secure) const
{
    return coverKind(paddr, secure, PF_NONE) != NoCover;
}

bool
CentralizedPrefetcherEndpoint::cacheFillInfo(
    Addr paddr, bool secure, Tick &fill_tick, bool &had_demand) const
{
    return cache && cache->getHitBlkFillInfo(
        physicalKey(paddr, secure).block, secure, fill_tick, had_demand);
}

bool
CentralizedPrefetcherEndpoint::enqueue(
    const CentralizedPrefetchCandidate &candidate,
    CentralizedDataPrefetcher *owner)
{
    if (!owner || !candidate.paddrValid ||
        candidate.actualLevel != cacheLevel) {
        endpointStats.rejectedInsert++;
        recordReject(candidate.source, InvalidCandidate);
        return false;
    }
    auto ingress_it = ingress.find(owner->coreId());
    fatal_if(ingress_it == ingress.end() ||
             ingress_it->second.owner != owner,
             "%s: unregistered engine/core-id pair", name());
    if (totalOccupancy() >= queueSize) {
        endpointStats.rejectedFull++;
        recordReject(candidate.source, TotalQueueFull);
        return false;
    }
    if (ingress_it->second.queue.size() >= perCoreQueueSize) {
        endpointStats.rejectedPerCoreFull++;
        recordReject(candidate.source, PerCoreQueueFull);
        return false;
    }
    if (!cache ||
        cache->prefetchMshrCredits(candidate.paddr) < minMshrCredits) {
        endpointStats.rejectedCredit++;
        recordReject(candidate.source, EnqueueCredit);
        return false;
    }
    if (contains(candidate.paddr, candidate.key.secure)) {
        endpointStats.rejectedDuplicate++;
        recordReject(candidate.source, EnqueueDuplicate);
        return false;
    }
    const bool inserted = completions.emplace(
        candidate.physicalKey,
        CentralizedCompletion(owner, candidate)).second;
    if (!inserted) {
        endpointStats.rejectedDuplicate++;
        recordReject(candidate.source, EnqueueDuplicate);
        return false;
    }
    ingress_it->second.queue.push_back(candidate);
    ingressOccupancy++;
    endpointStats.accepted++;
    if (sourceIndex(candidate.source) < NUM_PF_SOURCES) {
        endpointStats.acceptedBySource[sourceIndex(candidate.source)]++;
    }
    endpointStats.queueOccupancy.sample(totalOccupancy());
    scheduleArbitration();
    return true;
}

void
CentralizedPrefetcherEndpoint::scheduleArbitration()
{
    if (ingressOccupancy && !arbitrationEvent.scheduled()) {
        schedule(arbitrationEvent, nextCycle());
    }
}

CentralizedPrefetcherEndpoint::OwnerIngress *
CentralizedPrefetcherEndpoint::selectIngress()
{
    if (rrCoreIds.empty() || !ingressOccupancy) {
        return nullptr;
    }
    for (size_t offset = 0; offset < rrCoreIds.size(); ++offset) {
        const size_t idx = (rrCursor + offset) % rrCoreIds.size();
        auto &entry = ingress.at(rrCoreIds[idx]);
        if (!entry.queue.empty()) {
            rrCursor = (idx + 1) % rrCoreIds.size();
            return &entry;
        }
    }
    return nullptr;
}

bool
CentralizedPrefetcherEndpoint::insertReady(
    const CentralizedPrefetchCandidate &candidate)
{
    if (!candidate.trigger.pfi_old || !system->isMemAddr(candidate.paddr)) {
        return false;
    }
    if (cacheSnoop && queueFilter &&
        (inCache(candidate.paddr, candidate.key.secure) ||
         inMissQueue(candidate.paddr, candidate.key.secure) ||
         cache->inWriteQueue(candidate.paddr, candidate.key.secure))) {
        statsQueued.pfInCache++;
        return false;
    }

    PrefetchInfo trigger_pfi(*candidate.trigger.pfi_old);
    PrefetchInfo new_pfi(trigger_pfi, candidate.vaddr);
    new_pfi.setXsMetadata(Request::XsMetadata(
        candidate.source, candidate.depth, candidate.distance,
        candidate.preferredLevel, candidate.actualLevel,
        candidate.crossPage, true, candidate.centralizedCoreId,
        candidate.triggerPcHash));
    DeferredPacket dpp(this, new_pfi, 0, candidate.priority);
    dpp.pfahead = false;
    dpp.pfahead_host = 0;
    const Tick ready = curTick() + clockPeriod() * latency;
    dpp.createPkt(candidate.paddr, blkSize, requestorId, tagPrefetch,
                  ready, candidate.source, candidate.depth,
                  candidate.distance, candidate.preferredLevel,
                  candidate.actualLevel, candidate.crossPage);
    dpp.pkt->req->setXsMetadata(new_pfi.getXsMetadata());
    const size_t before = pfq.size();
    statsQueued.pfIdentified++;
    addToQueue(pfq, dpp);
    return pfq.size() > before;
}

void
CentralizedPrefetcherEndpoint::failCompletion(
    const CentralizedPhysicalKey &key, bool issue_credit_drop,
    bool duplicate_drop)
{
    auto it = completions.find(key);
    fatal_if(it == completions.end(),
             "%s: missing centralized completion for %#x", name(),
             key.block);
    const CentralizedCompletion completion = it->second;
    completions.erase(it);
    completion.owner->endpointFailed(
        completion,
        issue_credit_drop ? CentralizedDataPrefetcher::IssueCreditDrop :
        duplicate_drop ? CentralizedDataPrefetcher::PhysicalDuplicate :
                         CentralizedDataPrefetcher::EndpointRejected);
}

void
CentralizedPrefetcherEndpoint::arbitrate()
{
    for (unsigned count = 0;
         count < arbitrationWidth && ingressOccupancy; ++count) {
        OwnerIngress *selected = selectIngress();
        fatal_if(!selected, "%s: ingress accounting mismatch", name());
        CentralizedPrefetchCandidate candidate = selected->queue.front();
        selected->queue.pop_front();
        ingressOccupancy--;
        endpointStats.rrSelections++;
        if (!cache ||
            cache->prefetchMshrCredits(candidate.paddr) < minMshrCredits) {
            endpointStats.issueCreditDrop++;
            recordReject(candidate.source, IssueCredit);
            failCompletion(candidate.physicalKey, true, false);
            continue;
        }
        if (!insertReady(candidate)) {
            endpointStats.rejectedInsert++;
            recordReject(candidate.source, ArbitrationInsert);
            failCompletion(candidate.physicalKey, false, false);
        }
    }
    endpointStats.queueOccupancy.sample(totalOccupancy());
    scheduleArbitration();
}

void
CentralizedPrefetcherEndpoint::addToQueue(
    std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    const size_t before = queue.size();
    Queued::addToQueue(queue, dpp);
    if (&queue == &pfq && queue.size() > before && cache) {
        cache->notifyPrefetchPending();
    }
}

void
CentralizedPrefetcherEndpoint::startup()
{
    ClockedObject::startup();
    fatal_if(!cache || blkSize != 64,
             "%s: centralized endpoint requires a parent and 64-byte lines",
             name());
}

void
CentralizedPrefetcherEndpoint::routeQualityFeedback(
    const Request::XsMetadata &metadata, unsigned event)
{
    if (!metadata.centralizedPrefetch) {
        return;
    }
    const auto it = ingress.find(metadata.centralizedCoreId);
    if (it == ingress.end() || !it->second.owner) {
        return;
    }
    if (event >= CentralizedDataPrefetcher::NumQualityFeedbackEvents) {
        return;
    }
    it->second.owner->recordQualityFeedback(
        metadata,
        static_cast<CentralizedDataPrefetcher::QualityFeedbackEvent>(event));
}

void
CentralizedPrefetcherEndpoint::prefetchUnused(
    Addr paddr, const Request::XsMetadata &metadata)
{
    Base::prefetchUnused(paddr, metadata);
    routeQualityFeedback(metadata, CentralizedDataPrefetcher::QualityUnused);
}

void
CentralizedPrefetcherEndpoint::prefetchLate(
    const Request::XsMetadata &metadata, bool firstDemandMerge)
{
    Base::prefetchLate(metadata, firstDemandMerge);
    routeQualityFeedback(metadata, CentralizedDataPrefetcher::QualityLate);
}

void
CentralizedPrefetcherEndpoint::recordPrefetchAdmitted(
    const Request::XsMetadata &metadata)
{
    Base::recordPrefetchAdmitted(metadata);
    routeQualityFeedback(metadata, CentralizedDataPrefetcher::QualityAdmitted);
}

void
CentralizedPrefetcherEndpoint::recordPrefetchFill(
    const Request::XsMetadata &metadata)
{
    Base::recordPrefetchFill(metadata);
    routeQualityFeedback(metadata, CentralizedDataPrefetcher::QualityFilled);
}

void
CentralizedPrefetcherEndpoint::recordUpperPrefetchConsumed(
    const Request::XsMetadata &metadata)
{
    Base::recordUpperPrefetchConsumed(metadata);
    routeQualityFeedback(metadata,
                         CentralizedDataPrefetcher::QualityUpperConsumed);
}

void
CentralizedPrefetcherEndpoint::recordPrefetchFillVictim(
    const Request::XsMetadata &metadata, bool dirty, bool demandTouched)
{
    Base::recordPrefetchFillVictim(metadata, dirty, demandTouched);
    if (!demandTouched) {
        routeQualityFeedback(
            metadata, CentralizedDataPrefetcher::QualityPollutionVictim);
    }
}

void
CentralizedPrefetcherEndpoint::recordPrefetchUseful(
    const Request::XsMetadata &metadata, bool)
{
    routeQualityFeedback(metadata, CentralizedDataPrefetcher::QualityUseful);
}

PacketPtr
CentralizedPrefetcherEndpoint::getPacket()
{
    while (!pfq.empty()) {
        if (pfq.front().tick > curTick()) {
            if (cache) {
                cache->notifyPrefetchPending();
            }
            return nullptr;
        }
        PacketPtr queued = pfq.front().pkt;
        const auto pkey =
            physicalKey(queued->getAddr(), queued->isSecure());
        if (cache &&
            (cache->inCache(queued->getAddr(), queued->isSecure()) ||
             cache->inMissQueue(queued->getAddr(), queued->isSecure()) ||
             cache->inWriteQueue(queued->getAddr(), queued->isSecure()))) {
            pfq.pop_front();
            delete queued;
            endpointStats.rejectedInsert++;
            recordReject(completions.at(pkey).source, IssueDuplicate);
            failCompletion(pkey, false, true);
            continue;
        }
        if (!cache ||
            cache->prefetchMshrCredits(queued->getAddr()) <
                minMshrCredits) {
            pfq.pop_front();
            delete queued;
            endpointStats.issueCreditDrop++;
            recordReject(completions.at(pkey).source, IssueCredit);
            failCompletion(pkey, true, false);
            continue;
        }
        PacketPtr pkt = Queued::getPacket();
        if (!pkt) {
            if (cache) {
                cache->notifyPrefetchPending();
            }
            return nullptr;
        }
        auto it = completions.find(pkey);
        fatal_if(it == completions.end(),
                 "%s: handed packet has no centralized completion for %#x",
                 name(), pkey.block);
        fatal_if(!handedOffIndex.insert(pkey).second,
                 "%s: duplicate handed-off packet for %#x",
                 name(), pkey.block);
        scheduleArbitration();
        return pkt;
    }
    scheduleArbitration();
    return nullptr;
}

void
CentralizedPrefetcherEndpoint::notifyPrefetchResult(
    const PacketPtr &pkt, bool admitted)
{
    const auto pkey = physicalKey(pkt->getAddr(), pkt->isSecure());
    fatal_if(handedOffIndex.erase(pkey) == 0,
             "%s: completed packet was not handed off for %#x",
             name(), pkey.block);
    auto it = completions.find(pkey);
    fatal_if(it == completions.end(),
             "%s: completed packet has no completion for %#x",
             name(), pkey.block);
    const CentralizedCompletion completion = it->second;
    completions.erase(it);
    if (admitted) {
        recordCrossPagePrefetchIssued(pkt->req->getXsMetadata());
        endpointStats.issued++;
        const unsigned src = sourceIndex(completion.source);
        if (src < NUM_PF_SOURCES) {
            endpointStats.admittedBySource[src]++;
        }
        completion.owner->endpointAccepted(completion);
    } else {
        endpointStats.rejectedInsert++;
        recordReject(completion.source, CacheAdmission);
        completion.owner->endpointFailed(
            completion, CentralizedDataPrefetcher::PhysicalDuplicate);
    }
    endpointStats.queueOccupancy.sample(totalOccupancy());
    scheduleArbitration();
}

CentralizedDataPrefetcher::CentralStats::CentralStats(
    statistics::Group *parent, unsigned queue_size)
  : statistics::Group(parent),
    ADD_STAT(generated, statistics::units::Count::get(),
             "central candidates generated"),
    ADD_STAT(dispatched, statistics::units::Count::get(),
             "central candidates dispatched"),
    ADD_STAT(issued, statistics::units::Count::get(),
             "central prefetches issued"),
    ADD_STAT(dropped, statistics::units::Count::get(),
             "central candidates dropped"),
    ADD_STAT(translationRequests, statistics::units::Count::get(),
             "central cross-page translations"),
    ADD_STAT(generatedBySource, statistics::units::Count::get(),
             "generated candidates by source"),
    ADD_STAT(dispatchedBySource, statistics::units::Count::get(),
             "dispatched candidates by source"),
    ADD_STAT(admittedBySource, statistics::units::Count::get(),
             "cache-admitted prefetches by source"),
    ADD_STAT(issuedBySource, statistics::units::Count::get(),
             "cache-admitted prefetches by source (legacy name)"),
    ADD_STAT(droppedBySourceAndReason, statistics::units::Count::get(),
             "dropped candidates by source and reason"),
    ADD_STAT(generatedBySourceAndPreferredLevel,
             statistics::units::Count::get(),
             "generated candidates by source and distance-derived level"),
    ADD_STAT(nativeLevelBySource, statistics::units::Count::get(),
             "producer-annotated native levels by source; level 0 is absent"),
    ADD_STAT(nativeLevelMismatchBySource, statistics::units::Count::get(),
             "valid native annotations differing from distance level"),
    ADD_STAT(preferredToNativeLevel, statistics::units::Count::get(),
             "distance-derived level to producer-native-level matrix"),
    ADD_STAT(translationRequestsBySource, statistics::units::Count::get(),
             "TLB translation requests by candidate source"),
    ADD_STAT(translationSuccessBySource, statistics::units::Count::get(),
             "successful TLB translations by candidate source"),
    ADD_STAT(translationFailureBySource, statistics::units::Count::get(),
             "failed TLB translations by candidate source"),
    ADD_STAT(translationQueueWaitTicksBySource,
             statistics::units::Tick::get(),
             "total candidate-to-TLB-request wait ticks by source"),
    ADD_STAT(translationLatencyTicksBySource,
             statistics::units::Tick::get(),
             "total TLB translation service ticks by source"),
    ADD_STAT(translationQueueWait, statistics::units::Tick::get(),
             "ticks from candidate generation to TLB request"),
    ADD_STAT(translationLatency, statistics::units::Tick::get(),
             "ticks from TLB request to translation completion"),
    ADD_STAT(preferredLevel, statistics::units::Count::get(),
             "preferred cache level"),
    ADD_STAT(actualLevel, statistics::units::Count::get(),
             "actual cache level"),
    ADD_STAT(dropReason, statistics::units::Count::get(),
             "drop reason"),
    ADD_STAT(distanceBucket, statistics::units::Count::get(),
             "distance buckets in cache lines"),
    ADD_STAT(preferredToActual, statistics::units::Count::get(),
             "preferred-to-actual level matrix"),
    ADD_STAT(crossPageGenerated, statistics::units::Count::get(),
             "cross-page candidates by source and preferred level"),
    ADD_STAT(crossPageDispatched, statistics::units::Count::get(),
             "cross-page candidates by source and actual level"),
    ADD_STAT(crossPageIssued, statistics::units::Count::get(),
             "cross-page prefetches admitted by source and actual level"),
    ADD_STAT(crossPageDroppedByReason, statistics::units::Count::get(),
             "cross-page candidates dropped by reason"),
    ADD_STAT(physicalDuplicateBySourceAndCover,
             statistics::units::Count::get(),
             "physical duplicate drops by candidate source and cover source"),
    ADD_STAT(physicalDuplicateCacheDemandBySourceAndTiming,
             statistics::units::Count::get(),
             "cache-demand physical duplicate drops by candidate source and fill timing"),
    ADD_STAT(coveredPromotionBySourceAndReason,
             statistics::units::Count::get(),
             "lower-level cache-resident candidates promoted or dropped by source and reason"),
    ADD_STAT(qualityFeedbackByEvent, statistics::units::Count::get(),
             "dynamic quality table feedback events"),
    ADD_STAT(dynamicSelectedLevel, statistics::units::Count::get(),
             "dynamic dispatcher selected target levels"),
    ADD_STAT(dynamicRejectedLevel, statistics::units::Count::get(),
             "dynamic dispatcher resource or quality rejected target levels"),
    ADD_STAT(dynamicL1PollutionRejects, statistics::units::Count::get(),
             "L1 candidates rejected by dynamic pollution guard"),
    ADD_STAT(dynamicCmcNearL1, statistics::units::Count::get(),
             "CMC candidates treated as near/high-confidence L1 candidates"),
    ADD_STAT(dynamicCmcFarDeep, statistics::units::Count::get(),
             "CMC candidates kept at distance-derived deeper levels"),
    ADD_STAT(dynamicL1QualityRejects, statistics::units::Count::get(),
             "L1 candidates rejected by dynamic quality threshold"),
    ADD_STAT(dynamicL1DistanceRejects, statistics::units::Count::get(),
             "L1 candidates rejected by dynamic distance guard"),
    ADD_STAT(cmcGenerated, statistics::units::Count::get(),
             "central candidates generated by CMC"),
    ADD_STAT(cmcEarlySuppressed, statistics::units::Count::get(),
             "CMC candidates suppressed by early duplicate filters"),
    ADD_STAT(cmcDynamicAdmittedLevel, statistics::units::Count::get(),
             "CMC candidates admitted by dynamic dispatcher level"),
    ADD_STAT(sourceEarlyDuplicateDrop, statistics::units::Count::get(),
             "source-aware recent duplicate drops before endpoint admission"),
    ADD_STAT(sourceEarlyDuplicateBySource, statistics::units::Count::get(),
             "source-aware recent duplicate drops by source and key kind"),
    ADD_STAT(qualityTableLookups, statistics::units::Count::get(),
             "dynamic quality table lookups"),
    ADD_STAT(qualityTableHits, statistics::units::Count::get(),
             "dynamic quality table lookup hits"),
    ADD_STAT(qualityTableMisses, statistics::units::Count::get(),
             "dynamic quality table lookup misses"),
    ADD_STAT(qualityTableReplacements, statistics::units::Count::get(),
             "set-associative dynamic quality table entry replacements"),
    ADD_STAT(qualityTableReplacementsBySource, statistics::units::Count::get(),
             "dynamic quality table replacements by source"),
    ADD_STAT(qualityHotnessObservations, statistics::units::Count::get(),
             "dynamic quality table hotness observation updates"),
    ADD_STAT(qualityHotnessDecays, statistics::units::Count::get(),
             "dynamic quality table lazy hotness decay steps"),
    ADD_STAT(dynamicHotnessPreferredLevel, statistics::units::Count::get(),
             "dynamic dispatcher target preference selected by hotness"),
    ADD_STAT(qualityHotnessSamples, statistics::units::Count::get(),
             "sampled dynamic quality table hotness"),
    ADD_STAT(l1PollutionScoreSamples, statistics::units::Count::get(),
             "sampled dynamic L1 pollution pressure score"),
    ADD_STAT(queueOccupancy, statistics::units::Count::get(),
             "ready plus translating candidates")
{
    using namespace statistics;
    generatedBySource.init(NUM_PF_SOURCES).flags(total);
    dispatchedBySource.init(NUM_PF_SOURCES).flags(total);
    admittedBySource.init(NUM_PF_SOURCES).flags(total);
    issuedBySource.init(NUM_PF_SOURCES).flags(total);
    droppedBySourceAndReason.init(NUM_PF_SOURCES, NumDropReasons);
    droppedBySourceAndReason
        .ysubname(Duplicate, "virtual_duplicate")
        .ysubname(CentralQueueFull, "central_queue_full")
        .ysubname(MissingTrigger, "missing_trigger")
        .ysubname(TranslationUnavailable, "translation_unavailable")
        .ysubname(TranslationFailed, "translation_failed")
        .ysubname(PhysicalDuplicate, "physical_duplicate")
        .ysubname(NoAdmission, "no_admission")
        .ysubname(EndpointRejected, "endpoint_rejected")
        .ysubname(IssueCreditDrop, "issue_credit_drop");
    generatedBySourceAndPreferredLevel.init(NUM_PF_SOURCES, 4);
    generatedBySourceAndPreferredLevel
        .ysubname(0, "unused")
        .ysubname(1, "l1")
        .ysubname(2, "l2")
        .ysubname(3, "l3");
    nativeLevelBySource.init(NUM_PF_SOURCES, 4);
    nativeLevelBySource
        .ysubname(0, "unannotated")
        .ysubname(1, "l1")
        .ysubname(2, "l2")
        .ysubname(3, "l3");
    nativeLevelMismatchBySource.init(NUM_PF_SOURCES).flags(total);
    preferredToNativeLevel.init(4, 4);
    preferredToNativeLevel
        .ysubname(0, "unannotated")
        .ysubname(1, "l1")
        .ysubname(2, "l2")
        .ysubname(3, "l3");
    translationRequestsBySource.init(NUM_PF_SOURCES).flags(total);
    translationSuccessBySource.init(NUM_PF_SOURCES).flags(total);
    translationFailureBySource.init(NUM_PF_SOURCES).flags(total);
    translationQueueWaitTicksBySource.init(NUM_PF_SOURCES).flags(total);
    translationLatencyTicksBySource.init(NUM_PF_SOURCES).flags(total);
    translationQueueWait.init(32);
    translationLatency.init(32);
    preferredLevel.init(4).flags(total);
    actualLevel.init(4).flags(total);
    dropReason.init(NumDropReasons).flags(total);
    distanceBucket.init(6).flags(total);
    preferredToActual.init(4, 4);
    crossPageGenerated.init(NUM_PF_SOURCES, 4);
    crossPageDispatched.init(NUM_PF_SOURCES, 4);
    crossPageIssued.init(NUM_PF_SOURCES, 4);
    crossPageDroppedByReason.init(NumDropReasons).flags(total);
    physicalDuplicateBySourceAndCover.init(
        NUM_PF_SOURCES, NumPhysicalDuplicateCoverReasons);
    physicalDuplicateBySourceAndCover
        .ysubname(PendingPhysicalDuplicate, "pending_physical")
        .ysubname(L1CacheDemand, "l1_cache_demand_or_unknown")
        .ysubname(L1CachePrefetchSameSource, "l1_cache_prefetch_same_source")
        .ysubname(L1CachePrefetchOtherSource, "l1_cache_prefetch_other_source")
        .ysubname(L1MissQueueDemand, "l1_mshr_demand")
        .ysubname(L1MissQueuePrefetchSameSource, "l1_mshr_prefetch_same_source")
        .ysubname(L1MissQueuePrefetchOtherSource, "l1_mshr_prefetch_other_source")
        .ysubname(L1MissQueueUnknown, "l1_mshr_unknown")
        .ysubname(L1WriteQueue, "l1_write_queue")
        .ysubname(L2EndpointPending, "l2_endpoint_pending_prefetch")
        .ysubname(L2CacheDemand, "l2_cache_demand_or_unknown")
        .ysubname(L2CachePrefetchSameSource, "l2_cache_prefetch_same_source")
        .ysubname(L2CachePrefetchOtherSource, "l2_cache_prefetch_other_source")
        .ysubname(L2MissQueueDemand, "l2_mshr_demand")
        .ysubname(L2MissQueuePrefetchSameSource, "l2_mshr_prefetch_same_source")
        .ysubname(L2MissQueuePrefetchOtherSource, "l2_mshr_prefetch_other_source")
        .ysubname(L2MissQueueUnknown, "l2_mshr_unknown")
        .ysubname(L2WriteQueue, "l2_write_queue")
        .ysubname(L3EndpointPending, "l3_endpoint_pending_prefetch")
        .ysubname(L3CacheDemand, "l3_cache_demand_or_unknown")
        .ysubname(L3CachePrefetchSameSource, "l3_cache_prefetch_same_source")
        .ysubname(L3CachePrefetchOtherSource, "l3_cache_prefetch_other_source")
        .ysubname(L3MissQueueDemand, "l3_mshr_demand")
        .ysubname(L3MissQueuePrefetchSameSource, "l3_mshr_prefetch_same_source")
        .ysubname(L3MissQueuePrefetchOtherSource, "l3_mshr_prefetch_other_source")
        .ysubname(L3MissQueueUnknown, "l3_mshr_unknown")
        .ysubname(L3WriteQueue, "l3_write_queue");
    physicalDuplicateCacheDemandBySourceAndTiming.init(
        NUM_PF_SOURCES, NumCacheDemandDuplicateTimingReasons);
    physicalDuplicateCacheDemandBySourceAndTiming
        .ysubname(L1ResidentBeforeCandidate, "l1_resident_before_candidate")
        .ysubname(L1DemandFillAfterCandidate, "l1_demand_fill_after_candidate")
        .ysubname(L1NonDemandFillAfterCandidate, "l1_non_demand_fill_after_candidate")
        .ysubname(L1FillTimeUnknown, "l1_fill_time_unknown")
        .ysubname(L2ResidentBeforeCandidate, "l2_resident_before_candidate")
        .ysubname(L2DemandFillAfterCandidate, "l2_demand_fill_after_candidate")
        .ysubname(L2NonDemandFillAfterCandidate, "l2_non_demand_fill_after_candidate")
        .ysubname(L2FillTimeUnknown, "l2_fill_time_unknown")
        .ysubname(L3ResidentBeforeCandidate, "l3_resident_before_candidate")
        .ysubname(L3DemandFillAfterCandidate, "l3_demand_fill_after_candidate")
        .ysubname(L3NonDemandFillAfterCandidate, "l3_non_demand_fill_after_candidate")
        .ysubname(L3FillTimeUnknown, "l3_fill_time_unknown");
    coveredPromotionBySourceAndReason.init(
        NUM_PF_SOURCES, NumCoveredPromotionReasons);
    coveredPromotionBySourceAndReason
        .ysubname(L2HitPromoteToL1, "l2_hit_promote_to_l1")
        .ysubname(L2HitL1MshrFull, "l2_hit_l1_mshr_full")
        .ysubname(L3HitPromoteToL1, "l3_hit_promote_to_l1")
        .ysubname(L3HitPromoteToL2, "l3_hit_promote_to_l2")
        .ysubname(L3HitL1L2MshrFull, "l3_hit_l1_l2_mshr_full");
    qualityFeedbackByEvent.init(NumQualityFeedbackEvents).flags(total);
    qualityFeedbackByEvent
        .subname(QualityAdmitted, "admitted")
        .subname(QualityFilled, "filled")
        .subname(QualityUseful, "useful")
        .subname(QualityUpperConsumed, "upper_pf_consumed")
        .subname(QualityUnused, "unused")
        .subname(QualityLate, "late")
        .subname(QualityPollutionVictim, "pollution_victim")
        .subname(QualityDuplicateDemand, "duplicate_demand");
    dynamicSelectedLevel.init(4).flags(total);
    dynamicSelectedLevel
        .subname(0, "dropped")
        .subname(1, "l1")
        .subname(2, "l2")
        .subname(3, "l3");
    dynamicRejectedLevel.init(4).flags(total);
    dynamicRejectedLevel
        .subname(0, "unused")
        .subname(1, "l1")
        .subname(2, "l2")
        .subname(3, "l3");
    cmcDynamicAdmittedLevel.init(4).flags(total);
    cmcDynamicAdmittedLevel
        .subname(0, "dropped")
        .subname(1, "l1")
        .subname(2, "l2")
        .subname(3, "l3");
    sourceEarlyDuplicateBySource.init(NUM_PF_SOURCES, 2);
    sourceEarlyDuplicateBySource
        .ysubname(0, "virtual")
        .ysubname(1, "physical");
    qualityTableReplacementsBySource.init(NUM_PF_SOURCES).flags(total);
    dynamicHotnessPreferredLevel.init(4).flags(total);
    dynamicHotnessPreferredLevel
        .subname(0, "none")
        .subname(1, "l1")
        .subname(2, "l2")
        .subname(3, "l3");
    qualityHotnessSamples.init(0, 64, 1);
    l1PollutionScoreSamples.init(0, 256, 1);
    queueOccupancy.init(0, queue_size, 1);
}

CentralizedDataPrefetcher::Translation::Translation(
    CentralizedDataPrefetcher *owner,
    const CentralizedPrefetchCandidate &candidate,
    const RequestPtr &request, ThreadContext *tc)
  : owner(owner), candidate(candidate), request(request), tc(tc),
    completed(false), failed(false), startTick(curTick()),
    completionTick(0)
{
}

void
CentralizedDataPrefetcher::Translation::finish(
    const Fault &fault, const RequestPtr &, ThreadContext *, BaseMMU::Mode)
{
    failed = fault != NoFault;
    completionTick = curTick();
    completed = true;
    owner->translationFinished();
}

CentralizedDataPrefetcher::CentralizedDataPrefetcher(
    const CentralizedDataPrefetcherParams &p)
  : XSCompositePrefetcher(p), l2Endpoint(p.l2_endpoint),
    l3Endpoint(p.l3_endpoint), centralQueueSize(p.central_queue_size),
    dispatchWidth(p.dispatch_width),
    l1DistanceThreshold(p.l1_distance_threshold),
    l2DistanceThreshold(p.l2_distance_threshold),
    l1MinMshrCredits(p.l1_min_mshr_credits),
    coreIdentifier(p.core_id), trainOnStore(p.train_on_store),
    enableDynamicDispatcher(p.dynamic_arbitration),
    qualityTableEntries(p.quality_table_entries),
    qualityTableAssoc(std::max(1u, p.quality_table_assoc)),
    qualityTableSets((p.quality_table_entries + qualityTableAssoc - 1) /
                     qualityTableAssoc),
    qualityInitialScore(p.quality_initial_score),
    qualityMaxScore(p.quality_max_score),
    qualityL1Threshold(p.quality_l1_threshold),
    qualityL2Threshold(p.quality_l2_threshold),
    qualityDropThreshold(p.quality_drop_threshold),
    qualityUsefulWeight(p.quality_useful_weight),
    qualityUnusedWeight(p.quality_unused_weight),
    qualityLateWeight(p.quality_late_weight),
    qualityDuplicateDemandWeight(p.quality_duplicate_demand_weight),
    qualityHotnessMax(p.quality_hotness_max),
    qualityHotnessObservationWeight(p.quality_hotness_observation_weight),
    qualityHotnessUsefulWeight(p.quality_hotness_useful_weight),
    qualityHotnessLateWeight(p.quality_hotness_late_weight),
    qualityHotnessDecayPeriod(p.quality_hotness_decay_period),
    qualityHotnessL1Threshold(p.quality_hotness_l1_threshold),
    qualityHotnessL2Threshold(p.quality_hotness_l2_threshold),
    cmcNearDistance(p.cmc_near_distance),
    cmcFarL1Threshold(p.cmc_far_l1_threshold),
    l1PollutionThreshold(p.l1_pollution_threshold),
    l1PollutionBypassThreshold(p.l1_pollution_bypass_threshold),
    l1PollutionUnusedWeight(p.l1_pollution_unused_weight),
    l1PollutionUsefulWeight(p.l1_pollution_useful_weight),
    l1PollutionDecay(p.l1_pollution_decay),
    duplicateFilterEntries(p.duplicate_filter_entries),
    managedPrefetchers(p.managed_prefetchers.begin(),
                       p.managed_prefetchers.end()),
    managedChildFilter(256),
    qualityTable(qualityTableSets * qualityTableAssoc),
    qualityAgeCounter(0), qualityHotnessCounter(0),
    l1PollutionScore(0), nextSequence(0),
    dispatchEvent([this] { dispatch(); }, name()),
    centralStats(this, p.central_queue_size)
{
    fatal_if(!usePFBuffer || !useVirtualAddresses,
             "%s requires use_pf_buffer=True and virtual-address mode",
             name());
    fatal_if(!l2Endpoint,
             "%s: centralized mode requires an L2 endpoint", name());
    fatal_if(!centralQueueSize || !dispatchWidth ||
             !l1DistanceThreshold ||
             l1DistanceThreshold >= l2DistanceThreshold,
             "%s: invalid central dispatcher parameters", name());
    fatal_if(l2Endpoint && l3Endpoint && l2Endpoint == l3Endpoint,
             "%s: L2 and L3 endpoints must be distinct", name());
    fatal_if(enableDynamicDispatcher &&
             (qualityTableEntries == 0 || qualityTableAssoc == 0 ||
              qualityTableSets == 0),
             "%s: dynamic quality table must have non-zero entries and assoc",
             name());
    fatal_if(qualityInitialScore < 0 || qualityInitialScore > qualityMaxScore,
             "%s: invalid dynamic quality initial/max score", name());
    fatal_if(qualityHotnessL1Threshold > qualityHotnessMax ||
             qualityHotnessL2Threshold > qualityHotnessMax ||
             qualityHotnessL2Threshold > qualityHotnessL1Threshold,
             "%s: invalid dynamic quality hotness thresholds", name());
    fatal_if(l1PollutionDecay < 0,
             "%s: l1_pollution_decay must be non-negative", name());
    if (l2Endpoint) {
        l2Endpoint->registerEngine(this);
    }
    if (l3Endpoint) {
        l3Endpoint->registerEngine(this);
    }
}

void
CentralizedDataPrefetcher::startup()
{
    ClockedObject::startup();
    fatal_if(!cache || blkSize != 64,
             "%s: centralized engine requires a parent and 64-byte lines",
             name());
}

void
CentralizedDataPrefetcher::bindManagedChildFilter(Base *prefetcher)
{
    if (prefetcher == nullptr) {
        return;
    }
    if (auto *bop = dynamic_cast<BOP *>(prefetcher)) {
        bop->filter = &managedChildFilter;
    }
    if (auto *stream = dynamic_cast<DespacitoStreamPrefetcher *>(prefetcher)) {
        stream->filter = &managedChildFilter;
    }
    if (auto *cmc = dynamic_cast<CMCPrefetcher *>(prefetcher)) {
        cmc->filter = &managedChildFilter;
    }
    if (auto *berti = dynamic_cast<BertiPrefetcher *>(prefetcher)) {
        berti->filter = &managedChildFilter;
    }
    if (auto *sstride = dynamic_cast<XSStridePrefetcher *>(prefetcher)) {
        sstride->filter = &managedChildFilter;
    }
    if (auto *opt = dynamic_cast<OptPrefetcher *>(prefetcher)) {
        opt->filter = &managedChildFilter;
    }
    if (auto *xsstream = dynamic_cast<XsStreamPrefetcher *>(prefetcher)) {
        xsstream->filter = &managedChildFilter;
    }
    if (auto *ipcp = dynamic_cast<IPCP *>(prefetcher)) {
        ipcp->rrf = &managedChildFilter;
    }
}


void
CentralizedDataPrefetcher::setParentInfo(System *sys, ProbeManager *pm,
    CacheAccessor *_cache, unsigned blk_size)
{
    XSCompositePrefetcher::setParentInfo(sys, pm, _cache, blk_size);
    for (auto *prefetcher : managedPrefetchers) {
        bindManagedChildFilter(prefetcher);
        prefetcher->setParentInfo(sys, pm, _cache, blk_size);
    }
}

void
CentralizedDataPrefetcher::addTLB(BaseTLB *_tlb, bool functional)
{
    XSCompositePrefetcher::addTLB(_tlb, functional);
    for (auto *prefetcher : managedPrefetchers) {
        prefetcher->addTLB(_tlb, functional);
    }
}

void
CentralizedDataPrefetcher::notifyIns(int ins_num)
{
    XSCompositePrefetcher::notifyIns(ins_num);
    for (auto *prefetcher : managedPrefetchers) {
        prefetcher->notifyIns(ins_num);
    }
}

void
CentralizedDataPrefetcher::prefetchUnused(
    Addr paddr, const Request::XsMetadata &metadata)
{
    Base::prefetchUnused(paddr, metadata);
    recordQualityFeedback(metadata, QualityUnused);
}

void
CentralizedDataPrefetcher::prefetchLate(
    const Request::XsMetadata &metadata, bool firstDemandMerge)
{
    Base::prefetchLate(metadata, firstDemandMerge);
    recordQualityFeedback(metadata, QualityLate);
}

void
CentralizedDataPrefetcher::recordPrefetchAdmitted(
    const Request::XsMetadata &metadata)
{
    Base::recordPrefetchAdmitted(metadata);
    recordQualityFeedback(metadata, QualityAdmitted);
}

void
CentralizedDataPrefetcher::recordPrefetchFill(
    const Request::XsMetadata &metadata)
{
    Base::recordPrefetchFill(metadata);
    recordQualityFeedback(metadata, QualityFilled);
}

void
CentralizedDataPrefetcher::recordUpperPrefetchConsumed(
    const Request::XsMetadata &metadata)
{
    Base::recordUpperPrefetchConsumed(metadata);
    recordQualityFeedback(metadata, QualityUpperConsumed);
}

void
CentralizedDataPrefetcher::recordPrefetchFillVictim(
    const Request::XsMetadata &metadata, bool dirty, bool demandTouched)
{
    Base::recordPrefetchFillVictim(metadata, dirty, demandTouched);
    if (!demandTouched) {
        recordQualityFeedback(metadata, QualityPollutionVictim);
    }
}

void
CentralizedDataPrefetcher::recordPrefetchUseful(
    const Request::XsMetadata &metadata, bool)
{
    recordQualityFeedback(metadata, QualityUseful);
}

void
CentralizedDataPrefetcher::notify(
    const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    if (pkt->cmd == MemCmd::HardPFReq ||
        (pfi.isStore() && !trainOnStore)) {
        return;
    }
    Queued::notify(pkt, pfi);
}

void
CentralizedDataPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses,
    bool late, PrefetchSourceType source, bool miss_repeat)
{
    std::vector<AddrPriority> generated;
    XSCompositePrefetcher::calculatePrefetch(
        pfi, generated, late, source, miss_repeat);
    captureCandidates(generated, &pfi.trigger_info);
    for (auto *prefetcher : managedPrefetchers) {
        auto *queued = dynamic_cast<Queued *>(prefetcher);
        fatal_if(!queued,
                 "%s: managed centralized child %s is not a Queued prefetcher",
                 name(), prefetcher->name());
        generated.clear();
        queued->calculatePrefetch(
            pfi, generated, late, source, miss_repeat);
        captureCandidates(generated, &pfi.trigger_info);
    }
    addresses.clear();
}

bool
CentralizedDataPrefetcher::GetPFRequestsFromBuffer(
    std::vector<AddrPriority> &addresses)
{
    std::vector<AddrPriority> generated;
    XSCompositePrefetcher::GetPFRequestsFromBuffer(generated);
    captureCandidates(generated, nullptr);
    addresses.clear();
    return false;
}

void
CentralizedDataPrefetcher::captureCandidates(
    std::vector<AddrPriority> &addresses,
    const Base::PFtriggerInfo *fallback)
{
    for (const auto &command : addresses) {
        const Base::PFtriggerInfo *trigger =
            command.pf_trigger_info.pkt &&
            command.pf_trigger_info.pfi_old ?
                &command.pf_trigger_info : fallback;
        if (!trigger || !trigger->pkt || !trigger->pfi_old) {
            centralStats.generated++;
            const unsigned src = sourceIndex(command.pfSource);
            if (src < NUM_PF_SOURCES) {
                centralStats.generatedBySource[src]++;
                centralStats.droppedBySourceAndReason[src]
                                                     [MissingTrigger]++;
            }
            centralStats.dropped++;
            centralStats.dropReason[MissingTrigger]++;
            continue;
        }
        enqueueCandidate(command, *trigger);
    }
    addresses.clear();
}

CentralizedCandidateKey
CentralizedDataPrefetcher::keyFromPfi(
    const PrefetchInfo &pfi, Addr addr) const
{
    return {blockAddress(addr),
            pfi.hasContextId() ? pfi.contextId() : InvalidContextID,
            pfi.hasContextId(), pfi.isSecure()};
}

CentralizedCandidateKey
CentralizedDataPrefetcher::keyFromPacket(const PacketPtr &pkt) const
{
    const bool valid_context = pkt->req->hasContextId();
    const Addr addr = pkt->req->hasVaddr() ?
        pkt->req->getVaddr() : pkt->getAddr();
    return {blockAddress(addr),
            valid_context ? pkt->req->contextId() : InvalidContextID,
            valid_context, pkt->isSecure()};
}

CentralizedPhysicalKey
CentralizedDataPrefetcher::physicalKey(Addr paddr, bool secure) const
{
    return {blockAddress(paddr), secure};
}

void
CentralizedDataPrefetcher::enqueueCandidate(
    const AddrPriority &command, const Base::PFtriggerInfo &trigger)
{
    centralStats.generated++;
    const unsigned src = sourceIndex(command.pfSource);
    if (src < NUM_PF_SOURCES) {
        centralStats.generatedBySource[src]++;
    }
    PrefetchInfo pfi(*trigger.pfi_old);
    const Addr target = blockAddress(command.addr);
    const Addr origin = blockAddress(pfi.getAddr());
    const uint64_t byte_distance = target >= origin ?
        target - origin : origin - target;
    const uint32_t distance = std::min<uint64_t>(
        byte_distance / blkSize,
        std::numeric_limits<uint32_t>::max());
    const uint8_t preferred = preferredLevel(distance);
    const uint8_t native_level =
        command.pfahead && command.pfahead_host >= 1 &&
        command.pfahead_host <= 3 ? command.pfahead_host : 0;
    const bool cross_page = !samePage(origin, target);
    centralStats.preferredLevel[preferred]++;
    centralStats.distanceBucket[distanceBucket(distance)]++;
    if (src < NUM_PF_SOURCES) {
        centralStats.generatedBySourceAndPreferredLevel[src][preferred]++;
        centralStats.nativeLevelBySource[src][native_level]++;
        if (native_level && native_level != preferred) {
            centralStats.nativeLevelMismatchBySource[src]++;
        }
    }
    centralStats.preferredToNativeLevel[preferred][native_level]++;
    const auto key = keyFromPfi(pfi, target);
    CentralizedPrefetchCandidate candidate(
        target, command.priority, command.depth, command.pfSource,
        distance, preferred, nextSequence++, cross_page, curTick(),
        key, trigger);
    candidate.centralizedCoreId = static_cast<uint16_t>(coreIdentifier);
    if (command.pfSource == PrefetchSourceType::CMC) {
        centralStats.cmcGenerated++;
    }
    if (cross_page && src < NUM_PF_SOURCES) {
        centralStats.crossPageGenerated[src][preferred]++;
    }
    if (sourceRecentFilterHit(candidate, false)) {
        dropCandidate(candidate, Duplicate, false);
        return;
    }
    if (pending.find(key) != pending.end()) {
        dropCandidate(candidate, Duplicate, false);
        return;
    }
    if (centralOccupancy() >= centralQueueSize) {
        if (candidates.empty()) {
            dropCandidate(candidate, CentralQueueFull, false);
            return;
        }
        auto worst = std::prev(candidates.end());
        if (!CentralizedCandidatePriority{}(candidate, *worst)) {
            dropCandidate(candidate, CentralQueueFull, false);
            return;
        }
        dropCandidate(*worst, CentralQueueFull, true);
        candidates.erase(worst);
    }
    pending.insert(key);
    candidates.insert(candidate);
    centralStats.queueOccupancy.sample(centralOccupancy());
    if (!dispatchEvent.scheduled()) {
        schedule(dispatchEvent, nextCycle());
    }
}

uint8_t
CentralizedDataPrefetcher::preferredLevel(uint32_t distance) const
{
    if (distance <= l1DistanceThreshold) {
        return 1;
    }
    if (distance <= l2DistanceThreshold) {
        return 2;
    }
    return 3;
}

size_t
CentralizedDataPrefetcher::centralOccupancy() const
{
    return candidates.size() + translations.size();
}

size_t
CentralizedDataPrefetcher::localQueueOccupancy() const
{
    return pfq.size() + pfqMissingTranslation.size();
}

void
CentralizedDataPrefetcher::translationFinished()
{
    if (!dispatchEvent.scheduled()) {
        schedule(dispatchEvent, nextCycle());
    }
}

void
CentralizedDataPrefetcher::processPhysicalReleases()
{
    while (!physicalReleases.empty() &&
           physicalReleases.front().first <= curTick()) {
        pendingPhysical.erase(physicalReleases.front().second);
        physicalReleases.pop_front();
    }
}

void
CentralizedDataPrefetcher::deferPhysicalRelease(
    const CentralizedPhysicalKey &key)
{
    physicalReleases.emplace_back(nextCycle(), key);
    if (!dispatchEvent.scheduled()) {
        schedule(dispatchEvent, physicalReleases.front().first);
    }
}

void
CentralizedDataPrefetcher::processTranslations()
{
    auto it = translations.begin();
    while (it != translations.end()) {
        if (!(*it)->completed) {
            ++it;
            continue;
        }
        CentralizedPrefetchCandidate candidate = (*it)->candidate;
        const RequestPtr request = (*it)->request;
        const bool failed = (*it)->failed;
        const Tick start_tick = (*it)->startTick;
        const Tick completion_tick = (*it)->completionTick;
        it = translations.erase(it);
        const unsigned src = sourceIndex(candidate.source);
        centralStats.translationQueueWait.sample(
            start_tick - candidate.enqueueTick);
        centralStats.translationLatency.sample(
            completion_tick - start_tick);
        if (src < NUM_PF_SOURCES) {
            centralStats.translationQueueWaitTicksBySource[src] +=
                start_tick - candidate.enqueueTick;
            centralStats.translationLatencyTicksBySource[src] +=
                completion_tick - start_tick;
            if (failed) {
                centralStats.translationFailureBySource[src]++;
            } else {
                centralStats.translationSuccessBySource[src]++;
            }
        }
        if (failed) {
            dropCandidate(candidate, TranslationFailed, true);
            continue;
        }
        candidate.paddr = blockAddress(request->getPaddr());
        candidate.paddrValid = true;
        if (!system->isMemAddr(candidate.paddr)) {
            dropCandidate(candidate, TranslationFailed, true);
            continue;
        }
        candidate.physicalKey = physicalKey(candidate.paddr, candidate.key.secure);
        if (sourceRecentFilterHit(candidate, true)) {
            dropCandidate(candidate, PhysicalDuplicate, true);
            continue;
        }
        if (!registerPhysical(candidate)) {
            recordPhysicalDuplicate(candidate, PendingPhysicalDuplicate);
            dropCandidate(candidate, PhysicalDuplicate, true);
            continue;
        }
        candidates.insert(candidate);
    }
}

void
CentralizedDataPrefetcher::dispatch()
{
    processPhysicalReleases();
    processTranslations();
    for (unsigned count = 0;
         count < dispatchWidth && !candidates.empty(); ++count) {
        auto it = candidates.begin();
        CentralizedPrefetchCandidate candidate = *it;
        candidates.erase(it);
        if (!candidate.paddrValid && !resolvePaddr(candidate)) {
            continue;
        }
        const bool dispatched = enableDynamicDispatcher ?
            dispatchCandidateDynamic(candidate) : dispatchCandidate(candidate);
        if (!dispatched) {
            dropCandidate(candidate, NoAdmission, true);
        }
    }
    centralStats.queueOccupancy.sample(centralOccupancy());

    Tick next_event = MaxTick;
    if (!candidates.empty()) {
        next_event = nextCycle();
    }
    if (!physicalReleases.empty()) {
        next_event = std::min(next_event, physicalReleases.front().first);
    }
    if (next_event != MaxTick && !dispatchEvent.scheduled()) {
        schedule(dispatchEvent, next_event);
    }
}

bool
CentralizedDataPrefetcher::resolvePaddr(
    CentralizedPrefetchCandidate &candidate)
{
    if (!candidate.trigger.pkt || !candidate.trigger.pfi_old) {
        dropCandidate(candidate, TranslationUnavailable, true);
        return false;
    }
    PrefetchInfo pfi(*candidate.trigger.pfi_old);
    const RequestPtr &trigger_req = candidate.trigger.pkt->req;
    if (!trigger_req->hasVaddr() || !trigger_req->hasPaddr()) {
        dropCandidate(candidate, TranslationUnavailable, true);
        return false;
    }
    const Addr origin_vaddr = blockAddress(pfi.getAddr());
    if (samePage(origin_vaddr, candidate.vaddr)) {
        const Addr delta = candidate.vaddr >= origin_vaddr ?
            candidate.vaddr - origin_vaddr :
            origin_vaddr - candidate.vaddr;
        candidate.paddr = candidate.vaddr >= origin_vaddr ?
            trigger_req->getPaddr() + delta :
            trigger_req->getPaddr() - delta;
        candidate.paddr = blockAddress(candidate.paddr);
        candidate.paddrValid = true;
        if (!system->isMemAddr(candidate.paddr)) {
            dropCandidate(candidate, TranslationFailed, true);
            return false;
        }
        candidate.physicalKey = physicalKey(candidate.paddr, candidate.key.secure);
        if (sourceRecentFilterHit(candidate, true)) {
            dropCandidate(candidate, PhysicalDuplicate, true);
            return false;
        }
        if (!registerPhysical(candidate)) {
            recordPhysicalDuplicate(candidate, PendingPhysicalDuplicate);
            dropCandidate(candidate, PhysicalDuplicate, true);
            return false;
        }
        return true;
    }
    if (!tlb || !pfi.hasContextId() || !startTranslation(candidate)) {
        dropCandidate(candidate, TranslationUnavailable, true);
    }
    return false;
}

bool
CentralizedDataPrefetcher::startTranslation(
    const CentralizedPrefetchCandidate &candidate)
{
    PrefetchInfo pfi(*candidate.trigger.pfi_old);
    const ContextID context = pfi.contextId();
    if (static_cast<size_t>(context) >= system->threads.size()) {
        return false;
    }
    RequestPtr request = createPrefetchRequest(
        candidate.vaddr, pfi, candidate.trigger.pkt, candidate.source,
        candidate.depth, candidate.distance, candidate.preferredLevel, 0);
    request->setFlags(Request::PREFETCH);
    auto metadata = request->getXsMetadata();
    metadata.crossPagePrefetch = candidate.crossPage;
    metadata.centralizedPrefetch = true;
    metadata.centralizedCoreId = candidate.centralizedCoreId;
    metadata.centralizedTriggerPcHash = candidate.triggerPcHash;
    request->setXsMetadata(metadata);
    ThreadContext *tc = system->threads[context];
    translations.emplace_back(std::make_unique<Translation>(
        this, candidate, request, tc));
    Translation *translation = translations.back().get();
    centralStats.translationRequests++;
    const unsigned src = sourceIndex(candidate.source);
    if (src < NUM_PF_SOURCES) {
        centralStats.translationRequestsBySource[src]++;
    }
    if (functionalTLB) {
        tlb->translateFunctional(
            request, tc, translation, BaseMMU::Read);
    } else {
        tlb->translateTiming(request, tc, translation, BaseMMU::Read);
    }
    return true;
}

bool
CentralizedDataPrefetcher::registerPhysical(
    CentralizedPrefetchCandidate &candidate)
{
    candidate.physicalKey =
        physicalKey(candidate.paddr, candidate.key.secure);
    const bool inserted =
        pendingPhysical.insert(candidate.physicalKey).second;
    candidate.physicalRegistered = inserted;
    return inserted;
}

Request::XsMetadata
CentralizedDataPrefetcher::metadataForCandidate(
    const CentralizedPrefetchCandidate &candidate) const
{
    return Request::XsMetadata(
        candidate.source, candidate.depth, candidate.distance,
        candidate.preferredLevel, candidate.actualLevel,
        candidate.crossPage, true,
        candidate.centralizedCoreId, candidate.triggerPcHash);
}

CentralizedDataPrefetcher::QualityKey
CentralizedDataPrefetcher::qualityKeyFromCandidate(
    const CentralizedPrefetchCandidate &candidate) const
{
    QualityKey key;
    key.source = static_cast<uint8_t>(sourceIndex(candidate.source));
    key.distanceBucket = static_cast<uint8_t>(distanceBucket(candidate.distance));
    key.preferredLevel = candidate.preferredLevel;
    key.triggerPcHash = candidate.triggerPcHash;
    return key;
}

CentralizedDataPrefetcher::QualityKey
CentralizedDataPrefetcher::qualityKeyFromMetadata(
    const Request::XsMetadata &metadata) const
{
    QualityKey key;
    key.source = static_cast<uint8_t>(sourceIndex(metadata.prefetchSource));
    key.distanceBucket = static_cast<uint8_t>(
        distanceBucket(metadata.prefetchDistance));
    key.preferredLevel = metadata.preferredPrefetchLevel;
    key.triggerPcHash = metadata.centralizedTriggerPcHash;
    return key;
}

size_t
CentralizedDataPrefetcher::qualitySetIndex(const QualityKey &key) const
{
    size_t hash = key.triggerPcHash;
    hash ^= static_cast<size_t>(key.source) * 0x9e37;
    hash ^= static_cast<size_t>(key.distanceBucket) << 8;
    hash ^= static_cast<size_t>(key.preferredLevel) << 13;
    return qualityTableSets ? hash % qualityTableSets : 0;
}

CentralizedDataPrefetcher::QualityEntry &
CentralizedDataPrefetcher::qualityEntryForUpdate(const QualityKey &key)
{
    const size_t set = qualitySetIndex(key);
    const size_t base = set * qualityTableAssoc;
    QualityEntry *victim = nullptr;

    for (unsigned way = 0; way < qualityTableAssoc; ++way) {
        QualityEntry &entry = qualityTable[base + way];
        if (entry.valid && entry.key == key) {
            entry.age = ++qualityAgeCounter;
            return entry;
        }
        if (!entry.valid && !victim) {
            victim = &entry;
        }
    }

    if (!victim) {
        victim = &qualityTable[base];
        for (unsigned way = 1; way < qualityTableAssoc; ++way) {
            QualityEntry &entry = qualityTable[base + way];
            if (entry.score < victim->score ||
                (entry.score == victim->score && entry.age < victim->age)) {
                victim = &entry;
            }
        }
        centralStats.qualityTableReplacements++;
        if (key.source < NUM_PF_SOURCES) {
            centralStats.qualityTableReplacementsBySource[key.source]++;
        }
    }

    *victim = QualityEntry();
    victim->valid = true;
    victim->key = key;
    victim->score = static_cast<int16_t>(qualityInitialScore);
    victim->hotnessEpoch = qualityHotnessDecayPeriod ?
        qualityHotnessCounter / qualityHotnessDecayPeriod : 0;
    victim->age = ++qualityAgeCounter;
    return *victim;
}

const CentralizedDataPrefetcher::QualityEntry *
CentralizedDataPrefetcher::findQualityEntry(const QualityKey &key)
{
    if (!qualityTableSets || !qualityTableAssoc) {
        return nullptr;
    }

    centralStats.qualityTableLookups++;
    const size_t set = qualitySetIndex(key);
    const size_t base = set * qualityTableAssoc;
    for (unsigned way = 0; way < qualityTableAssoc; ++way) {
        QualityEntry &entry = qualityTable[base + way];
        if (entry.valid && entry.key == key) {
            centralStats.qualityTableHits++;
            decayHotness(entry);
            entry.age = ++qualityAgeCounter;
            return &entry;
        }
    }
    centralStats.qualityTableMisses++;
    return nullptr;
}

void
CentralizedDataPrefetcher::decayHotness(QualityEntry &entry)
{
    if (!qualityHotnessDecayPeriod || !entry.valid) {
        return;
    }
    const uint32_t epoch = qualityHotnessCounter / qualityHotnessDecayPeriod;
    if (entry.hotnessEpoch >= epoch) {
        return;
    }
    const uint32_t elapsed = epoch - entry.hotnessEpoch;
    const uint16_t old = entry.hotness;
    entry.hotness = static_cast<uint16_t>(
        elapsed >= entry.hotness ? 0 : entry.hotness - elapsed);
    entry.hotnessEpoch = epoch;
    if (old != entry.hotness) {
        centralStats.qualityHotnessDecays += old - entry.hotness;
    }
}

void
CentralizedDataPrefetcher::bumpHotness(QualityEntry &entry, unsigned amount)
{
    decayHotness(entry);
    if (!amount || !qualityHotnessMax) {
        centralStats.qualityHotnessSamples.sample(entry.hotness);
        return;
    }
    entry.hotness = static_cast<uint16_t>(std::min<unsigned>(
        qualityHotnessMax, static_cast<unsigned>(entry.hotness) + amount));
    centralStats.qualityHotnessObservations++;
    centralStats.qualityHotnessSamples.sample(entry.hotness);
}

void
CentralizedDataPrefetcher::observeQualityHotness(
    const QualityKey &key, unsigned amount)
{
    if (!enableDynamicDispatcher || key.source >= NUM_PF_SOURCES) {
        return;
    }
    ++qualityHotnessCounter;
    QualityEntry &entry = qualityEntryForUpdate(key);
    bumpHotness(entry, amount);
}

unsigned
CentralizedDataPrefetcher::qualityHotness(const QualityEntry *entry) const
{
    return entry ? entry->hotness : 0;
}

unsigned
CentralizedDataPrefetcher::hotnessPreferredLevel(
    const CentralizedPrefetchCandidate &candidate, const QualityEntry *entry,
    unsigned deepest) const
{
    const unsigned hotness = qualityHotness(entry);
    if (qualityHotnessL1Threshold && hotness >= qualityHotnessL1Threshold) {
        return 1;
    }
    if (qualityHotnessL2Threshold && hotness >= qualityHotnessL2Threshold) {
        return std::min<unsigned>(2, deepest);
    }
    return std::min<unsigned>(candidate.preferredLevel, deepest);
}

void
CentralizedDataPrefetcher::updateQuality(
    const QualityKey &key, QualityFeedbackEvent event, uint8_t level)
{
    if (!enableDynamicDispatcher || key.source >= NUM_PF_SOURCES) {
        return;
    }

    QualityEntry &entry = qualityEntryForUpdate(key);
    int delta = 0;
    switch (event) {
      case QualityAdmitted:
      case QualityFilled:
        entry.admitted = saturatingInc(entry.admitted, qualityMaxScore);
        break;
      case QualityUseful:
      case QualityUpperConsumed:
        entry.useful = saturatingInc(entry.useful, qualityMaxScore);
        bumpHotness(entry, qualityHotnessUsefulWeight);
        delta = qualityUsefulWeight;
        if (level == 1) {
            l1PollutionScore = std::max(
                0, l1PollutionScore + l1PollutionUsefulWeight);
        }
        break;
      case QualityLate:
        entry.late = saturatingInc(entry.late, qualityMaxScore);
        bumpHotness(entry, qualityHotnessLateWeight);
        delta = qualityLateWeight;
        break;
      case QualityUnused:
        entry.unused = saturatingInc(entry.unused, qualityMaxScore);
        delta = qualityUnusedWeight;
        if (level == 1) {
            l1PollutionScore = std::min(
                l1PollutionThreshold * 4 + qualityMaxScore,
                l1PollutionScore + l1PollutionUnusedWeight);
        }
        break;
      case QualityPollutionVictim:
        entry.pollution = saturatingInc(entry.pollution, qualityMaxScore);
        delta = qualityUnusedWeight;
        if (level == 1) {
            l1PollutionScore = std::min(
                l1PollutionThreshold * 4 + qualityMaxScore,
                l1PollutionScore + l1PollutionUnusedWeight);
        }
        break;
      case QualityDuplicateDemand:
        entry.duplicateDemand = saturatingInc(
            entry.duplicateDemand, qualityMaxScore);
        delta = qualityDuplicateDemandWeight;
        break;
      case NumQualityFeedbackEvents:
        break;
    }

    entry.score = static_cast<int16_t>(
        clampScore(static_cast<int>(entry.score) + delta, qualityMaxScore));
    if (event < NumQualityFeedbackEvents) {
        centralStats.qualityFeedbackByEvent[event]++;
    }
    centralStats.l1PollutionScoreSamples.sample(l1PollutionScore);
}

void
CentralizedDataPrefetcher::recordQualityFeedback(
    const Request::XsMetadata &metadata, QualityFeedbackEvent event)
{
    if (!metadata.centralizedPrefetch ||
        metadata.centralizedCoreId != coreIdentifier) {
        return;
    }
    updateQuality(qualityKeyFromMetadata(metadata), event,
                  metadata.issuedPrefetchLevel);
}

bool
CentralizedDataPrefetcher::canInjectAtLevel(
    const CentralizedPrefetchCandidate &candidate, unsigned level)
{
    if (level == 1) {
        return cache && localQueueOccupancy() < queueSize &&
               cache->prefetchMshrCredits(candidate.paddr) >=
                   l1MinMshrCredits;
    }
    CentralizedPrefetcherEndpoint *endpoint =
        level == 2 ? l2Endpoint : level == 3 ? l3Endpoint : nullptr;
    return endpoint && endpoint->canAccept(
        this, candidate.paddr, candidate.source, false);
}

int
CentralizedDataPrefetcher::qualityScore(
    const CentralizedPrefetchCandidate &candidate)
{
    const QualityEntry *entry = findQualityEntry(
        qualityKeyFromCandidate(candidate));
    return entry ? entry->score : qualityInitialScore;
}

int
CentralizedDataPrefetcher::levelAdmissionThreshold(unsigned level) const
{
    if (level == 1) {
        return qualityL1Threshold;
    }
    if (level == 2) {
        return qualityL2Threshold;
    }
    return qualityDropThreshold;
}

bool
CentralizedDataPrefetcher::dynamicL1Blocked(int score) const
{
    if (l1PollutionThreshold <= 0 ||
        l1PollutionScore < l1PollutionThreshold) {
        return false;
    }
    return score < l1PollutionBypassThreshold;
}

bool
CentralizedDataPrefetcher::dynamicL1Eligible(
    const CentralizedPrefetchCandidate &candidate, int score, bool cmc_near,
    bool hot_l1)
{
    if (score < qualityL1Threshold) {
        centralStats.dynamicL1QualityRejects++;
        return false;
    }
    if (candidate.distance > l1DistanceThreshold && !cmc_near && !hot_l1) {
        centralStats.dynamicL1DistanceRejects++;
        return false;
    }
    if (dynamicL1Blocked(score)) {
        centralStats.dynamicL1PollutionRejects++;
        return false;
    }
    return true;
}

bool
CentralizedDataPrefetcher::cmcNearCandidate(
    const CentralizedPrefetchCandidate &candidate,
    const QualityEntry *entry) const
{
    if (candidate.source != PrefetchSourceType::CMC) {
        return false;
    }
    if (candidate.distance <= cmcNearDistance) {
        return true;
    }
    const int score = entry ? entry->score : qualityInitialScore;
    return score >= cmcFarL1Threshold;
}

CentralizedSourceRecentKey
CentralizedDataPrefetcher::sourceRecentKey(
    const CentralizedPrefetchCandidate &candidate, bool physical) const
{
    return {physical ? candidate.physicalKey.block : candidate.key.block,
            physical ? InvalidContextID : candidate.key.context,
            physical ? false : candidate.key.contextValid,
            candidate.key.secure,
            static_cast<uint8_t>(sourceIndex(candidate.source)),
            static_cast<uint8_t>(distanceBucket(candidate.distance)),
            candidate.triggerPcHash};
}

void
CentralizedDataPrefetcher::insertSourceRecent(
    const CentralizedSourceRecentKey &key, bool physical)
{
    if (!duplicateFilterEntries) {
        return;
    }

    auto &set = physical ? recentPhysical : recentVirtual;
    auto &order = physical ? recentPhysicalOrder : recentVirtualOrder;
    if (!set.insert(key).second) {
        return;
    }
    order.push_back(key);
    while (order.size() > duplicateFilterEntries) {
        set.erase(order.front());
        order.pop_front();
    }
}

bool
CentralizedDataPrefetcher::sourceRecentFilterHit(
    const CentralizedPrefetchCandidate &candidate, bool physical)
{
    if (!enableDynamicDispatcher || !duplicateFilterEntries) {
        return false;
    }
    const CentralizedSourceRecentKey key = sourceRecentKey(candidate, physical);
    auto &set = physical ? recentPhysical : recentVirtual;
    if (set.find(key) == set.end()) {
        insertSourceRecent(key, physical);
        return false;
    }

    const unsigned src = sourceIndex(candidate.source);
    centralStats.sourceEarlyDuplicateDrop++;
    if (src < NUM_PF_SOURCES) {
        centralStats.sourceEarlyDuplicateBySource[src][physical ? 1 : 0]++;
    }
    if (candidate.source == PrefetchSourceType::CMC) {
        centralStats.cmcEarlySuppressed++;
    }
    updateQuality(qualityKeyFromCandidate(candidate),
                  QualityDuplicateDemand, 0);
    return true;
}

bool
CentralizedDataPrefetcher::dispatchCandidateDynamic(
    CentralizedPrefetchCandidate &candidate)
{
    if (l1PollutionDecay > 0 && l1PollutionScore > 0) {
        l1PollutionScore = std::max(0, l1PollutionScore - l1PollutionDecay);
        centralStats.l1PollutionScoreSamples.sample(l1PollutionScore);
    }

    const auto cover_reason = physicalDuplicateCoverReason(candidate);
    if (cover_reason != NumPhysicalDuplicateCoverReasons) {
        if (handleCoveredPromotion(candidate, cover_reason)) {
            return true;
        }
        recordPhysicalDuplicate(candidate, cover_reason);
        dropCandidate(candidate, PhysicalDuplicate, true);
        return true;
    }

    const unsigned deepest = l3Endpoint ? 3 : (l2Endpoint ? 2 : 1);
    if (deepest == 0) {
        return false;
    }

    const QualityKey qkey = qualityKeyFromCandidate(candidate);
    observeQualityHotness(qkey, qualityHotnessObservationWeight);
    const QualityEntry *entry = findQualityEntry(qkey);
    const int score = entry ? entry->score : qualityInitialScore;
    if (score < qualityDropThreshold) {
        centralStats.dynamicSelectedLevel[0]++;
        return false;
    }

    std::array<unsigned, 3> levels = {{0, 0, 0}};
    unsigned count = 0;
    auto append = [&](unsigned level) {
        if (level >= 1 && level <= deepest && count < levels.size()) {
            levels[count++] = level;
        }
    };

    const unsigned hot_first = hotnessPreferredLevel(candidate, entry, deepest);
    if (hot_first != std::min<unsigned>(candidate.preferredLevel, deepest)) {
        centralStats.dynamicHotnessPreferredLevel[hot_first]++;
    } else {
        centralStats.dynamicHotnessPreferredLevel[0]++;
    }

    if (candidate.source == PrefetchSourceType::CMC) {
        if (cmcNearCandidate(candidate, entry) || hot_first == 1) {
            centralStats.dynamicCmcNearL1++;
            append(1);
            append(2);
            append(3);
        } else {
            const unsigned first = hot_first;
            if (first > 1) {
                centralStats.dynamicCmcFarDeep++;
            }
            for (unsigned level = first; level <= deepest; ++level) {
                append(level);
            }
        }
    } else {
        for (unsigned level = hot_first; level <= deepest; ++level) {
            append(level);
        }
    }

    for (unsigned level : levels) {
        if (level == 0) {
            continue;
        }
        const bool cmc_near = candidate.source == PrefetchSourceType::CMC &&
            cmcNearCandidate(candidate, entry);
        const bool hot_l1 = hot_first == 1;
        if (level == 1 && !dynamicL1Eligible(
                candidate, score, cmc_near, hot_l1)) {
            centralStats.dynamicRejectedLevel[level]++;
            continue;
        }
        if (level != 1 && score < levelAdmissionThreshold(level)) {
            centralStats.dynamicRejectedLevel[level]++;
            continue;
        }
        if (!canInjectAtLevel(candidate, level)) {
            centralStats.dynamicRejectedLevel[level]++;
            continue;
        }
        centralStats.dynamicSelectedLevel[level]++;
        if (candidate.source == PrefetchSourceType::CMC) {
            centralStats.cmcDynamicAdmittedLevel[level]++;
        }
        return injectAtLevel(candidate, level);
    }

    centralStats.dynamicSelectedLevel[0]++;
    if (candidate.source == PrefetchSourceType::CMC) {
        centralStats.cmcDynamicAdmittedLevel[0]++;
    }
    return false;
}

bool
CentralizedDataPrefetcher::dispatchCandidate(
    CentralizedPrefetchCandidate &candidate, unsigned first_level)
{
    const auto cover_reason = physicalDuplicateCoverReason(candidate);
    if (cover_reason != NumPhysicalDuplicateCoverReasons) {
        if (handleCoveredPromotion(candidate, cover_reason)) {
            return true;
        }
        recordPhysicalDuplicate(candidate, cover_reason);
        dropCandidate(candidate, PhysicalDuplicate, true);
        return true;
    }

    const unsigned deepest = l3Endpoint ? 3 : (l2Endpoint ? 2 : 1);
    const unsigned first = first_level ? first_level :
        candidate.source == PrefetchSourceType::CMC ? 1 :
        std::min<unsigned>(candidate.preferredLevel, deepest);
    if (first > deepest) {
        return false;
    }

    for (unsigned target = first; target <= deepest; ++target) {
        if (target == 1) {
            if (!cache || localQueueOccupancy() >= queueSize ||
                cache->prefetchMshrCredits(candidate.paddr) <
                    l1MinMshrCredits) {
                if (candidate.source == PrefetchSourceType::CMC) {
                    continue;
                }
                return false;
            }
            candidate.actualLevel = 1;
            if (injectLocal(candidate)) {
                const bool inserted = localCompletions.emplace(
                    candidate.physicalKey,
                    CentralizedCompletion(this, candidate)).second;
                fatal_if(!inserted,
                         "%s: duplicate local completion for %#x",
                         name(), candidate.physicalKey.block);
                recordDispatch(candidate);
                return true;
            }
            dropCandidate(candidate, EndpointRejected, true);
            return true;
        }

        CentralizedPrefetcherEndpoint *endpoint =
            target == 2 ? l2Endpoint : l3Endpoint;
        if (!endpoint || !endpoint->canAccept(
                this, candidate.paddr, candidate.source)) {
            if (candidate.source == PrefetchSourceType::CMC) {
                continue;
            }
            return false;
        }
        candidate.actualLevel = target;
        if (endpoint->enqueue(candidate, this)) {
            recordDispatch(candidate);
            return true;
        }
        dropCandidate(candidate, EndpointRejected, true);
        return true;
    }
    return false;
}

bool
CentralizedDataPrefetcher::canPromoteToL1(
    const CentralizedPrefetchCandidate &candidate) const
{
    return cache && localQueueOccupancy() < queueSize &&
           cache->prefetchMshrCredits(candidate.paddr) >=
               l1MinMshrCredits;
}

bool
CentralizedDataPrefetcher::canPromoteToL2(
    const CentralizedPrefetchCandidate &candidate)
{
    return l2Endpoint &&
           l2Endpoint->canAccept(
               this, candidate.paddr, candidate.source, false);
}

bool
CentralizedDataPrefetcher::injectAtLevel(
    CentralizedPrefetchCandidate &candidate, unsigned level)
{
    if (level == 1) {
        candidate.actualLevel = 1;
        if (injectLocal(candidate)) {
            const bool inserted = localCompletions.emplace(
                candidate.physicalKey,
                CentralizedCompletion(this, candidate)).second;
            fatal_if(!inserted,
                     "%s: duplicate local completion for %#x",
                     name(), candidate.physicalKey.block);
            recordDispatch(candidate);
            return true;
        }
        dropCandidate(candidate, EndpointRejected, true);
        return true;
    }

    CentralizedPrefetcherEndpoint *endpoint =
        level == 2 ? l2Endpoint : level == 3 ? l3Endpoint : nullptr;
    if (!endpoint) {
        return false;
    }
    candidate.actualLevel = level;
    if (endpoint->enqueue(candidate, this)) {
        recordDispatch(candidate);
        return true;
    }
    dropCandidate(candidate, EndpointRejected, true);
    return true;
}

bool
CentralizedDataPrefetcher::handleCoveredPromotion(
    CentralizedPrefetchCandidate &candidate,
    PhysicalDuplicateCoverReason cover_reason)
{
    const int score = enableDynamicDispatcher ?
        qualityScore(candidate) : qualityInitialScore;
    const QualityEntry *entry = enableDynamicDispatcher ?
        findQualityEntry(qualityKeyFromCandidate(candidate)) : nullptr;
    const bool cmc_near = enableDynamicDispatcher &&
                          cmcNearCandidate(candidate, entry);
    const bool l1_allowed = !enableDynamicDispatcher ||
        (score >= qualityL1Threshold &&
         (candidate.distance <= l1DistanceThreshold || cmc_near) &&
         !dynamicL1Blocked(score));
    const bool l2_allowed = !enableDynamicDispatcher ||
        score >= qualityL2Threshold;

    switch (cover_reason) {
      case L2CacheDemand:
      case L2CachePrefetchSameSource:
      case L2CachePrefetchOtherSource:
        if (enableDynamicDispatcher) {
            if (!l1_allowed ||
                (candidate.preferredLevel != 1 && !cmc_near)) {
                return false;
            }
        } else if (candidate.preferredLevel != 1 &&
                   !(candidate.source == PrefetchSourceType::CMC)) {
            return false;
        }
        if (!canPromoteToL1(candidate)) {
            recordCoveredPromotion(candidate, L2HitL1MshrFull);
            recordPhysicalDuplicate(candidate, cover_reason);
            dropCandidate(candidate, PhysicalDuplicate, true);
            return true;
        }
        recordCoveredPromotion(candidate, L2HitPromoteToL1);
        return injectAtLevel(candidate, 1);

      case L3CacheDemand:
      case L3CachePrefetchSameSource:
      case L3CachePrefetchOtherSource:
        if ((candidate.preferredLevel == 1 || cmc_near) && l1_allowed) {
            if (canPromoteToL1(candidate)) {
                recordCoveredPromotion(candidate, L3HitPromoteToL1);
                return injectAtLevel(candidate, 1);
            }
        }
        if ((candidate.preferredLevel <= 2 || cmc_near) && l2_allowed) {
            if (canPromoteToL2(candidate)) {
                recordCoveredPromotion(candidate, L3HitPromoteToL2);
                return injectAtLevel(candidate, 2);
            }
        }
        if (enableDynamicDispatcher &&
            (score < qualityL2Threshold || candidate.preferredLevel > 2)) {
            return false;
        }
        recordCoveredPromotion(candidate, L3HitL1L2MshrFull);
        recordPhysicalDuplicate(candidate, cover_reason);
        dropCandidate(candidate, PhysicalDuplicate, true);
        return true;

      default:
        return false;
    }
}

bool
CentralizedDataPrefetcher::injectLocal(
    CentralizedPrefetchCandidate &candidate)
{
    if (!candidate.trigger.pfi_old ||
        !system->isMemAddr(candidate.paddr)) {
        return false;
    }
    if (cacheSnoop && queueFilter &&
        (inCache(candidate.paddr, candidate.key.secure) ||
         inMissQueue(candidate.paddr, candidate.key.secure) ||
         cache->inWriteQueue(candidate.paddr, candidate.key.secure))) {
        statsQueued.pfInCache++;
        return false;
    }
    PrefetchInfo pfi(*candidate.trigger.pfi_old);
    PrefetchInfo new_pfi(pfi, candidate.vaddr);
    new_pfi.setXsMetadata(metadataForCandidate(candidate));
    DeferredPacket dpp(this, new_pfi, 0, candidate.priority);
    dpp.pfahead = false;
    dpp.pfahead_host = 0;
    const Tick ready = curTick() + clockPeriod() * latency;
    dpp.createPkt(candidate.paddr, blkSize, requestorId, tagPrefetch,
                  ready, candidate.source, candidate.depth,
                  candidate.distance, candidate.preferredLevel,
                  candidate.actualLevel, candidate.crossPage);
    dpp.pkt->req->setXsMetadata(new_pfi.getXsMetadata());
    const size_t before = pfq.size();
    statsQueued.pfIdentified++;
    addToQueue(pfq, dpp);
    return pfq.size() > before;
}

void
CentralizedDataPrefetcher::addToQueue(
    std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    const size_t before = queue.size();
    Queued::addToQueue(queue, dpp);
    if (&queue == &pfq && queue.size() > before && cache) {
        cache->notifyPrefetchPending();
    }
}

void
CentralizedDataPrefetcher::recordDispatch(
    const CentralizedPrefetchCandidate &candidate)
{
    centralStats.dispatched++;
    centralStats.actualLevel[candidate.actualLevel]++;
    centralStats.preferredToActual[candidate.preferredLevel]
                                  [candidate.actualLevel]++;
    const unsigned src = sourceIndex(candidate.source);
    if (src < NUM_PF_SOURCES) {
        centralStats.dispatchedBySource[src]++;
    }
    if (candidate.crossPage && src < NUM_PF_SOURCES) {
        centralStats.crossPageDispatched[src][candidate.actualLevel]++;
    }
}

void
CentralizedDataPrefetcher::recordCoveredPromotion(
    const CentralizedPrefetchCandidate &candidate,
    CoveredPromotionReason reason)
{
    const unsigned src = sourceIndex(candidate.source);
    if (src < NUM_PF_SOURCES && reason < NumCoveredPromotionReasons) {
        centralStats.coveredPromotionBySourceAndReason[src][reason]++;
    }
}

void
CentralizedDataPrefetcher::recordPhysicalDuplicate(
    const CentralizedPrefetchCandidate &candidate,
    PhysicalDuplicateCoverReason reason, PrefetchSourceType)
{
    const unsigned src = sourceIndex(candidate.source);
    if (src < NUM_PF_SOURCES &&
        reason < NumPhysicalDuplicateCoverReasons) {
        centralStats.physicalDuplicateBySourceAndCover[src][reason]++;
    }
    switch (reason) {
      case L1CacheDemand:
      case L2CacheDemand:
      case L3CacheDemand:
      case L1MissQueueDemand:
      case L2MissQueueDemand:
      case L3MissQueueDemand:
        updateQuality(qualityKeyFromCandidate(candidate),
                      QualityDuplicateDemand, 0);
        break;
      default:
        break;
    }
    recordCacheDemandDuplicateTiming(candidate, reason);
}

void
CentralizedDataPrefetcher::recordCacheDemandDuplicateTiming(
    const CentralizedPrefetchCandidate &candidate,
    PhysicalDuplicateCoverReason reason)
{
    const unsigned src = sourceIndex(candidate.source);
    if (src >= NUM_PF_SOURCES) {
        return;
    }

    CacheDemandDuplicateTimingReason unknown_reason;
    CacheDemandDuplicateTimingReason resident_reason;
    CacheDemandDuplicateTimingReason demand_after_reason;
    CacheDemandDuplicateTimingReason non_demand_after_reason;
    bool have_info = false;
    Tick fill_tick = 0;
    bool had_demand = false;

    switch (reason) {
      case L1CacheDemand:
        unknown_reason = L1FillTimeUnknown;
        resident_reason = L1ResidentBeforeCandidate;
        demand_after_reason = L1DemandFillAfterCandidate;
        non_demand_after_reason = L1NonDemandFillAfterCandidate;
        have_info = cache && cache->getHitBlkFillInfo(
            candidate.paddr, candidate.key.secure, fill_tick, had_demand);
        break;
      case L2CacheDemand:
        unknown_reason = L2FillTimeUnknown;
        resident_reason = L2ResidentBeforeCandidate;
        demand_after_reason = L2DemandFillAfterCandidate;
        non_demand_after_reason = L2NonDemandFillAfterCandidate;
        have_info = l2Endpoint && l2Endpoint->cacheFillInfo(
            candidate.paddr, candidate.key.secure, fill_tick, had_demand);
        break;
      case L3CacheDemand:
        unknown_reason = L3FillTimeUnknown;
        resident_reason = L3ResidentBeforeCandidate;
        demand_after_reason = L3DemandFillAfterCandidate;
        non_demand_after_reason = L3NonDemandFillAfterCandidate;
        have_info = l3Endpoint && l3Endpoint->cacheFillInfo(
            candidate.paddr, candidate.key.secure, fill_tick, had_demand);
        break;
      default:
        return;
    }

    CacheDemandDuplicateTimingReason timing_reason = unknown_reason;
    if (have_info && fill_tick != 0) {
        if (fill_tick <= candidate.enqueueTick) {
            timing_reason = resident_reason;
        } else {
            timing_reason = had_demand ?
                demand_after_reason : non_demand_after_reason;
        }
    }
    centralStats.physicalDuplicateCacheDemandBySourceAndTiming[src]
        [timing_reason]++;
}

CentralizedDataPrefetcher::PhysicalDuplicateCoverReason
CentralizedDataPrefetcher::cacheCoverReason(
    const CacheAccessor *cache, Addr paddr, bool secure,
    PrefetchSourceType source, PhysicalDuplicateCoverReason demand_reason,
    PhysicalDuplicateCoverReason same_source_reason,
    PhysicalDuplicateCoverReason other_source_reason,
    PhysicalDuplicateCoverReason miss_queue_demand_reason,
    PhysicalDuplicateCoverReason miss_queue_same_source_reason,
    PhysicalDuplicateCoverReason miss_queue_other_source_reason,
    PhysicalDuplicateCoverReason miss_queue_unknown_reason,
    PhysicalDuplicateCoverReason write_queue_reason) const
{
    if (!cache) {
        return NumPhysicalDuplicateCoverReasons;
    }

    Request::XsMetadata metadata;
    if (cache->getHitBlkXsMetadata(paddr, secure, metadata)) {
        if (metadata.validXsMetadata && metadata.prefetchSource != PF_NONE) {
            return metadata.prefetchSource == source ?
                same_source_reason : other_source_reason;
        }
        return demand_reason;
    }
    Request::XsMetadata mshr_metadata;
    bool mshr_has_cpu = false;
    bool mshr_has_pref = false;
    if (cache->getMissQueueXsMetadata(
            paddr, secure, mshr_metadata, mshr_has_cpu, mshr_has_pref)) {
        if (mshr_has_cpu) {
            return miss_queue_demand_reason;
        }
        if (mshr_has_pref && mshr_metadata.validXsMetadata &&
            mshr_metadata.prefetchSource != PF_NONE) {
            return mshr_metadata.prefetchSource == source ?
                miss_queue_same_source_reason : miss_queue_other_source_reason;
        }
        return miss_queue_unknown_reason;
    }
    if (cache->inWriteQueue(paddr, secure)) {
        return write_queue_reason;
    }
    return NumPhysicalDuplicateCoverReasons;
}

CentralizedDataPrefetcher::PhysicalDuplicateCoverReason
CentralizedDataPrefetcher::endpointCoverReason(
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
    PhysicalDuplicateCoverReason write_queue_reason) const
{
    if (!endpoint) {
        return NumPhysicalDuplicateCoverReasons;
    }

    switch (endpoint->coverKind(paddr, secure, source)) {
      case CentralizedPrefetcherEndpoint::EndpointPendingCover:
        return pending_reason;
      case CentralizedPrefetcherEndpoint::CacheDemandCover:
        return demand_reason;
      case CentralizedPrefetcherEndpoint::CachePrefetchSameSourceCover:
        return same_source_reason;
      case CentralizedPrefetcherEndpoint::CachePrefetchOtherSourceCover:
        return other_source_reason;
      case CentralizedPrefetcherEndpoint::MissQueueDemandCover:
        return miss_queue_demand_reason;
      case CentralizedPrefetcherEndpoint::MissQueuePrefetchSameSourceCover:
        return miss_queue_same_source_reason;
      case CentralizedPrefetcherEndpoint::MissQueuePrefetchOtherSourceCover:
        return miss_queue_other_source_reason;
      case CentralizedPrefetcherEndpoint::MissQueueUnknownCover:
        return miss_queue_unknown_reason;
      case CentralizedPrefetcherEndpoint::WriteQueueCover:
        return write_queue_reason;
      case CentralizedPrefetcherEndpoint::NoCover:
        return NumPhysicalDuplicateCoverReasons;
    }
    return NumPhysicalDuplicateCoverReasons;
}

CentralizedDataPrefetcher::PhysicalDuplicateCoverReason
CentralizedDataPrefetcher::physicalDuplicateCoverReason(
    const CentralizedPrefetchCandidate &candidate) const
{
    const bool secure = candidate.key.secure;
    auto reason = cacheCoverReason(
        cache, candidate.paddr, secure, candidate.source, L1CacheDemand,
        L1CachePrefetchSameSource, L1CachePrefetchOtherSource,
        L1MissQueueDemand, L1MissQueuePrefetchSameSource,
        L1MissQueuePrefetchOtherSource, L1MissQueueUnknown, L1WriteQueue);
    if (reason != NumPhysicalDuplicateCoverReasons) {
        return reason;
    }

    reason = endpointCoverReason(
        l2Endpoint, candidate.paddr, secure, candidate.source,
        L2EndpointPending, L2CacheDemand, L2CachePrefetchSameSource,
        L2CachePrefetchOtherSource, L2MissQueueDemand,
        L2MissQueuePrefetchSameSource, L2MissQueuePrefetchOtherSource,
        L2MissQueueUnknown, L2WriteQueue);
    if (reason != NumPhysicalDuplicateCoverReasons) {
        return reason;
    }

    return endpointCoverReason(
        l3Endpoint, candidate.paddr, secure, candidate.source,
        L3EndpointPending, L3CacheDemand, L3CachePrefetchSameSource,
        L3CachePrefetchOtherSource, L3MissQueueDemand,
        L3MissQueuePrefetchSameSource, L3MissQueuePrefetchOtherSource,
        L3MissQueueUnknown, L3WriteQueue);
}

void
CentralizedDataPrefetcher::dropCandidate(
    const CentralizedPrefetchCandidate &candidate, DropReason reason,
    bool erase_pending)
{
    if (erase_pending) {
        fatal_if(pending.erase(candidate.key) == 0,
                 "%s: dropping unknown centralized candidate %#x",
                 name(), candidate.vaddr);
    }
    if (candidate.physicalRegistered) {
        pendingPhysical.erase(candidate.physicalKey);
    }
    centralStats.dropped++;
    centralStats.dropReason[reason]++;
    const unsigned src = sourceIndex(candidate.source);
    if (src < NUM_PF_SOURCES) {
        centralStats.droppedBySourceAndReason[src][reason]++;
    }
    if (candidate.crossPage) {
        centralStats.crossPageDroppedByReason[reason]++;
    }
}

void
CentralizedDataPrefetcher::failCompletion(
    const CentralizedCandidateKey &key,
    const CentralizedPhysicalKey &physical_key,
    PrefetchSourceType source, DropReason reason, bool cross_page)
{
    fatal_if(pending.erase(key) == 0,
             "%s: completing unknown centralized candidate %#x",
             name(), key.block);
    pendingPhysical.erase(physical_key);
    centralStats.dropped++;
    centralStats.dropReason[reason]++;
    const unsigned src = sourceIndex(source);
    if (src < NUM_PF_SOURCES) {
        centralStats.droppedBySourceAndReason[src][reason]++;
    }
    if (cross_page) {
        centralStats.crossPageDroppedByReason[reason]++;
    }
}

PacketPtr
CentralizedDataPrefetcher::getPacket()
{
    while (!pfq.empty()) {
        if (pfq.front().tick > curTick()) {
            if (cache) {
                cache->notifyPrefetchPending();
            }
            return nullptr;
        }
        PacketPtr queued = pfq.front().pkt;
        const auto pkey =
            physicalKey(queued->getAddr(), queued->isSecure());
        if (cache &&
            (cache->inCache(queued->getAddr(), queued->isSecure()) ||
             cache->inMissQueue(queued->getAddr(), queued->isSecure()) ||
             cache->inWriteQueue(queued->getAddr(), queued->isSecure()))) {
            pfq.pop_front();
            delete queued;
            auto completion = localCompletions.find(pkey);
            fatal_if(completion == localCompletions.end(),
                     "%s: missing local completion for %#x",
                     name(), pkey.block);
            const CentralizedCompletion failed = completion->second;
            localCompletions.erase(completion);
            endpointFailed(failed, PhysicalDuplicate);
            continue;
        }
        if (!cache ||
            cache->prefetchMshrCredits(queued->getAddr()) <
                l1MinMshrCredits) {
            pfq.pop_front();
            delete queued;
            auto completion = localCompletions.find(pkey);
            fatal_if(completion == localCompletions.end(),
                     "%s: missing local completion for %#x",
                     name(), pkey.block);
            const CentralizedCompletion failed = completion->second;
            localCompletions.erase(completion);
            endpointFailed(failed, IssueCreditDrop);
            continue;
        }
        PacketPtr pkt = Queued::getPacket();
        if (!pkt) {
            if (cache) {
                cache->notifyPrefetchPending();
            }
            return nullptr;
        }
        return pkt;
    }
    return nullptr;
}

void
CentralizedDataPrefetcher::notifyPrefetchResult(
    const PacketPtr &pkt, bool admitted)
{
    const auto pkey = physicalKey(pkt->getAddr(), pkt->isSecure());
    auto it = localCompletions.find(pkey);
    fatal_if(it == localCompletions.end(),
             "%s: completed local packet has no completion for %#x",
             name(), pkey.block);
    const CentralizedCompletion completion = it->second;
    localCompletions.erase(it);
    if (admitted) {
        recordCrossPagePrefetchIssued(pkt->req->getXsMetadata());
        endpointAccepted(completion);
    } else {
        endpointFailed(completion, PhysicalDuplicate);
    }
}

void
CentralizedDataPrefetcher::endpointAccepted(
    const CentralizedCompletion &completion)
{
    fatal_if(pending.erase(completion.key) == 0 ||
             pendingPhysical.find(completion.physicalKey) ==
                 pendingPhysical.end(),
             "%s: accepted unknown centralized candidate %#x",
             name(), completion.physicalKey.block);
    deferPhysicalRelease(completion.physicalKey);
    centralStats.issued++;
    const unsigned src = sourceIndex(completion.source);
    if (src < NUM_PF_SOURCES) {
        centralStats.admittedBySource[src]++;
        centralStats.issuedBySource[src]++;
        if (completion.candidate.crossPage) {
            centralStats.crossPageIssued[src][completion.actualLevel]++;
        }
    }
}

void
CentralizedDataPrefetcher::endpointFailed(
    const CentralizedCompletion &completion, DropReason reason)
{
    if (reason == IssueCreditDrop) {
        // The distance-derived target level is a strict placement decision.
        // If the selected level loses credits after enqueue, do not retry by
        // sinking the same request into a deeper level.
    }
    if (reason == PhysicalDuplicate) {
        const auto cover_reason = physicalDuplicateCoverReason(
            completion.candidate);
        if (cover_reason != NumPhysicalDuplicateCoverReasons) {
            recordPhysicalDuplicate(completion.candidate, cover_reason);
        }
    }
    failCompletion(completion.key, completion.physicalKey,
                   completion.source, reason,
                   completion.candidate.crossPage);
}

} // namespace prefetch
} // namespace gem5
