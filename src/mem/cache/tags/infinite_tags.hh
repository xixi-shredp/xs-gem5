/*
 * Infinite-capacity cache tag store for limit studies.
 */

#ifndef __MEM_CACHE_TAGS_INFINITE_TAGS_HH__
#define __MEM_CACHE_TAGS_INFINITE_TAGS_HH__

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/cache/tags/base.hh"
#include "params/InfiniteTags.hh"

namespace gem5
{

class InfiniteTags : public BaseTags
{
  private:
    struct BlockKey
    {
        Addr addr;
        bool secure;

        bool operator==(const BlockKey &other) const
        {
            return addr == other.addr && secure == other.secure;
        }
    };

    struct BlockKeyHash
    {
        std::size_t operator()(const BlockKey &key) const
        {
            return std::hash<Addr>{}(key.addr) ^
                   (static_cast<std::size_t>(key.secure) << 1);
        }
    };

    std::vector<std::unique_ptr<CacheBlk>> blocks;
    std::vector<std::unique_ptr<uint8_t[]>> blockData;
    std::unordered_map<BlockKey, CacheBlk *, BlockKeyHash> index;

    BlockKey makeKey(Addr addr, bool is_secure) const;

  public:
    typedef InfiniteTagsParams Params;

    InfiniteTags(const Params &p);

    void tagsInit() override;
    CacheBlk *findBlock(Addr addr, bool is_secure) const override;
    ReplaceableEntry *findBlockBySetAndWay(int set, int way) const override;
    CacheBlk *accessBlock(const PacketPtr pkt, Cycles &lat) override;
    CacheBlk *findVictim(Addr addr, const bool is_secure,
                         const std::size_t size,
                         std::vector<CacheBlk*> &evict_blks) override;
    Addr extractTag(const Addr addr) const override;
    void insertBlock(const PacketPtr pkt, CacheBlk *blk) override;
    void invalidate(CacheBlk *blk) override;
    void moveBlock(CacheBlk *src_blk, CacheBlk *dest_blk) override;
    Addr regenerateBlkAddr(const CacheBlk *blk) const override;
    void forEachBlk(std::function<void(CacheBlk &)> visitor) override;
    bool anyBlk(std::function<bool(CacheBlk &)> visitor) override;
};

} // namespace gem5

#endif // __MEM_CACHE_TAGS_INFINITE_TAGS_HH__
