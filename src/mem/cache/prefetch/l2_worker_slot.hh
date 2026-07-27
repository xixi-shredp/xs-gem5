#ifndef __MEM_CACHE_PREFETCH_L2_WORKER_SLOT_HH__
#define __MEM_CACHE_PREFETCH_L2_WORKER_SLOT_HH__

#include <vector>

#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"
#include "mem/cache/prefetch/despacito_stream.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/L2WorkerSlotPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class L2WorkerSlotPrefetcher : public CompositeWithWorkerPrefetcher
{
  public:
    L2WorkerSlotPrefetcher(const L2WorkerSlotPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override {}

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType source,
                           bool miss_repeat) override;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                       unsigned blk_size) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void notifyFill(const PacketPtr &pkt) override;

    void notifyIns(int ins_num) override;

    void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) override;

    void pfHitNotify(float accuracy, PrefetchSourceType pf_source,
                     const PacketPtr &pkt) override;

    bool GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) override;
    bool hasPFRequestsInBuffer() override;

  private:
    Base *vbop;
    Base *pbop;
    Base *tp;

    Queued *vbopQueued;
    Queued *pbopQueued;
    Queued *tpQueued;

    const bool enableVBOP;
    const bool enablePBOP;
    const bool enableTP;

    Queued *requireQueued(Base *prefetcher, const char *slot_name);
    void setChildFilter(Base *prefetcher);
    void calculateSlot(Queued *slot, bool enabled, const PrefetchInfo &pfi,
                       std::vector<AddrPriority> &addresses, bool late,
                       PrefetchSourceType source, bool miss_repeat);
    bool slotHasBufferedRequest(Queued *slot, bool enabled) const;
    bool getSlotRequest(Queued *slot, bool enabled,
                        std::vector<AddrPriority> &addresses);
};

}  // namespace prefetch
}  // namespace gem5

#endif  // __MEM_CACHE_PREFETCH_L2_WORKER_SLOT_HH__
