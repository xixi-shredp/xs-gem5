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
 * INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_CACHE_PREFETCH_AMD_RIP_REGION_HH__
#define __MEM_CACHE_PREFETCH_AMD_RIP_REGION_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct AMDRIPRegionPrefetcherParams;

namespace prefetch
{

class AMDRIPRegionPrefetcher : public Queued
{
  private:
    static constexpr unsigned secondLineMinusBit = 0;
    static constexpr unsigned secondLineHomeBit = 1;
    static constexpr unsigned secondLinePlusBit = 2;

    struct LineEntry
    {
        bool valid = false;
        Addr homeAddr = 0;
        Addr rip = 0;
        RequestorID requestorId = 0;
        bool secure = false;
        unsigned addressOffset = 0;
        uint64_t accessBits = 0;
        uint8_t secondLineAccessBits = 0;
        uint64_t lastTouch = 0;
    };

    struct HistoryEntry
    {
        std::vector<uint8_t> counters;

        HistoryEntry() = default;
        explicit HistoryEntry(unsigned region_lines)
            : counters(region_lines, 0)
        {}
    };

    struct Candidate
    {
        Addr addr = 0;
        int32_t priority = 0;
        unsigned distance = 0;
    };

    const unsigned lineEntryEntries;
    const unsigned regionHistoryEntries;
    const unsigned negativeLines;
    const unsigned positiveLines;
    const unsigned regionLineCount;
    const unsigned homeIndex;
    const unsigned ripBits;
    const unsigned addressOffsetShift;
    const unsigned addressOffsetBits;
    const unsigned counterBits;
    const unsigned counterThreshold;
    const unsigned minPatternBits;
    const bool useRequestorId;
    const unsigned degree;
    const uint8_t counterMax;
    const Addr ripMask;
    const Addr addressOffsetMask;

    uint64_t accessCounter = 0;
    std::vector<LineEntry> lineEntryTable;
    std::vector<HistoryEntry> regionHistoryTable;

    struct AMDRIPRegionStats : public statistics::Group
    {
        AMDRIPRegionStats(statistics::Group *parent);

        statistics::Scalar noPcMisses;
        statistics::Scalar lineEntryAllocations;
        statistics::Scalar lineEntryUpdates;
        statistics::Scalar lineEntryEvictions;
        statistics::Scalar patternsRecorded;
        statistics::Scalar sequentialFiltered;
        statistics::Scalar sparseFiltered;
        statistics::Scalar historyLookups;
        statistics::Scalar prefetchCandidates;
    } statsAmdRip;

    Addr normalizeRip(Addr pc) const;
    unsigned addressOffsetFromAddr(Addr addr) const;
    unsigned makeHistoryIndex(Addr rip, unsigned address_offset) const;
    bool requestorMatches(const LineEntry &entry,
                          const PrefetchInfo &pfi) const;
    int lineOffsetFromHome(Addr home_addr, Addr blk_addr) const;
    unsigned offsetToIndex(int offset) const;
    int indexToOffset(unsigned index) const;
    bool offsetAddress(Addr base, int offset, Addr &result) const;
    bool touchLineEntries(const PrefetchInfo &pfi, Addr blk_addr);
    void allocateLineEntry(const PrefetchInfo &pfi, Addr blk_addr, Addr rip);
    void evictLineEntry(LineEntry &entry);
    bool isSequentialPattern(const LineEntry &entry) const;
    bool hasTrainablePattern(const LineEntry &entry) const;
    void updateRegionHistory(const LineEntry &entry);
    void generatePrefetches(const PrefetchInfo &pfi, Addr blk_addr, Addr rip,
                            std::vector<AddrPriority> &addresses);
    void saturatingIncrement(uint8_t &counter) const;
    void saturatingDecrement(uint8_t &counter) const;
    unsigned popCount(uint64_t value) const;

  public:
    AMDRIPRegionPrefetcher(const AMDRIPRegionPrefetcherParams &p);
    ~AMDRIPRegionPrefetcher() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_AMD_RIP_REGION_HH__
