#include "mem/cache/prefetch/l2_worker_slot.hh"

#include "base/logging.hh"
#include "debug/HWPrefetch.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

L2WorkerSlotPrefetcher::L2WorkerSlotPrefetcher(
    const L2WorkerSlotPrefetcherParams &p)
    : CompositeWithWorkerPrefetcher(p),
      vbop(p.vbop),
      pbop(p.pbop),
      tp(p.tp),
      vbopQueued(requireQueued(p.vbop, "vbop")),
      pbopQueued(requireQueued(p.pbop, "pbop")),
      tpQueued(requireQueued(p.tp, "tp")),
      enableVBOP(p.enable_vbop),
      enablePBOP(p.enable_pbop),
      enableTP(p.enable_tp)
{
    setChildFilter(vbop);
    setChildFilter(pbop);
    setChildFilter(tp);
}

Queued *
L2WorkerSlotPrefetcher::requireQueued(Base *prefetcher, const char *slot_name)
{
    if (prefetcher == nullptr) {
        return nullptr;
    }
    auto queued = dynamic_cast<Queued *>(prefetcher);
    fatal_if(queued == nullptr,
             "L2WorkerSlotPrefetcher slot %s must be a Queued prefetcher",
             slot_name);
    return queued;
}

void
L2WorkerSlotPrefetcher::setChildFilter(Base *prefetcher)
{
    if (auto bop = dynamic_cast<BOP *>(prefetcher)) {
        bop->filter = &pfLRUFilter;
    }
    if (auto stream = dynamic_cast<DespacitoStreamPrefetcher *>(prefetcher)) {
        stream->filter = &pfLRUFilter;
    }
    if (auto cmc = dynamic_cast<CMCPrefetcher *>(prefetcher)) {
        cmc->filter = &pfLRUFilter;
    }
}

void
L2WorkerSlotPrefetcher::setParentInfo(System *sys, ProbeManager *pm,
                                      CacheAccessor *_cache,
                                      unsigned blk_size)
{
    if (enableVBOP && vbop != nullptr) {
        vbop->setParentInfo(sys, pm, _cache, blk_size);
    }
    if (enablePBOP && pbop != nullptr) {
        pbop->setParentInfo(sys, pm, _cache, blk_size);
    }
    if (enableTP && tp != nullptr) {
        tp->setParentInfo(sys, pm, _cache, blk_size);
    }
    CompositeWithWorkerPrefetcher::setParentInfo(sys, pm, _cache, blk_size);
}

void
L2WorkerSlotPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    WorkerPrefetcher::notify(pkt, pfi);
    Queued::notify(pkt, pfi);
}

void
L2WorkerSlotPrefetcher::calculateSlot(
    Queued *slot, bool enabled, const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses, bool late, PrefetchSourceType source,
    bool miss_repeat)
{
    if (!enabled || slot == nullptr) {
        return;
    }

    if (auto bop = dynamic_cast<BOP *>(slot)) {
        bop->calculatePrefetch(pfi, addresses,
                               late && source == PrefetchSourceType::HWP_BOP);
        return;
    }

    if (auto stream = dynamic_cast<DespacitoStreamPrefetcher *>(slot)) {
        stream->calculatePrefetch(
            pfi, addresses,
            late && source == PrefetchSourceType::DespacitoStream);
        return;
    }

    if (auto cmc = dynamic_cast<CMCPrefetcher *>(slot)) {
        cmc->doPrefetch(pfi, addresses, late, source, false);
        return;
    }

    slot->calculatePrefetch(pfi, addresses, late, source, miss_repeat);
}

void
L2WorkerSlotPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
    PrefetchSourceType source, bool miss_repeat)
{
    calculateSlot(vbopQueued, enableVBOP, pfi, addresses, late, source,
                  miss_repeat);
    calculateSlot(pbopQueued, enablePBOP, pfi, addresses, late, source,
                  miss_repeat);
    calculateSlot(tpQueued, enableTP, pfi, addresses, late, source,
                  miss_repeat);
}

void
L2WorkerSlotPrefetcher::notifyFill(const PacketPtr &pkt)
{
    if (enableVBOP && vbop != nullptr) {
        vbop->notifyFill(pkt);
    }
    if (enablePBOP && pbop != nullptr) {
        pbop->notifyFill(pkt);
    }
    if (enableTP && tp != nullptr) {
        tp->notifyFill(pkt);
    }
}

void
L2WorkerSlotPrefetcher::notifyIns(int ins_num)
{
    if (enableVBOP && vbop != nullptr) {
        vbop->notifyIns(ins_num);
    }
    if (enablePBOP && pbop != nullptr) {
        pbop->notifyIns(ins_num);
    }
    if (enableTP && tp != nullptr) {
        tp->notifyIns(ins_num);
    }
    WorkerPrefetcher::notifyIns(ins_num);
}

void
L2WorkerSlotPrefetcher::prefetchUnused(Addr paddr,
                                       PrefetchSourceType pfSource)
{
    Base::prefetchUnused(paddr, pfSource);
    if (enableVBOP && vbop != nullptr) {
        vbop->prefetchUnused(paddr, pfSource);
    }
    if (enablePBOP && pbop != nullptr) {
        pbop->prefetchUnused(paddr, pfSource);
    }
    if (enableTP && tp != nullptr) {
        tp->prefetchUnused(paddr, pfSource);
    }
}

void
L2WorkerSlotPrefetcher::pfHitNotify(float accuracy,
                                    PrefetchSourceType pf_source,
                                    const PacketPtr &pkt)
{
    if (enableVBOP && vbop != nullptr) {
        vbop->pfHitNotify(accuracy, pf_source, pkt);
    }
    if (enablePBOP && pbop != nullptr) {
        pbop->pfHitNotify(accuracy, pf_source, pkt);
    }
    if (enableTP && tp != nullptr) {
        tp->pfHitNotify(accuracy, pf_source, pkt);
    }
}

bool
L2WorkerSlotPrefetcher::slotHasBufferedRequest(Queued *slot,
                                               bool enabled) const
{
    return enabled && slot != nullptr && slot->hasPFRequestsInBuffer();
}

bool
L2WorkerSlotPrefetcher::getSlotRequest(
    Queued *slot, bool enabled, std::vector<AddrPriority> &addresses)
{
    return enabled && slot != nullptr &&
           slot->GetPFRequestsFromBuffer(addresses);
}

bool
L2WorkerSlotPrefetcher::GetPFRequestsFromBuffer(
    std::vector<AddrPriority> &addresses)
{
    if (pfq.size() == queueSize) {
        return false;
    }

    bool sent = ticksToCycles(latestTransferTick) == ticksToCycles(curTick());
    if (!sent) {
        sent = getSlotRequest(vbopQueued, enableVBOP, addresses);
    }
    if (!sent) {
        sent = getSlotRequest(pbopQueued, enablePBOP, addresses);
    }
    if (!sent) {
        sent = getSlotRequest(tpQueued, enableTP, addresses);
    }
    return sent;
}

bool
L2WorkerSlotPrefetcher::hasPFRequestsInBuffer()
{
    return slotHasBufferedRequest(vbopQueued, enableVBOP) ||
           slotHasBufferedRequest(pbopQueued, enablePBOP) ||
           slotHasBufferedRequest(tpQueued, enableTP);
}

}  // namespace prefetch
}  // namespace gem5
