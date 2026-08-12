/*
 * Infinite-capacity cache tag store for limit studies.
 */

#include "mem/cache/tags/infinite_tags.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "sim/cur_tick.hh"
#include "sim/system.hh"

namespace gem5
{

InfiniteTags::InfiniteTags(const Params &p) : BaseTags(p)
{
    fatal_if(blkSize < 4 || !isPowerOf2(blkSize),
             "Block size must be at least 4 and a power of 2");
}

void
InfiniteTags::tagsInit()
{
}

InfiniteTags::BlockKey
InfiniteTags::makeKey(Addr addr, bool is_secure) const
{
    return {blkAlign(addr), is_secure};
}

CacheBlk *
InfiniteTags::findBlock(Addr addr, bool is_secure) const
{
    auto it = index.find(makeKey(addr, is_secure));
    return it == index.end() ? nullptr : it->second;
}

ReplaceableEntry *
InfiniteTags::findBlockBySetAndWay(int set, int way) const
{
    panic("InfiniteTags does not have set/way indexing");
}

CacheBlk *
InfiniteTags::accessBlock(const PacketPtr pkt, Cycles &lat)
{
    CacheBlk *blk = findBlock(pkt->getAddr(), pkt->isSecure());

    stats.tagAccesses += 1;
    if (blk != nullptr) {
        stats.dataAccesses += 1;
        blk->increaseRefCount();
        if (pkt->isDemand()) {
            blk->increaseDemandHits();
        }
    }

    lat = lookupLatency;
    return blk;
}

CacheBlk *
InfiniteTags::findVictim(Addr addr, const bool is_secure,
                         const std::size_t size,
                         std::vector<CacheBlk*> &evict_blks)
{
    (void)addr;
    (void)is_secure;
    (void)size;
    (void)evict_blks;

    auto blk = std::make_unique<CacheBlk>();
    auto data = std::make_unique<uint8_t[]>(blkSize);
    blk->data = data.get();

    CacheBlk *raw_blk = blk.get();
    blocks.push_back(std::move(blk));
    blockData.push_back(std::move(data));
    return raw_blk;
}

Addr
InfiniteTags::extractTag(const Addr addr) const
{
    return blkAlign(addr);
}

void
InfiniteTags::insertBlock(const PacketPtr pkt, CacheBlk *blk)
{
    BaseTags::insertBlock(pkt, blk);
    index[makeKey(pkt->getAddr(), pkt->isSecure())] = blk;
    stats.tagsInUse++;
}

void
InfiniteTags::invalidate(CacheBlk *blk)
{
    const auto key = BlockKey{regenerateBlkAddr(blk), blk->isSecure()};
    index.erase(key);
    BaseTags::invalidate(blk);
    stats.tagsInUse--;
}

void
InfiniteTags::moveBlock(CacheBlk *src_blk, CacheBlk *dest_blk)
{
    (void)src_blk;
    (void)dest_blk;
    panic("InfiniteTags does not support block relocation");
}

Addr
InfiniteTags::regenerateBlkAddr(const CacheBlk *blk) const
{
    return blk->getTag();
}

void
InfiniteTags::forEachBlk(std::function<void(CacheBlk &)> visitor)
{
    for (auto &blk : blocks) {
        visitor(*blk);
    }
}

bool
InfiniteTags::anyBlk(std::function<bool(CacheBlk &)> visitor)
{
    for (auto &blk : blocks) {
        if (visitor(*blk)) {
            return true;
        }
    }
    return false;
}

} // namespace gem5
