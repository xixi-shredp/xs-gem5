#ifndef __MEM_CACHE_XS_L2_SLICED_CACHE_ACCESSOR_HH__
#define __MEM_CACHE_XS_L2_SLICED_CACHE_ACCESSOR_HH__

#include <vector>

#include "base/types.hh"
#include "mem/cache/cache_probe_arg.hh"

namespace gem5
{

class L2CacheWrapper;

class SlicedCacheAccessor : public CacheAccessor
{
private:
    L2CacheWrapper* l2_wrapper;

    CacheAccessor* getSlice(Addr addr) const;

public:
    SlicedCacheAccessor(L2CacheWrapper* l2_wrapper)
        : l2_wrapper(l2_wrapper)
    {}

    bool inCache(Addr addr, bool is_secure) const override;
    unsigned level() const override;
    unsigned prefetchMshrCredits(Addr addr) const override;
    void notifyPrefetchPending() override;
    bool hasBeenPrefetched(Addr addr, bool is_secure) const override;
    bool hasBeenPrefetched(Addr addr, bool is_secure, RequestorID requestor) const override;
    bool hasEverBeenPrefetched(Addr addr, bool is_secure) const override;
    Request::XsMetadata getHitBlkXsMetadata(PacketPtr pkt) override;
    bool getHitBlkXsMetadata(Addr addr, bool is_secure,
                             Request::XsMetadata &metadata) const override;
    bool getHitBlkFillInfo(Addr addr, bool is_secure,
                           Tick &fill_tick, bool &had_demand) const override;
    bool inMissQueue(Addr addr, bool is_secure) const override;
    bool getMissQueueXsMetadata(Addr addr, bool is_secure,
                                Request::XsMetadata &metadata,
                                bool &has_cpu, bool &has_pref) const override;
    bool inWriteQueue(Addr addr, bool is_secure) const override;
    bool coalesce() const override;
    const uint8_t* findBlock(Addr addr, bool is_secure) const override;
};

} // namespace gem5

#endif // __MEM_CACHE_XS_L2_SLICED_CACHE_ACCESSOR_HH__
