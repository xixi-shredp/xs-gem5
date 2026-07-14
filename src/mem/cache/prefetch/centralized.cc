#include "mem/cache/prefetch/centralized.hh"

#include <algorithm>
#include <limits>

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

CentralizedPrefetchCandidate::CentralizedPrefetchCandidate(
    Addr vaddr, int32_t priority, int depth, PrefetchSourceType source,
    uint32_t distance, uint8_t preferred_level, uint64_t sequence,
    const CentralizedCandidateKey &key,
    const Base::PFtriggerInfo &trigger)
  : vaddr(vaddr), paddr(0), paddrValid(false), physicalRegistered(false),
    priority(priority), depth(depth), source(source), distance(distance),
    preferredLevel(preferred_level), actualLevel(0), sequence(sequence),
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
    ADD_STAT(queueOccupancy, statistics::units::Count::get(),
             "total endpoint queue occupancy")
{
    queueOccupancy.init(0, queue_size, 1);
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
    const CentralizedDataPrefetcher *owner, Addr paddr) const
{
    if (!cache || !owner || totalOccupancy() >= queueSize ||
        cache->prefetchMshrCredits(paddr) < minMshrCredits) {
        return false;
    }
    const auto it = ingress.find(owner->coreId());
    return it != ingress.end() && it->second.owner == owner &&
           it->second.queue.size() < perCoreQueueSize;
}

CentralizedPhysicalKey
CentralizedPrefetcherEndpoint::physicalKey(Addr paddr, bool secure) const
{
    return {blockAddress(paddr), secure};
}

bool
CentralizedPrefetcherEndpoint::contains(Addr paddr, bool secure) const
{
    const auto key = physicalKey(paddr, secure);
    return completions.find(key) != completions.end() ||
           (cache && (cache->inCache(key.block, secure) ||
                      cache->inMissQueue(key.block, secure) ||
                      cache->inWriteQueue(key.block, secure)));
}

bool
CentralizedPrefetcherEndpoint::enqueue(
    const CentralizedPrefetchCandidate &candidate,
    CentralizedDataPrefetcher *owner)
{
    if (!owner || !candidate.paddrValid ||
        candidate.actualLevel != cacheLevel) {
        endpointStats.rejectedInsert++;
        return false;
    }
    auto ingress_it = ingress.find(owner->coreId());
    fatal_if(ingress_it == ingress.end() ||
             ingress_it->second.owner != owner,
             "%s: unregistered engine/core-id pair", name());
    if (totalOccupancy() >= queueSize) {
        endpointStats.rejectedFull++;
        return false;
    }
    if (ingress_it->second.queue.size() >= perCoreQueueSize) {
        endpointStats.rejectedPerCoreFull++;
        return false;
    }
    if (!cache ||
        cache->prefetchMshrCredits(candidate.paddr) < minMshrCredits) {
        endpointStats.rejectedCredit++;
        return false;
    }
    if (contains(candidate.paddr, candidate.key.secure)) {
        endpointStats.rejectedDuplicate++;
        return false;
    }
    const bool inserted = completions.emplace(
        candidate.physicalKey,
        CentralizedCompletion(owner, candidate)).second;
    if (!inserted) {
        endpointStats.rejectedDuplicate++;
        return false;
    }
    ingress_it->second.queue.push_back(candidate);
    ingressOccupancy++;
    endpointStats.accepted++;
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
        candidate.preferredLevel, candidate.actualLevel));
    DeferredPacket dpp(this, new_pfi, 0, candidate.priority);
    dpp.pfahead = false;
    dpp.pfahead_host = 0;
    const Tick ready = curTick() + clockPeriod() * latency;
    dpp.createPkt(candidate.paddr, blkSize, requestorId, tagPrefetch,
                  ready, candidate.source, candidate.depth,
                  candidate.distance, candidate.preferredLevel,
                  candidate.actualLevel);
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
            failCompletion(candidate.physicalKey, true, false);
            continue;
        }
        if (!insertReady(candidate)) {
            endpointStats.rejectedInsert++;
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
            failCompletion(pkey, false, true);
            continue;
        }
        if (!cache ||
            cache->prefetchMshrCredits(queued->getAddr()) <
                minMshrCredits) {
            pfq.pop_front();
            delete queued;
            endpointStats.issueCreditDrop++;
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
        endpointStats.issued++;
        completion.owner->endpointAccepted(completion);
    } else {
        endpointStats.rejectedInsert++;
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
    ADD_STAT(issuedBySource, statistics::units::Count::get(),
             "issued candidates by source"),
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
    ADD_STAT(queueOccupancy, statistics::units::Count::get(),
             "ready plus translating candidates")
{
    using namespace statistics;
    generatedBySource.init(NUM_PF_SOURCES).flags(total);
    issuedBySource.init(NUM_PF_SOURCES).flags(total);
    preferredLevel.init(4).flags(total);
    actualLevel.init(4).flags(total);
    dropReason.init(NumDropReasons).flags(total);
    distanceBucket.init(6).flags(total);
    preferredToActual.init(4, 4);
    queueOccupancy.init(0, queue_size, 1);
}

CentralizedDataPrefetcher::Translation::Translation(
    CentralizedDataPrefetcher *owner,
    const CentralizedPrefetchCandidate &candidate,
    const RequestPtr &request, ThreadContext *tc)
  : owner(owner), candidate(candidate), request(request), tc(tc),
    completed(false), failed(false)
{
}

void
CentralizedDataPrefetcher::Translation::finish(
    const Fault &fault, const RequestPtr &, ThreadContext *, BaseMMU::Mode)
{
    failed = fault != NoFault;
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
    nextSequence(0), dispatchEvent([this] { dispatch(); }, name()),
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
    centralStats.preferredLevel[preferred]++;
    centralStats.distanceBucket[distanceBucket(distance)]++;
    const auto key = keyFromPfi(pfi, target);
    CentralizedPrefetchCandidate candidate(
        target, command.priority, command.depth, command.pfSource,
        distance, preferred, nextSequence++, key, trigger);
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
        it = translations.erase(it);
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
        if (!registerPhysical(candidate)) {
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
        if (!dispatchCandidate(candidate)) {
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
        if (!registerPhysical(candidate)) {
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
    ThreadContext *tc = system->threads[context];
    translations.emplace_back(std::make_unique<Translation>(
        this, candidate, request, tc));
    Translation *translation = translations.back().get();
    centralStats.translationRequests++;
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

bool
CentralizedDataPrefetcher::dispatchCandidate(
    CentralizedPrefetchCandidate &candidate, unsigned first_level)
{
    const bool secure = candidate.key.secure;
    if ((cache && (cache->inCache(candidate.paddr, secure) ||
                   cache->inMissQueue(candidate.paddr, secure) ||
                   cache->inWriteQueue(candidate.paddr, secure))) ||
        (l2Endpoint &&
         l2Endpoint->contains(candidate.paddr, secure)) ||
        (l3Endpoint &&
         l3Endpoint->contains(candidate.paddr, secure))) {
        dropCandidate(candidate, PhysicalDuplicate, true);
        return true;
    }

    const unsigned deepest = l3Endpoint ? 3 : (l2Endpoint ? 2 : 1);
    const unsigned first = first_level ? first_level :
        std::min<unsigned>(candidate.preferredLevel, deepest);
    if (first > deepest) {
        return false;
    }
    for (unsigned level = first; level <= deepest; ++level) {
        if (level == 1) {
            if (!cache || localQueueOccupancy() >= queueSize ||
                cache->prefetchMshrCredits(candidate.paddr) <
                    l1MinMshrCredits) {
                continue;
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
            level == 2 ? l2Endpoint : l3Endpoint;
        if (!endpoint || !endpoint->canAccept(this, candidate.paddr)) {
            continue;
        }
        candidate.actualLevel = level;
        if (endpoint->enqueue(candidate, this)) {
            recordDispatch(candidate);
            return true;
        }
    }
    return false;
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
    new_pfi.setXsMetadata(Request::XsMetadata(
        candidate.source, candidate.depth, candidate.distance,
        candidate.preferredLevel, candidate.actualLevel));
    DeferredPacket dpp(this, new_pfi, 0, candidate.priority);
    dpp.pfahead = false;
    dpp.pfahead_host = 0;
    const Tick ready = curTick() + clockPeriod() * latency;
    dpp.createPkt(candidate.paddr, blkSize, requestorId, tagPrefetch,
                  ready, candidate.source, candidate.depth,
                  candidate.distance, candidate.preferredLevel,
                  candidate.actualLevel);
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
}

void
CentralizedDataPrefetcher::failCompletion(
    const CentralizedCandidateKey &key,
    const CentralizedPhysicalKey &physical_key,
    PrefetchSourceType, DropReason reason)
{
    fatal_if(pending.erase(key) == 0,
             "%s: completing unknown centralized candidate %#x",
             name(), key.block);
    pendingPhysical.erase(physical_key);
    centralStats.dropped++;
    centralStats.dropReason[reason]++;
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
        centralStats.issuedBySource[src]++;
    }
}

void
CentralizedDataPrefetcher::endpointFailed(
    const CentralizedCompletion &completion, DropReason reason)
{
    if (reason == IssueCreditDrop) {
        CentralizedPrefetchCandidate candidate = completion.candidate;
        candidate.actualLevel = 0;
        if (dispatchCandidate(candidate, completion.actualLevel + 1)) {
            return;
        }
    }
    failCompletion(completion.key, completion.physicalKey,
                   completion.source, reason);
}

} // namespace prefetch
} // namespace gem5
