/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_CACHE_PREFETCH_PATTERN_MERGING_HH__
#define __MEM_CACHE_PREFETCH_PATTERN_MERGING_HH__

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct PatternMergingPrefetcherParams;

namespace prefetch
{

class PatternMerging : public Queued
{
  private:
    enum class PrefetchLevel : uint8_t
    {
        None = 0,
        L1,
        L2,
        LLC
    };

    struct RegionKey
    {
        Addr region;
        bool secure;

        bool
        operator<(const RegionKey &other) const
        {
            return region < other.region ||
                   (region == other.region && secure < other.secure);
        }

        bool
        operator==(const RegionKey &other) const
        {
            return region == other.region && secure == other.secure;
        }
    };

    struct FilterEntry
    {
        Addr pc = 0;
        unsigned triggerOffset = 0;
    };

    struct ActiveEntry
    {
        Addr pc = 0;
        unsigned triggerOffset = 0;
        std::vector<bool> pattern;

        ActiveEntry() = default;
        ActiveEntry(unsigned pattern_len, Addr _pc, unsigned offset)
            : pc(_pc), triggerOffset(offset), pattern(pattern_len, false)
        {}
    };

    struct CounterVector
    {
        std::vector<uint16_t> counters;

        CounterVector() = default;
        explicit CounterVector(unsigned size) : counters(size, 0) {}
    };

    struct PrefetchBufferEntry
    {
        std::vector<PrefetchLevel> pattern;
        std::vector<bool> issued;

        PrefetchBufferEntry() = default;
        explicit PrefetchBufferEntry(unsigned pattern_len)
            : pattern(pattern_len, PrefetchLevel::None),
              issued(pattern_len, false)
        {}
    };

    const Addr regionSize;
    const unsigned patternLength;
    const unsigned ftEntries;
    const unsigned atEntries;
    const unsigned optEntries;
    const unsigned pptEntries;
    const unsigned pbEntries;
    const unsigned ftAssoc;
    const unsigned atAssoc;
    const unsigned pbAssoc;
    const unsigned ftSets;
    const unsigned atSets;
    const unsigned pbSets;
    const unsigned counterBits;
    const unsigned maxCounter;
    const unsigned pptMonitoringRange;
    const unsigned pptPatternLength;
    const unsigned l1ThresholdPercent;
    const unsigned l2ThresholdPercent;
    const unsigned l2PrefetchSkipCacheLevels;
    const unsigned llcPrefetchSkipCacheLevels;
    const unsigned maxPrefetchesPerAccess;

    std::map<RegionKey, FilterEntry> filterTable;
    std::vector<std::deque<RegionKey>> filterLru;

    std::map<RegionKey, ActiveEntry> activeTable;
    std::vector<std::deque<RegionKey>> activeLru;

    std::vector<CounterVector> offsetPatternTable;
    std::vector<CounterVector> pcPatternTable;

    std::map<RegionKey, PrefetchBufferEntry> prefetchBuffer;
    std::vector<std::deque<RegionKey>> prefetchBufferLru;

    Addr regionAddress(Addr addr) const;
    unsigned regionOffset(Addr addr) const;
    unsigned pcIndex(Addr pc) const;
    unsigned offsetIndex(unsigned offset) const;
    unsigned tableSetIndex(const RegionKey &key, unsigned sets) const;
    unsigned filterSetIndex(const RegionKey &key) const;
    unsigned activeSetIndex(const RegionKey &key) const;
    unsigned prefetchBufferSetIndex(const RegionKey &key) const;

    void trimFilterTable(const RegionKey &key);
    void removeTableOrderEntry(std::deque<RegionKey> &order,
                               const RegionKey &key);
    void touchActiveEntry(const RegionKey &key);
    void trimActiveTable(const RegionKey &key);
    void touchPrefetchBufferEntry(const RegionKey &key);
    void trimPrefetchBuffer(const RegionKey &key);
    void erasePrefetchBufferEntry(const RegionKey &key);

    void recordAccess(const RegionKey &key, Addr pc, unsigned offset);
    void mergeActiveEntry(const ActiveEntry &entry);
    void mergePattern(CounterVector &counter_vector,
                      const std::vector<bool> &pattern,
                      unsigned trigger_offset, unsigned monitoring_range);
    void finishActiveRegion(const RegionKey &key);

    std::vector<PrefetchLevel>
    extractPattern(const CounterVector &counter_vector,
                   unsigned monitoring_range) const;
    PrefetchLevel arbitratePrefetchLevel(PrefetchLevel opt_level,
                                         PrefetchLevel ppt_level) const;
    PrefetchLevel downgradePrefetchLevel(PrefetchLevel level) const;
    int32_t prefetchPriority(PrefetchLevel level) const;
    uint8_t prefetchSkipCacheLevels(PrefetchLevel level) const;
    void installPrefetchPattern(const RegionKey &key, const PrefetchInfo &pfi,
                                unsigned trigger_offset);
    void emitPrefetchesFromBuffer(const RegionKey &key,
                                  unsigned current_offset,
                                  std::vector<AddrPriority> &addresses);
    Addr offsetAddress(Addr region, unsigned offset) const;

  public:
    PatternMerging(const PatternMergingPrefetcherParams &p);
    ~PatternMerging() = default;

    void rxHint(BaseMMU::Translation *) override {}

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_PATTERN_MERGING_HH__
