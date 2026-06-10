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

#include "mem/cache/prefetch/amd_region_stream.hh"

#include <algorithm>

#include "base/logging.hh"
#include "params/AMDRegionStreamPrefetchers.hh"

namespace gem5
{

namespace prefetch
{

AMDRegionStreamPrefetchers::AMDRegionStreamPrefetchers(
    const AMDRegionStreamPrefetchersParams &p)
    : Base(p),
      streamPrefetcher(p.stream_prefetcher),
      regionPrefetcher(p.region_prefetcher),
      controlledStreamPrefetcher(
          dynamic_cast<AMDContiguousStreamPrefetcher *>(p.stream_prefetcher)),
      blockStreamOnRegionPending(p.block_stream_on_region_pending),
      statsRegionStream(this)
{
    fatal_if(streamPrefetcher == nullptr,
             "AMDRegionStreamPrefetchers needs a stream prefetcher");
    fatal_if(regionPrefetcher == nullptr,
             "AMDRegionStreamPrefetchers needs a region prefetcher");
    fatal_if(streamPrefetcher == regionPrefetcher,
             "AMDRegionStreamPrefetchers children must be distinct");
    fatal_if(blockStreamOnRegionPending &&
                 controlledStreamPrefetcher == nullptr,
             "AMDRegionStreamPrefetchers strict stream blocking requires "
             "AMDContiguousStreamPrefetcher as the stream child");
}

void
AMDRegionStreamPrefetchers::setParentInfo(System *sys, ProbeManager *pm,
                                          CacheAccessor *_cache,
                                          unsigned blk_size)
{
    Base::setParentInfo(sys, pm, _cache, blk_size);

    /*
     * The coordinator owns cache probe notifications.  Children still get
     * system and block-size information, but a null ProbeManager prevents
     * direct registration so stream blocking can be enforced centrally.
     */
    streamPrefetcher->setParentInfo(sys, nullptr, _cache, blk_size);
    regionPrefetcher->setParentInfo(sys, nullptr, _cache, blk_size);
}

void
AMDRegionStreamPrefetchers::addTLB(BaseTLB *tlb, bool functional)
{
    Base::addTLB(tlb, functional);
    streamPrefetcher->addTLB(tlb, functional);
    regionPrefetcher->addTLB(tlb, functional);
}

bool
AMDRegionStreamPrefetchers::regionHasPendingPrefetches() const
{
    return regionPrefetcher->hasPendingPacket();
}

void
AMDRegionStreamPrefetchers::notify(const PacketPtr &pkt,
                                   const PrefetchInfo &pfi)
{
    regionPrefetcher->notify(pkt, pfi);
    statsRegionStream.regionNotifications++;

    const bool block_new_stream =
        blockStreamOnRegionPending && regionHasPendingPrefetches();
    if (controlledStreamPrefetcher != nullptr) {
        controlledStreamPrefetcher->notifyWithNewStreamControl(
            pkt, pfi, !block_new_stream);
        statsRegionStream.streamNotifications++;
        if (block_new_stream) {
            statsRegionStream.streamBlockedByRegion++;
        }
        return;
    }

    if (block_new_stream) {
        statsRegionStream.streamBlockedByRegion++;
        return;
    }

    streamPrefetcher->notify(pkt, pfi);
    statsRegionStream.streamNotifications++;
}

void
AMDRegionStreamPrefetchers::notifyFill(const PacketPtr &pkt)
{
    regionPrefetcher->notifyFill(pkt);
    streamPrefetcher->notifyFill(pkt);
}

Tick
AMDRegionStreamPrefetchers::nextPrefetchReadyTime() const
{
    return std::min(regionPrefetcher->nextPrefetchReadyTime(),
                    streamPrefetcher->nextPrefetchReadyTime());
}

bool
AMDRegionStreamPrefetchers::hasPendingPacket()
{
    return regionPrefetcher->hasPendingPacket() ||
           streamPrefetcher->hasPendingPacket();
}

PacketPtr
AMDRegionStreamPrefetchers::getPacket()
{
    if (regionPrefetcher->nextPrefetchReadyTime() <= curTick()) {
        PacketPtr pkt = regionPrefetcher->getPacket();
        panic_if(!pkt, "Region prefetcher is ready but returned no packet");
        prefetchStats.pfIssued++;
        issuedPrefetches++;
        statsRegionStream.regionPacketsIssued++;
        return pkt;
    }

    if (streamPrefetcher->nextPrefetchReadyTime() <= curTick()) {
        PacketPtr pkt = streamPrefetcher->getPacket();
        panic_if(!pkt, "Stream prefetcher is ready but returned no packet");
        prefetchStats.pfIssued++;
        issuedPrefetches++;
        statsRegionStream.streamPacketsIssued++;
        return pkt;
    }

    return nullptr;
}

void
AMDRegionStreamPrefetchers::prefetchUnused(PrefetchSourceType pfSource)
{
    Base::prefetchUnused(pfSource);
    regionPrefetcher->prefetchUnused(pfSource);
    streamPrefetcher->prefetchUnused(pfSource);
}

void
AMDRegionStreamPrefetchers::prefetchUnused(Addr paddr,
                                           PrefetchSourceType pfSource)
{
    Base::prefetchUnused(paddr, pfSource);
    regionPrefetcher->prefetchUnused(paddr, pfSource);
    streamPrefetcher->prefetchUnused(paddr, pfSource);
}

void
AMDRegionStreamPrefetchers::incrDemandMhsrMisses()
{
    Base::incrDemandMhsrMisses();
    regionPrefetcher->incrDemandMhsrMisses();
    streamPrefetcher->incrDemandMhsrMisses();
}

AMDRegionStreamPrefetchers::AMDRegionStreamStats::AMDRegionStreamStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(regionNotifications, statistics::units::Count::get(),
               "access notifications forwarded to the region prefetcher"),
      ADD_STAT(streamNotifications, statistics::units::Count::get(),
               "access notifications forwarded to the stream prefetcher"),
      ADD_STAT(streamBlockedByRegion, statistics::units::Count::get(),
               "stream notifications blocked by pending region prefetches"),
      ADD_STAT(regionPacketsIssued, statistics::units::Count::get(),
               "prefetch packets issued from the region child"),
      ADD_STAT(streamPacketsIssued, statistics::units::Count::get(),
               "prefetch packets issued from the stream child")
{}

} // namespace prefetch
} // namespace gem5
