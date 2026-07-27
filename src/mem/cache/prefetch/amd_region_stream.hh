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

#ifndef __MEM_CACHE_PREFETCH_AMD_REGION_STREAM_HH__
#define __MEM_CACHE_PREFETCH_AMD_REGION_STREAM_HH__

#include "base/statistics.hh"
#include "mem/cache/prefetch/amd_contiguous_stream.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct AMDRegionStreamPrefetchersParams;

namespace prefetch
{

class AMDRegionStreamPrefetchers : public Queued
{
  private:
    Base *streamPrefetcher;
    Base *regionPrefetcher;
    AMDContiguousStreamPrefetcher *controlledStreamPrefetcher;
    const bool blockStreamOnRegionPending;

    struct AMDRegionStreamStats : public statistics::Group
    {
        AMDRegionStreamStats(statistics::Group *parent);

        statistics::Scalar regionNotifications;
        statistics::Scalar streamNotifications;
        statistics::Scalar streamBlockedByRegion;
        statistics::Scalar regionPacketsIssued;
        statistics::Scalar streamPacketsIssued;
    } statsRegionStream;

    bool regionHasPendingPrefetches() const;

  public:
    AMDRegionStreamPrefetchers(const AMDRegionStreamPrefetchersParams &p);
    ~AMDRegionStreamPrefetchers() = default;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                       unsigned blk_size) override;
    void addTLB(BaseTLB *tlb, bool functional) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
    void notifyFill(const PacketPtr &pkt) override;

    PacketPtr getPacket() override;
    bool hasPendingPacket() override;
    Tick nextPrefetchReadyTime() const override;
    void prefetchUnused(PrefetchSourceType pfSource) override;
    void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) override;
    void incrDemandMhsrMisses() override;
    void rxHint(BaseMMU::Translation *dpp) override {}
    void pfHitNotify(float accuracy, PrefetchSourceType pfSource,
                     const PacketPtr &pkt) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_AMD_REGION_STREAM_HH__
