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
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/cache/prefetch/amd_rip_region.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/AMDRIPRegionPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

AMDRIPRegionPrefetcher::AMDRIPRegionPrefetcher(
    const AMDRIPRegionPrefetcherParams &p)
    : Queued(p),
      lineEntryEntries(p.line_entry_entries),
      regionHistoryEntries(p.region_history_entries),
      negativeLines(p.negative_lines),
      positiveLines(p.positive_lines),
      regionLineCount(p.negative_lines + p.positive_lines + 1),
      homeIndex(p.negative_lines),
      ripBits(p.rip_bits),
      addressOffsetShift(p.address_offset_shift),
      addressOffsetBits(p.address_offset_bits),
      counterBits(p.counter_bits),
      counterThreshold(p.counter_threshold),
      minPatternBits(p.min_pattern_bits),
      useRequestorId(p.use_requestor_id),
      degree(p.degree),
      counterMax((1U << p.counter_bits) - 1),
      ripMask(p.rip_bits >= 64 ? std::numeric_limits<Addr>::max()
                               : ((Addr(1) << p.rip_bits) - 1)),
      addressOffsetMask(p.address_offset_bits >= 64
                            ? std::numeric_limits<Addr>::max()
                            : ((Addr(1) << p.address_offset_bits) - 1)),
      lineEntryTable(lineEntryEntries),
      regionHistoryTable(regionHistoryEntries, HistoryEntry(regionLineCount)),
      statsAmdRip(this)
{
    fatal_if(lineEntryEntries == 0,
             "AMDRIPRegionPrefetcher needs at least one line entry");
    fatal_if(regionHistoryEntries == 0,
             "AMDRIPRegionPrefetcher needs at least one history entry");
    fatal_if(regionLineCount <= 1 || regionLineCount > 64,
             "AMDRIPRegionPrefetcher supports 2..64 region lines");
    fatal_if(counterBits == 0 || counterBits > 7,
             "AMDRIPRegionPrefetcher supports 1..7 counter bits");
    fatal_if(counterThreshold > counterMax,
             "AMDRIPRegionPrefetcher counter threshold exceeds max value");
    fatal_if(ripBits > 64 || addressOffsetBits > 63,
             "AMDRIPRegionPrefetcher invalid RIP/offset bit width");
}

void
AMDRIPRegionPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                          std::vector<AddrPriority> &addresses)
{
    accessCounter++;

    const Addr blk_addr = blockAddress(pfi.getAddr());
    const bool matched = touchLineEntries(pfi, blk_addr);

    if (!pfi.isCacheMiss()) {
        return;
    }

    if (!pfi.hasPC()) {
        statsAmdRip.noPcMisses++;
        DPRINTF(HWPrefetch, "AMD RIP region ignores miss with no PC\n");
        return;
    }

    const Addr rip = normalizeRip(pfi.getPC());
    if (matched) {
        return;
    }

    allocateLineEntry(pfi, blk_addr, rip);
    generatePrefetches(pfi, blk_addr, rip, addresses);
}

Addr
AMDRIPRegionPrefetcher::normalizeRip(Addr pc) const
{
    return pc & ripMask;
}

unsigned
AMDRIPRegionPrefetcher::addressOffsetFromAddr(Addr addr) const
{
    return (addr >> addressOffsetShift) & addressOffsetMask;
}

unsigned
AMDRIPRegionPrefetcher::makeHistoryIndex(Addr rip,
                                         unsigned address_offset) const
{
    uint64_t value = rip & ripMask;
    value ^= (static_cast<uint64_t>(address_offset) & addressOffsetMask) << 17;
    value ^= value >> 9;
    value ^= value >> 21;

    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return value % regionHistoryEntries;
}

bool
AMDRIPRegionPrefetcher::requestorMatches(const LineEntry &entry,
                                         const PrefetchInfo &pfi) const
{
    return !useRequestorId || entry.requestorId == pfi.getRequestorId();
}

int
AMDRIPRegionPrefetcher::lineOffsetFromHome(Addr home_addr, Addr blk_addr) const
{
    if (blk_addr >= home_addr) {
        return static_cast<int>((blk_addr - home_addr) / blkSize);
    }
    return -static_cast<int>((home_addr - blk_addr) / blkSize);
}

unsigned
AMDRIPRegionPrefetcher::offsetToIndex(int offset) const
{
    return static_cast<unsigned>(offset + static_cast<int>(negativeLines));
}

int
AMDRIPRegionPrefetcher::indexToOffset(unsigned index) const
{
    return static_cast<int>(index) - static_cast<int>(negativeLines);
}

bool
AMDRIPRegionPrefetcher::offsetAddress(Addr base, int offset,
                                      Addr &result) const
{
    const Addr byte_distance =
        static_cast<Addr>(std::abs(offset)) * static_cast<Addr>(blkSize);
    if (offset < 0) {
        if (base < byte_distance) {
            return false;
        }
        result = base - byte_distance;
    } else {
        result = base + byte_distance;
    }
    return true;
}

bool
AMDRIPRegionPrefetcher::touchLineEntries(const PrefetchInfo &pfi,
                                         Addr blk_addr)
{
    bool matched = false;

    for (auto &entry : lineEntryTable) {
        if (!entry.valid || entry.secure != pfi.isSecure() ||
            !requestorMatches(entry, pfi)) {
            continue;
        }

        const int offset = lineOffsetFromHome(entry.homeAddr, blk_addr);
        if (offset < -static_cast<int>(negativeLines) ||
            offset > static_cast<int>(positiveLines)) {
            continue;
        }

        const unsigned index = offsetToIndex(offset);
        entry.accessBits |= (uint64_t(1) << index);
        if (offset == -1) {
            entry.secondLineAccessBits |= (1U << secondLineMinusBit);
        } else if (offset == 0) {
            entry.secondLineAccessBits |= (1U << secondLineHomeBit);
        } else if (offset == 1) {
            entry.secondLineAccessBits |= (1U << secondLinePlusBit);
        }
        entry.lastTouch = accessCounter;
        matched = true;
        statsAmdRip.lineEntryUpdates++;
    }

    return matched;
}

void
AMDRIPRegionPrefetcher::allocateLineEntry(const PrefetchInfo &pfi,
                                          Addr blk_addr, Addr rip)
{
    LineEntry *victim = nullptr;
    for (auto &entry : lineEntryTable) {
        if (!entry.valid) {
            victim = &entry;
            break;
        }
    }

    if (victim == nullptr) {
        victim = &lineEntryTable.front();
        for (auto &entry : lineEntryTable) {
            if (entry.lastTouch < victim->lastTouch) {
                victim = &entry;
            }
        }
        evictLineEntry(*victim);
    }

    *victim = LineEntry();
    victim->valid = true;
    victim->homeAddr = blk_addr;
    victim->rip = rip;
    victim->requestorId = pfi.getRequestorId();
    victim->secure = pfi.isSecure();
    victim->addressOffset = addressOffsetFromAddr(pfi.getAddr());
    victim->accessBits = uint64_t(1) << homeIndex;
    victim->lastTouch = accessCounter;
    statsAmdRip.lineEntryAllocations++;
}

void
AMDRIPRegionPrefetcher::evictLineEntry(LineEntry &entry)
{
    if (!entry.valid) {
        return;
    }

    statsAmdRip.lineEntryEvictions++;
    if (isSequentialPattern(entry)) {
        statsAmdRip.sequentialFiltered++;
    } else if (!hasTrainablePattern(entry)) {
        statsAmdRip.sparseFiltered++;
    } else {
        updateRegionHistory(entry);
        statsAmdRip.patternsRecorded++;
    }

    entry.valid = false;
}

bool
AMDRIPRegionPrefetcher::isSequentialPattern(const LineEntry &entry) const
{
    bool ascending =
        (entry.secondLineAccessBits & (1U << secondLinePlusBit)) != 0;
    for (unsigned offset = 1; ascending && offset <= positiveLines; ++offset) {
        ascending =
            (entry.accessBits & (uint64_t(1) << offsetToIndex(offset))) != 0;
    }

    bool descending =
        (entry.secondLineAccessBits & (1U << secondLineMinusBit)) != 0;
    for (unsigned offset = 1; descending && offset <= negativeLines;
         ++offset) {
        descending =
            (entry.accessBits &
             (uint64_t(1) << offsetToIndex(-static_cast<int>(offset)))) != 0;
    }

    return ascending || descending;
}

bool
AMDRIPRegionPrefetcher::hasTrainablePattern(const LineEntry &entry) const
{
    const uint64_t non_home = entry.accessBits & ~(uint64_t(1) << homeIndex);
    return popCount(non_home) >= minPatternBits;
}

void
AMDRIPRegionPrefetcher::updateRegionHistory(const LineEntry &entry)
{
    const unsigned history_index =
        makeHistoryIndex(entry.rip, entry.addressOffset);
    auto &history = regionHistoryTable[history_index];

    for (unsigned index = 0; index < regionLineCount; ++index) {
        if (index == homeIndex) {
            continue;
        }

        auto &counter = history.counters[index];
        if (entry.accessBits & (uint64_t(1) << index)) {
            saturatingIncrement(counter);
        } else {
            saturatingDecrement(counter);
        }
    }
}

void
AMDRIPRegionPrefetcher::generatePrefetches(
    const PrefetchInfo &pfi, Addr blk_addr, Addr rip,
    std::vector<AddrPriority> &addresses)
{
    const unsigned history_index =
        makeHistoryIndex(rip, addressOffsetFromAddr(pfi.getAddr()));
    const auto &history = regionHistoryTable[history_index];
    std::vector<Candidate> candidates;

    statsAmdRip.historyLookups++;
    for (unsigned index = 0; index < regionLineCount; ++index) {
        if (index == homeIndex || history.counters[index] < counterThreshold) {
            continue;
        }

        const int offset = indexToOffset(index);
        Addr target = 0;
        if (!offsetAddress(blk_addr, offset, target)) {
            continue;
        }
        target = blockAddress(target);
        if (target == blk_addr || inCache(target, pfi.isSecure()) ||
            inMissQueue(target, pfi.isSecure())) {
            continue;
        }

        Candidate candidate;
        candidate.addr = target;
        candidate.priority = static_cast<int32_t>(history.counters[index]);
        candidate.distance = static_cast<unsigned>(std::abs(offset));
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &lhs, const Candidate &rhs) {
                  if (lhs.priority != rhs.priority) {
                      return lhs.priority > rhs.priority;
                  }
                  return lhs.distance < rhs.distance;
              });

    for (const auto &candidate : candidates) {
        if (addresses.size() >= degree) {
            break;
        }
        addresses.emplace_back(candidate.addr, candidate.priority,
                               PrefetchSourceType::AMDRIPRegion);
        statsAmdRip.prefetchCandidates++;
    }
}

void
AMDRIPRegionPrefetcher::saturatingIncrement(uint8_t &counter) const
{
    if (counter < counterMax) {
        counter++;
    }
}

void
AMDRIPRegionPrefetcher::saturatingDecrement(uint8_t &counter) const
{
    if (counter > 0) {
        counter--;
    }
}

unsigned
AMDRIPRegionPrefetcher::popCount(uint64_t value) const
{
    unsigned count = 0;
    while (value != 0) {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

AMDRIPRegionPrefetcher::AMDRIPRegionStats::AMDRIPRegionStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(noPcMisses, statistics::units::Count::get(),
               "misses ignored because no PC/RIP was available"),
      ADD_STAT(lineEntryAllocations, statistics::units::Count::get(),
               "line entry table allocations"),
      ADD_STAT(lineEntryUpdates, statistics::units::Count::get(),
               "line entry table access-bit updates"),
      ADD_STAT(lineEntryEvictions, statistics::units::Count::get(),
               "line entry table evictions"),
      ADD_STAT(patternsRecorded, statistics::units::Count::get(),
               "non-sequential patterns recorded in the region history table"),
      ADD_STAT(sequentialFiltered, statistics::units::Count::get(),
               "line entries filtered as contiguous sequential patterns"),
      ADD_STAT(sparseFiltered, statistics::units::Count::get(),
               "line entries filtered because they had too few pattern bits"),
      ADD_STAT(historyLookups, statistics::units::Count::get(),
               "region history lookups on PC-bearing misses"),
      ADD_STAT(prefetchCandidates, statistics::units::Count::get(),
               "prefetch candidates emitted from region history counters")
{}

} // namespace prefetch
} // namespace gem5
