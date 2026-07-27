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

#include "mem/cache/prefetch/pattern_merging.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/PatternMergingPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

PatternMerging::PatternMerging(const PatternMergingPrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      patternLength(p.pattern_length),
      ftEntries(p.ft_entries),
      atEntries(p.at_entries),
      optEntries(p.opt_entries),
      pptEntries(p.ppt_entries),
      pbEntries(p.pb_entries),
      ftAssoc(p.ft_assoc),
      atAssoc(p.at_assoc),
      pbAssoc(p.pb_assoc),
      ftSets(p.ft_assoc == 0 ? 0 : p.ft_entries / p.ft_assoc),
      atSets(p.at_assoc == 0 ? 0 : p.at_entries / p.at_assoc),
      pbSets(p.pb_assoc == 0 ? 0 : p.pb_entries / p.pb_assoc),
      counterBits(p.counter_bits),
      maxCounter(p.counter_bits >= 16 ? 0xffff : (1U << p.counter_bits) - 1),
      pptMonitoringRange(p.ppt_monitoring_range),
      pptPatternLength(p.ppt_monitoring_range == 0
                           ? 0
                           : (p.pattern_length + p.ppt_monitoring_range - 1) /
                                 p.ppt_monitoring_range),
      l1ThresholdPercent(p.l1_threshold_percent),
      l2ThresholdPercent(p.l2_threshold_percent),
      l2PrefetchSkipCacheLevels(p.l2_prefetch_skip_cache_levels),
      llcPrefetchSkipCacheLevels(p.llc_prefetch_skip_cache_levels),
      maxPrefetchesPerAccess(p.max_prefetches_per_access),
      filterLru(ftSets),
      activeLru(atSets),
      offsetPatternTable(p.opt_entries, CounterVector(p.pattern_length)),
      pcPatternTable(p.ppt_entries, CounterVector(pptPatternLength)),
      prefetchBufferLru(pbSets)
{
    fatal_if(regionSize == 0, "PMP region_size must be non-zero");
    fatal_if(!isPowerOf2(regionSize), "PMP region_size must be a power of 2");
    fatal_if(patternLength == 0, "PMP pattern_length must be non-zero");
    fatal_if(
        regionSize / blkSize != patternLength,
        "PMP pattern_length (%u) must match region_size / block_size (%u)",
        patternLength, (unsigned)(regionSize / blkSize));
    fatal_if(ftEntries == 0 || atEntries == 0 || optEntries == 0 ||
                 pptEntries == 0 || pbEntries == 0,
             "PMP table sizes must be non-zero");
    fatal_if(ftAssoc == 0 || atAssoc == 0 || pbAssoc == 0,
             "PMP table associativities must be non-zero");
    fatal_if(ftEntries % ftAssoc != 0 || atEntries % atAssoc != 0 ||
                 pbEntries % pbAssoc != 0,
             "PMP table entries must be divisible by their associativities");
    fatal_if(counterBits == 0 || counterBits >= 16,
             "PMP counter_bits must be in [1, 15]");
    fatal_if(pptMonitoringRange == 0,
             "PMP ppt_monitoring_range must be non-zero");
    fatal_if(l2ThresholdPercent > l1ThresholdPercent,
             "PMP l2_threshold_percent must be <= l1_threshold_percent");
    fatal_if(l2PrefetchSkipCacheLevels > 3 || llcPrefetchSkipCacheLevels > 3,
             "PMP prefetch skip cache levels must be <= 3");
    fatal_if(llcPrefetchSkipCacheLevels < l2PrefetchSkipCacheLevels,
             "PMP LLC prefetch skip level must be >= L2 prefetch skip level");
}

Addr
PatternMerging::regionAddress(Addr addr) const
{
    return roundDown(blockAddress(addr), regionSize);
}

unsigned
PatternMerging::regionOffset(Addr addr) const
{
    Addr region = regionAddress(addr);
    return (blockAddress(addr) - region) / blkSize;
}

unsigned
PatternMerging::pcIndex(Addr pc) const
{
    Addr folded = pc >> 2;
    folded ^= folded >> 7;
    folded ^= folded >> 13;
    return folded % pptEntries;
}

unsigned
PatternMerging::offsetIndex(unsigned offset) const
{
    return offset % optEntries;
}

unsigned
PatternMerging::tableSetIndex(const RegionKey &key, unsigned sets) const
{
    const Addr region_number = key.region / regionSize;
    return region_number % sets;
}

unsigned
PatternMerging::filterSetIndex(const RegionKey &key) const
{
    return tableSetIndex(key, ftSets);
}

unsigned
PatternMerging::activeSetIndex(const RegionKey &key) const
{
    return tableSetIndex(key, atSets);
}

unsigned
PatternMerging::prefetchBufferSetIndex(const RegionKey &key) const
{
    return tableSetIndex(key, pbSets);
}

void
PatternMerging::trimFilterTable(const RegionKey &key)
{
    auto &set_lru = filterLru[filterSetIndex(key)];
    while (set_lru.size() > ftAssoc) {
        RegionKey victim = set_lru.back();
        set_lru.pop_back();
        filterTable.erase(victim);
    }
}

void
PatternMerging::removeTableOrderEntry(std::deque<RegionKey> &order,
                                      const RegionKey &key)
{
    order.erase(std::remove(order.begin(), order.end(), key), order.end());
}

void
PatternMerging::touchActiveEntry(const RegionKey &key)
{
    auto &set_lru = activeLru[activeSetIndex(key)];
    removeTableOrderEntry(set_lru, key);
    set_lru.push_front(key);
}

void
PatternMerging::trimActiveTable(const RegionKey &key)
{
    auto &set_lru = activeLru[activeSetIndex(key)];
    while (set_lru.size() > atAssoc) {
        RegionKey victim = set_lru.back();
        set_lru.pop_back();
        auto it = activeTable.find(victim);
        if (it == activeTable.end()) {
            continue;
        }
        mergeActiveEntry(it->second);
        activeTable.erase(it);
        removeTableOrderEntry(set_lru, victim);
    }
}

void
PatternMerging::touchPrefetchBufferEntry(const RegionKey &key)
{
    auto &set_lru = prefetchBufferLru[prefetchBufferSetIndex(key)];
    removeTableOrderEntry(set_lru, key);
    set_lru.push_front(key);
}

void
PatternMerging::trimPrefetchBuffer(const RegionKey &key)
{
    auto &set_lru = prefetchBufferLru[prefetchBufferSetIndex(key)];
    while (set_lru.size() > pbAssoc) {
        RegionKey victim = set_lru.back();
        set_lru.pop_back();
        prefetchBuffer.erase(victim);
    }
}

void
PatternMerging::erasePrefetchBufferEntry(const RegionKey &key)
{
    prefetchBuffer.erase(key);
    removeTableOrderEntry(prefetchBufferLru[prefetchBufferSetIndex(key)], key);
}

void
PatternMerging::recordAccess(const RegionKey &key, Addr pc, unsigned offset)
{
    auto active_it = activeTable.find(key);
    if (active_it != activeTable.end()) {
        active_it->second.pattern[offset] = true;
        touchActiveEntry(key);
        return;
    }

    auto filter_it = filterTable.find(key);
    if (filter_it != filterTable.end()) {
        if (filter_it->second.triggerOffset == offset) {
            return;
        }

        ActiveEntry entry(patternLength, filter_it->second.pc,
                          filter_it->second.triggerOffset);
        entry.pattern[filter_it->second.triggerOffset] = true;
        entry.pattern[offset] = true;
        activeTable[key] = entry;
        filterTable.erase(filter_it);
        removeTableOrderEntry(filterLru[filterSetIndex(key)], key);
        touchActiveEntry(key);
        trimActiveTable(key);
        return;
    }

    auto &set_lru = filterLru[filterSetIndex(key)];
    removeTableOrderEntry(set_lru, key);
    filterTable[key] = FilterEntry{pc, offset};
    set_lru.push_front(key);
    trimFilterTable(key);
}

void
PatternMerging::mergeActiveEntry(const ActiveEntry &entry)
{
    mergePattern(offsetPatternTable[offsetIndex(entry.triggerOffset)],
                 entry.pattern, entry.triggerOffset, 1);
    mergePattern(pcPatternTable[pcIndex(entry.pc)], entry.pattern,
                 entry.triggerOffset, pptMonitoringRange);
}

void
PatternMerging::mergePattern(CounterVector &counter_vector,
                             const std::vector<bool> &pattern,
                             unsigned trigger_offset,
                             unsigned monitoring_range)
{
    std::vector<bool> touched(counter_vector.counters.size(), false);
    for (unsigned offset = 0; offset < patternLength; ++offset) {
        if (!pattern[offset]) {
            continue;
        }

        const unsigned anchored =
            (offset + patternLength - trigger_offset) % patternLength;
        const unsigned counter = anchored / monitoring_range;
        if (counter < touched.size()) {
            touched[counter] = true;
        }
    }

    for (unsigned i = 0; i < touched.size(); ++i) {
        if (touched[i] && counter_vector.counters[i] < maxCounter) {
            counter_vector.counters[i]++;
        }
    }

    if (!counter_vector.counters.empty() &&
        counter_vector.counters[0] >= maxCounter) {
        for (auto &counter : counter_vector.counters) {
            counter >>= 1;
        }
    }
}

void
PatternMerging::finishActiveRegion(const RegionKey &key)
{
    auto active_it = activeTable.find(key);
    if (active_it != activeTable.end()) {
        mergeActiveEntry(active_it->second);
        activeTable.erase(active_it);
        removeTableOrderEntry(activeLru[activeSetIndex(key)], key);
        return;
    }

    filterTable.erase(key);
    removeTableOrderEntry(filterLru[filterSetIndex(key)], key);
}

std::vector<PatternMerging::PrefetchLevel>
PatternMerging::extractPattern(const CounterVector &counter_vector,
                               unsigned monitoring_range) const
{
    std::vector<PrefetchLevel> pattern(patternLength, PrefetchLevel::None);
    if (counter_vector.counters.empty() || counter_vector.counters[0] == 0) {
        return pattern;
    }

    const unsigned time_counter = counter_vector.counters[0];
    for (unsigned anchored = 1; anchored < patternLength; ++anchored) {
        const unsigned counter = anchored / monitoring_range;
        if (counter >= counter_vector.counters.size()) {
            continue;
        }
        if (monitoring_range > 1 && counter == 0) {
            continue;
        }

        const unsigned confidence =
            (counter_vector.counters[counter] * 100) / time_counter;
        if (confidence >= l1ThresholdPercent) {
            pattern[anchored] = PrefetchLevel::L1;
        } else if (confidence >= l2ThresholdPercent) {
            pattern[anchored] = PrefetchLevel::L2;
        }
    }

    return pattern;
}

PatternMerging::PrefetchLevel
PatternMerging::downgradePrefetchLevel(PrefetchLevel level) const
{
    switch (level) {
        case PrefetchLevel::L1:
            return PrefetchLevel::L2;
        case PrefetchLevel::L2:
            return PrefetchLevel::LLC;
        default:
            return level;
    }
}

PatternMerging::PrefetchLevel
PatternMerging::arbitratePrefetchLevel(PrefetchLevel opt_level,
                                       PrefetchLevel ppt_level) const
{
    if (opt_level == PrefetchLevel::None) {
        return PrefetchLevel::None;
    }

    if (ppt_level == PrefetchLevel::None) {
        return downgradePrefetchLevel(opt_level);
    }

    if (opt_level == PrefetchLevel::L1 && ppt_level == PrefetchLevel::L1) {
        return PrefetchLevel::L1;
    }

    return PrefetchLevel::L2;
}

int32_t
PatternMerging::prefetchPriority(PrefetchLevel level) const
{
    switch (level) {
        case PrefetchLevel::L1:
            return 2;
        case PrefetchLevel::L2:
            return 1;
        case PrefetchLevel::LLC:
            return 0;
        default:
            return 0;
    }
}

uint8_t
PatternMerging::prefetchSkipCacheLevels(PrefetchLevel level) const
{
    switch (level) {
        case PrefetchLevel::L2:
            return l2PrefetchSkipCacheLevels;
        case PrefetchLevel::LLC:
            return llcPrefetchSkipCacheLevels;
        default:
            return 0;
    }
}

void
PatternMerging::installPrefetchPattern(const RegionKey &key,
                                       const PrefetchInfo &pfi,
                                       unsigned trigger_offset)
{
    const auto opt_pattern =
        extractPattern(offsetPatternTable[offsetIndex(trigger_offset)], 1);
    const auto ppt_pattern = extractPattern(
        pcPatternTable[pcIndex(pfi.getPC())], pptMonitoringRange);

    bool opt_has_prediction = false;
    for (unsigned anchored = 1; anchored < patternLength; ++anchored) {
        opt_has_prediction |= opt_pattern[anchored] != PrefetchLevel::None;
    }

    if (!opt_has_prediction) {
        erasePrefetchBufferEntry(key);
        return;
    }

    PrefetchBufferEntry entry(patternLength);
    for (unsigned anchored = 1; anchored < patternLength; ++anchored) {
        PrefetchLevel opt_level = opt_pattern[anchored];
        if (opt_level == PrefetchLevel::None) {
            continue;
        }

        const PrefetchLevel final_level =
            arbitratePrefetchLevel(opt_level, ppt_pattern[anchored]);

        const unsigned offset = (trigger_offset + anchored) % patternLength;
        entry.pattern[offset] = final_level;
    }

    prefetchBuffer[key] = entry;
    touchPrefetchBufferEntry(key);
    trimPrefetchBuffer(key);
}

Addr
PatternMerging::offsetAddress(Addr region, unsigned offset) const
{
    return region + offset * blkSize;
}

void
PatternMerging::emitPrefetchesFromBuffer(const RegionKey &key,
                                         unsigned current_offset,
                                         std::vector<AddrPriority> &addresses)
{
    auto pb_it = prefetchBuffer.find(key);
    if (pb_it == prefetchBuffer.end()) {
        return;
    }

    touchPrefetchBufferEntry(key);

    const unsigned pending = pfq.size() + pfqMissingTranslation.size();
    if (pending >= queueSize) {
        return;
    }

    const unsigned queue_space = queueSize - pending;
    const unsigned limit = std::min(queue_space, maxPrefetchesPerAccess);
    auto &entry = pb_it->second;

    for (unsigned distance = 1;
         distance < patternLength && addresses.size() < limit; ++distance) {
        const unsigned forward = (current_offset + distance) % patternLength;
        if (!entry.issued[forward] &&
            entry.pattern[forward] != PrefetchLevel::None) {
            addresses.push_back(AddrPriority(
                offsetAddress(key.region, forward),
                prefetchPriority(entry.pattern[forward]),
                PrefetchSourceType::PatternMerging));
            entry.issued[forward] = true;
            if (addresses.size() == limit) {
                break;
            }
        }

        const unsigned backward =
            (current_offset + patternLength - distance) % patternLength;
        if (backward == forward) {
            continue;
        }
        if (!entry.issued[backward] &&
            entry.pattern[backward] != PrefetchLevel::None) {
            addresses.push_back(AddrPriority(
                offsetAddress(key.region, backward),
                prefetchPriority(entry.pattern[backward]),
                PrefetchSourceType::PatternMerging));
            entry.issued[backward] = true;
        }
    }
}

void
PatternMerging::calculatePrefetch(const PrefetchInfo &pfi,
                                  std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Ignoring PMP request with no PC.\n");
        return;
    }

    const Addr region = regionAddress(pfi.getAddr());
    const unsigned offset = regionOffset(pfi.getAddr());
    RegionKey key{region, pfi.isSecure()};

    const bool trigger_access = activeTable.find(key) == activeTable.end() &&
                                filterTable.find(key) == filterTable.end();

    if (trigger_access) {
        erasePrefetchBufferEntry(key);
        installPrefetchPattern(key, pfi, offset);
    }

    recordAccess(key, pfi.getPC(), offset);
    emitPrefetchesFromBuffer(key, offset, addresses);
}

} // namespace prefetch
} // namespace gem5
