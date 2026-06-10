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

#ifndef __MEM_CACHE_PREFETCH_AMD_CONTIGUOUS_STREAM_HH__
#define __MEM_CACHE_PREFETCH_AMD_CONTIGUOUS_STREAM_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct AMDContiguousStreamPrefetcherParams;

namespace prefetch
{

class AMDContiguousStreamPrefetcher : public Queued
{
  private:
    struct StreamEntry
    {
        bool valid = false;
        Addr lastAddr = 0;
        int direction = 0;
        RequestorID requestorId = 0;
        bool secure = false;
        uint64_t lastTouch = 0;
    };

    struct LastAccessEntry
    {
        bool valid = false;
        Addr addr = 0;
        RequestorID requestorId = 0;
        bool secure = false;
        uint64_t lastTouch = 0;
    };

    const unsigned streamEntries;
    const unsigned lastAccessEntries;
    const unsigned degree;
    const bool useRequestorId;

    uint64_t accessCounter = 0;
    bool allowNewStreamThisNotify = true;
    std::vector<StreamEntry> streamTable;
    std::vector<LastAccessEntry> lastAccessTable;

    struct AMDContiguousStreamStats : public statistics::Group
    {
        AMDContiguousStreamStats(statistics::Group *parent);

        statistics::Scalar streamsCreated;
        statistics::Scalar streamUpdates;
        statistics::Scalar newStreamsBlocked;
        statistics::Scalar prefetchCandidates;
    } statsAmdStream;

    bool requestorMatches(RequestorID lhs, RequestorID rhs) const;
    bool sameContext(const StreamEntry &entry, const PrefetchInfo &pfi) const;
    bool sameContext(const LastAccessEntry &entry,
                     const PrefetchInfo &pfi) const;
    bool adjacentDirection(Addr previous, Addr current, int &direction) const;
    bool offsetAddress(Addr base, int direction, unsigned distance,
                       Addr &result) const;
    LastAccessEntry &findOrAllocateLastAccess(const PrefetchInfo &pfi);
    StreamEntry &allocateStream(const PrefetchInfo &pfi, Addr blk_addr,
                                int direction);
    void generateStreamPrefetches(Addr blk_addr, int direction,
                                  std::vector<AddrPriority> &addresses);

  public:
    AMDContiguousStreamPrefetcher(
        const AMDContiguousStreamPrefetcherParams &p);
    ~AMDContiguousStreamPrefetcher() = default;

    void notifyWithNewStreamControl(const CacheAccessProbeArg &acc, bool miss,
                                    bool allow_new_stream);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_AMD_CONTIGUOUS_STREAM_HH__
