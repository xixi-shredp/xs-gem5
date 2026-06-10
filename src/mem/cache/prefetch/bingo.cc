/**
 * @file
 * Bingo spatial data prefetcher implementation.
 */

#include "mem/cache/prefetch/bingo.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/BingoPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

Bingo::BingoStats::BingoStats(statistics::Group *parent)
    : statistics::Group(parent, "bingo"),
      ADD_STAT(triggerAccesses, statistics::units::Count::get(),
               "trigger accesses (FT misses)"),
      ADD_STAT(ftPromotions, statistics::units::Count::get(),
               "FT entries promoted to AT"),
      ADD_STAT(atEvictTrain, statistics::units::Count::get(),
               "PHT trainings due to AT LRU eviction"),
      ADD_STAT(llcEvictTrain, statistics::units::Count::get(),
               "PHT trainings due to cache eviction"),
      ADD_STAT(phtMaxHits, statistics::units::Count::get(),
               "PHT lookups matching PC+Address"),
      ADD_STAT(phtMinHits, statistics::units::Count::get(),
               "PHT lookups falling back to PC+Offset voting"),
      ADD_STAT(phtMisses, statistics::units::Count::get(),
               "PHT lookups with no match"),
      ADD_STAT(pfFromPcAddress, statistics::units::Count::get(),
               "prefetches issued from PC+Address matches"),
      ADD_STAT(pfFromPcOffset, statistics::units::Count::get(),
               "prefetches issued from PC+Offset voting"),
      ADD_STAT(coverFromPcAddress, statistics::units::Count::get(),
               "covered misses attributed to PC+Address"),
      ADD_STAT(coverFromPcOffset, statistics::units::Count::get(),
               "covered misses attributed to PC+Offset"),
      ADD_STAT(overpredFromPcAddress, statistics::units::Count::get(),
               "overpredictions attributed to PC+Address"),
      ADD_STAT(overpredFromPcOffset, statistics::units::Count::get(),
               "overpredictions attributed to PC+Offset")
{}

static unsigned
computeSets(unsigned size, unsigned ways)
{
    fatal_if(ways == 0 || size % ways != 0,
             "Bingo: pht_size must be a multiple of pht_ways");
    unsigned sets = size / ways;
    fatal_if(!isPowerOf2(sets),
             "Bingo: pht_size/pht_ways must be a power of two");
    return sets;
}

Bingo::Bingo(const BingoPrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      patternLen(p.pattern_len),
      ftSize(p.ft_size),
      atSize(p.at_size),
      phtSize(p.pht_size),
      phtWays(p.pht_ways),
      phtSets(computeSets(p.pht_size, p.pht_ways)),
      pcWidth(p.pc_width),
      minAddrWidth(p.min_addr_width),
      maxAddrWidth(p.max_addr_width),
      indexLen(floorLog2(phtSets)),
      minTagMask(((uint64_t)1 << (p.pc_width + p.min_addr_width - indexLen)) -
                 1),
      maxTagMask(((uint64_t)1 << (p.pc_width + p.max_addr_width - indexLen)) -
                 1),
      thresh(p.thresh),
      rotatePattern(p.rotate_pattern),
      pht(phtSets),
      lruClock(0),
      bingoStats(this)
{
    fatal_if(!isPowerOf2(patternLen),
             "Bingo: pattern_len must be a power of two");
    fatal_if(regionSize != patternLen * blkSize,
             "Bingo: region_size (%u) must equal pattern_len (%u) * "
             "blkSize (%u)",
             regionSize, patternLen, blkSize);
    fatal_if(maxAddrWidth < minAddrWidth,
             "Bingo: max_addr_width must be >= min_addr_width");

    for (auto &set : pht) {
        set.resize(phtWays);
        for (auto &w : set) {
            w.valid = false;
            w.tag = 0;
            w.lru = 0;
            w.pattern.assign(patternLen, false);
        }
    }
}

// ---------- FilterTable ----------
Bingo::FTEntry *
Bingo::ftFind(Addr region)
{
    auto it = ftMap.find(region);
    if (it == ftMap.end()) {
        return nullptr;
    }
    // promote to MRU (front)
    ftList.splice(ftList.begin(), ftList, it->second);
    return &(*it->second);
}

void
Bingo::ftInsert(Addr region, Addr pc, uint32_t offset)
{
    if (ftMap.count(region)) {
        return;
    }
    ftList.push_front({region, pc, offset, true});
    ftMap[region] = ftList.begin();
    while (ftList.size() > ftSize) {
        auto victim = std::prev(ftList.end());
        ftMap.erase(victim->region);
        ftList.erase(victim);
    }
}

void
Bingo::ftErase(Addr region)
{
    auto it = ftMap.find(region);
    if (it == ftMap.end()) {
        return;
    }
    ftList.erase(it->second);
    ftMap.erase(it);
}

// ---------- AccumulationTable ----------
Bingo::ATEntry *
Bingo::atFind(Addr region)
{
    auto it = atMap.find(region);
    if (it == atMap.end()) {
        return nullptr;
    }
    return &(*it->second);
}

bool
Bingo::atSetPattern(Addr region, uint32_t offset)
{
    auto it = atMap.find(region);
    if (it == atMap.end()) {
        return false;
    }
    it->second->pattern[offset] = true;
    // promote to MRU
    atList.splice(atList.begin(), atList, it->second);
    return true;
}

Bingo::ATEntry
Bingo::atInsertFromFt(const FTEntry &src, uint32_t nowOffset)
{
    ATEntry victim{0, 0, 0, {}, false};
    std::vector<bool> pattern(patternLen, false);
    pattern[src.offset] = true;
    pattern[nowOffset] = true;
    atList.push_front({src.region, src.pc, src.offset, pattern, true});
    atMap[src.region] = atList.begin();
    while (atList.size() > atSize) {
        auto vit = std::prev(atList.end());
        victim = *vit;
        atMap.erase(vit->region);
        atList.erase(vit);
    }
    return victim;
}

Bingo::ATEntry
Bingo::atErase(Addr region)
{
    ATEntry out{0, 0, 0, {}, false};
    auto it = atMap.find(region);
    if (it == atMap.end()) {
        return out;
    }
    out = *it->second;
    atList.erase(it->second);
    atMap.erase(it);
    return out;
}

// ---------- PHT helpers ----------
std::vector<bool>
Bingo::myRotate(const std::vector<bool> &x, int n)
{
    int len = (int)x.size();
    std::vector<bool> y(len, false);
    n = ((n % len) + len) % len;
    for (int i = 0; i < len; i++) {
        y[i] = x[((i - n) % len + len) % len];
    }
    return y;
}

uint64_t
Bingo::buildKey(Addr pc, Addr blockNumber) const
{
    uint64_t pcBits = pc & (((uint64_t)1 << pcWidth) - 1);
    uint64_t address = blockNumber & (((uint64_t)1 << maxAddrWidth) - 1);
    uint64_t offset = address & (((uint64_t)1 << minAddrWidth) - 1);
    uint64_t base = address >> minAddrWidth;
    uint64_t key =
        (base << (pcWidth + minAddrWidth)) | (pcBits << minAddrWidth) | offset;
    uint64_t tag = (pcBits << minAddrWidth) | offset;
    uint64_t idxMask = ((uint64_t)1 << indexLen) - 1;
    do {
        tag >>= indexLen;
        key ^= tag & idxMask;
    } while (tag > 0);
    return key;
}

std::vector<bool>
Bingo::vote(const std::vector<std::vector<bool>> &cands) const
{
    std::vector<bool> ret(patternLen, false);
    int n = (int)cands.size();
    if (n == 0) {
        return ret;
    }
    for (unsigned i = 0; i < patternLen; i++) {
        int cnt = 0;
        for (int j = 0; j < n; j++) {
            if (cands[j][i]) {
                cnt++;
            }
        }
        if ((double)cnt / n >= thresh) {
            ret[i] = true;
        }
    }
    return ret;
}

std::vector<bool>
Bingo::phtFind(Addr pc, Addr blockNumber, Event &ev)
{
    uint64_t key = buildKey(pc, blockNumber);
    uint64_t index = key & (phtSets - 1);
    uint64_t tag = key >> indexLen;
    auto &set = pht[index];

    std::vector<std::vector<bool>> minCands;
    std::vector<bool> pattern;
    int maxHitWay = -1;
    for (unsigned w = 0; w < phtWays; w++) {
        if (!set[w].valid) {
            continue;
        }
        bool maxMatch = ((set[w].tag & maxTagMask) == (tag & maxTagMask));
        bool minMatch = ((set[w].tag & minTagMask) == (tag & minTagMask));
        if (maxMatch) {
            pattern = set[w].pattern;
            maxHitWay = (int)w;
            break;
        }
        if (minMatch) {
            minCands.push_back(set[w].pattern);
        }
    }

    if (maxHitWay >= 0) {
        set[maxHitWay].lru = ++lruClock; // set_mru
        ev = PC_ADDRESS;
        bingoStats.phtMaxHits++;
    } else if (!minCands.empty()) {
        pattern = vote(minCands);
        ev = PC_OFFSET;
        bingoStats.phtMinHits++;
    } else {
        pattern.assign(patternLen, false);
        ev = EV_MISS;
        bingoStats.phtMisses++;
    }

    int offset = (int)(blockNumber % patternLen);
    if (rotatePattern) {
        pattern = myRotate(pattern, +offset);
    }
    return pattern;
}

void
Bingo::phtInsert(Addr pc, Addr blockNumber, const std::vector<bool> &patternIn)
{
    std::vector<bool> pattern = patternIn;
    int offset = (int)(blockNumber % patternLen);
    if (rotatePattern) {
        pattern = myRotate(pattern, -offset);
    }

    uint64_t key = buildKey(pc, blockNumber);
    uint64_t index = key & (phtSets - 1);
    uint64_t tag = key >> indexLen;
    auto &set = pht[index];

    // find existing tag (exact) or invalid way or LRU victim
    int victim = -1;
    uint64_t oldestLru = UINT64_MAX;
    for (unsigned w = 0; w < phtWays; w++) {
        if (set[w].valid && set[w].tag == tag) {
            set[w].pattern = pattern;
            set[w].lru = ++lruClock;
            return;
        }
        if (!set[w].valid) {
            victim = (int)w;
            oldestLru = 0;
        } else if (set[w].lru < oldestLru) {
            victim = (int)w;
            oldestLru = set[w].lru;
        }
    }
    set[victim].valid = true;
    set[victim].tag = tag;
    set[victim].pattern = pattern;
    set[victim].lru = ++lruClock;
}

// ---------- main entry points ----------
void
Bingo::calculatePrefetch(const PrefetchInfo &pfi,
                         std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Bingo: request with no PC, skipping.\n");
        return;
    }

    Addr blkAddr = blockAddress(pfi.getAddr());
    Addr blockNo = blkAddr >> lBlkSize;
    Addr region = blockNo / patternLen;
    uint32_t off = blockNo % patternLen;
    Addr pc = pfi.getPC();

    // Coverage attribution: if this demand block was previously prefetched
    // by us, count it as a covered miss tagged with the event that issued it.
    {
        auto cit = prefetchedBlocks.find(blockNo);
        if (cit != prefetchedBlocks.end()) {
            if (cit->second == PC_ADDRESS) {
                bingoStats.coverFromPcAddress++;
            } else if (cit->second == PC_OFFSET) {
                bingoStats.coverFromPcOffset++;
            }
            prefetchedBlocks.erase(cit);
        }
    }

    // (1) AT hit -> just accumulate
    if (atSetPattern(region, off)) {
        return;
    }

    // (2) FT miss -> trigger access
    if (ftFind(region) == nullptr) {
        ftInsert(region, pc, off);
        bingoStats.triggerAccesses++;
        Event ev;
        std::vector<bool> pat = phtFind(pc, blockNo, ev);
        regionEvent[region] = ev;
        if (ev == EV_MISS) {
            return;
        }
        // emit prefetches (priority = proximity to trigger offset)
        int prio = 0;
        for (unsigned i = 0; i < patternLen; i++) {
            if (!pat[i] || i == off) {
                continue;
            }
            Addr pfBlock = region * patternLen + i;
            Addr pfAddr = pfBlock << lBlkSize;
            addresses.push_back(AddrPriority(pfAddr, prio--, PrefetchSourceType::Bingo));
            prefetchedBlocks[pfBlock] = ev;
            if (ev == PC_ADDRESS) {
                bingoStats.pfFromPcAddress++;
            } else {
                bingoStats.pfFromPcOffset++;
            }
        }
        return;
    }

    // (3) FT hit with different offset -> promote to AT
    FTEntry *fe = ftFind(region);
    if (fe->offset != off) {
        FTEntry saved = *fe;
        ftErase(region);
        ATEntry victim = atInsertFromFt(saved, off);
        bingoStats.ftPromotions++;
        if (victim.valid) {
            Addr vBlock = victim.region * patternLen + victim.offset;
            phtInsert(victim.pc, vBlock, victim.pattern);
            bingoStats.atEvictTrain++;
        }
    }
}

} // namespace prefetch
} // namespace gem5
