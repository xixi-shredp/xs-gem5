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

#include "mem/cache/prefetch/amd_contiguous_stream.hh"

#include <cstdlib>

#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/AMDContiguousStreamPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

AMDContiguousStreamPrefetcher::AMDContiguousStreamPrefetcher(
    const AMDContiguousStreamPrefetcherParams &p)
    : Queued(p),
      streamEntries(p.stream_entries),
      lastAccessEntries(p.last_access_entries),
      degree(p.degree),
      useRequestorId(p.use_requestor_id),
      streamTable(streamEntries),
      lastAccessTable(lastAccessEntries),
      statsAmdStream(this)
{
    fatal_if(streamEntries == 0,
             "AMDContiguousStreamPrefetcher needs stream entries");
    fatal_if(lastAccessEntries == 0,
             "AMDContiguousStreamPrefetcher needs last-access entries");
    fatal_if(degree == 0,
             "AMDContiguousStreamPrefetcher needs non-zero degree");
}

bool
AMDContiguousStreamPrefetcher::requestorMatches(RequestorID lhs,
                                                RequestorID rhs) const
{
    return !useRequestorId || lhs == rhs;
}

bool
AMDContiguousStreamPrefetcher::sameContext(const StreamEntry &entry,
                                           const PrefetchInfo &pfi) const
{
    return entry.valid && entry.secure == pfi.isSecure() &&
           requestorMatches(entry.requestorId, pfi.getRequestorId());
}

bool
AMDContiguousStreamPrefetcher::sameContext(const LastAccessEntry &entry,
                                           const PrefetchInfo &pfi) const
{
    return entry.valid && entry.secure == pfi.isSecure() &&
           requestorMatches(entry.requestorId, pfi.getRequestorId());
}

bool
AMDContiguousStreamPrefetcher::adjacentDirection(Addr previous, Addr current,
                                                 int &direction) const
{
    if (current == previous + blkSize) {
        direction = 1;
    } else if (previous >= blkSize && current == previous - blkSize) {
        direction = -1;
    } else {
        return false;
    }

    assert(direction == 1 || direction == -1);
    return true;
}

bool
AMDContiguousStreamPrefetcher::offsetAddress(Addr base, int direction,
                                             unsigned distance,
                                             Addr &result) const
{
    const Addr byte_distance = static_cast<Addr>(distance) * blkSize;
    if (direction < 0) {
        if (base < byte_distance) {
            return false;
        }
        result = base - byte_distance;
    } else {
        result = base + byte_distance;
    }
    return true;
}

AMDContiguousStreamPrefetcher::LastAccessEntry &
AMDContiguousStreamPrefetcher::findOrAllocateLastAccess(
    const PrefetchInfo &pfi)
{
    LastAccessEntry *victim = nullptr;
    for (auto &entry : lastAccessTable) {
        if (sameContext(entry, pfi)) {
            return entry;
        }
        if (!entry.valid && victim == nullptr) {
            victim = &entry;
        }
    }

    if (victim == nullptr) {
        victim = &lastAccessTable.front();
        for (auto &entry : lastAccessTable) {
            if (entry.lastTouch < victim->lastTouch) {
                victim = &entry;
            }
        }
    }

    *victim = LastAccessEntry();
    victim->valid = true;
    victim->requestorId = pfi.getRequestorId();
    victim->secure = pfi.isSecure();
    return *victim;
}

AMDContiguousStreamPrefetcher::StreamEntry &
AMDContiguousStreamPrefetcher::allocateStream(const PrefetchInfo &pfi,
                                              Addr blk_addr, int direction)
{
    StreamEntry *victim = nullptr;
    for (auto &entry : streamTable) {
        if (!entry.valid) {
            victim = &entry;
            break;
        }
    }

    if (victim == nullptr) {
        victim = &streamTable.front();
        for (auto &entry : streamTable) {
            if (entry.lastTouch < victim->lastTouch) {
                victim = &entry;
            }
        }
    }

    *victim = StreamEntry();
    victim->valid = true;
    victim->lastAddr = blk_addr;
    victim->direction = direction;
    victim->requestorId = pfi.getRequestorId();
    victim->secure = pfi.isSecure();
    victim->lastTouch = accessCounter;
    statsAmdStream.streamsCreated++;
    return *victim;
}

void
AMDContiguousStreamPrefetcher::generateStreamPrefetches(
    Addr blk_addr, int direction, std::vector<AddrPriority> &addresses)
{
    for (unsigned distance = 1; distance <= degree; ++distance) {
        Addr target = 0;
        if (!offsetAddress(blk_addr, direction, distance, target)) {
            return;
        }
        addresses.emplace_back(target, 0,
                               PrefetchSourceType::AMDContiguousStream);
        statsAmdStream.prefetchCandidates++;
    }
}

void
AMDContiguousStreamPrefetcher::notifyWithNewStreamControl(
    const CacheAccessProbeArg &acc, bool miss, bool allow_new_stream)
{
    const PacketPtr pkt = acc.pkt;
    CacheAccessor &cache = acc.cache;

    if (pkt->cmd.isSWPrefetch()) {
        return;
    }
    if (pkt->cmd.isHWPrefetch()) {
        return;
    }
    if (pkt->req->isCacheMaintenance()) {
        return;
    }
    if (pkt->isCleanEviction()) {
        return;
    }
    if (pkt->isWrite() && cache.coalesce()) {
        return;
    }
    if (!pkt->req->hasPaddr()) {
        panic("Request must have a physical address");
    }

    const bool has_been_prefetched =
        cache.hasBeenPrefetched(pkt->getAddr(), pkt->isSecure(), requestorId);
    if (has_been_prefetched) {
        usefulPrefetches += 1;
        prefetchStats.pfUseful++;
        PrefetchSourceType pf_source =
            cache.getHitBlkXsMetadata(pkt).prefetchSource;
        prefetchStats.pfUseful_srcs[pf_source]++;
        if (miss) {
            prefetchStats.pfUsefulButMiss++;
        }
    }

    if (!observeAccess(pkt, miss)) {
        return;
    }

    const bool old_allow_new_stream = allowNewStreamThisNotify;
    allowNewStreamThisNotify = allow_new_stream;
    const PrefetchSourceType trigger_source = !miss ?
        cache.getHitBlkXsMetadata(pkt).prefetchSource : pkt->getPFSource();
    const int trigger_depth = !miss ?
        cache.getHitBlkXsMetadata(pkt).prefetchDepth : pkt->getPFDepth();
    const Request::XsMetadata xs_metadata(trigger_source, trigger_depth);
    if (useVirtualAddresses && pkt->req->hasVaddr()) {
        PrefetchInfo pfi(pkt, pkt->req->getVaddr(), miss, xs_metadata);
        Queued::notify(pkt, pfi);
    } else if (!useVirtualAddresses) {
        PrefetchInfo pfi(pkt, pkt->req->getPaddr(), miss, xs_metadata);
        Queued::notify(pkt, pfi);
    }
    allowNewStreamThisNotify = old_allow_new_stream;
}


void
AMDContiguousStreamPrefetcher::notifyWithNewStreamControl(
    const PacketPtr &pkt, const PrefetchInfo &pfi, bool allow_new_stream)
{
    const bool old_allow_new_stream = allowNewStreamThisNotify;
    allowNewStreamThisNotify = allow_new_stream;
    Queued::notify(pkt, pfi);
    allowNewStreamThisNotify = old_allow_new_stream;
}

void
AMDContiguousStreamPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    accessCounter++;

    const Addr blk_addr = blockAddress(pfi.getAddr());

    for (auto &entry : streamTable) {
        if (!sameContext(entry, pfi)) {
            continue;
        }

        Addr expected = 0;
        if (!offsetAddress(entry.lastAddr, entry.direction, 1, expected)) {
            entry.valid = false;
            continue;
        }

        if (blk_addr != expected) {
            continue;
        }

        entry.lastAddr = blk_addr;
        entry.lastTouch = accessCounter;
        statsAmdStream.streamUpdates++;
        generateStreamPrefetches(blk_addr, entry.direction, addresses);
        return;
    }

    auto &last_access = findOrAllocateLastAccess(pfi);
    int direction = 0;
    const bool creates_new_stream =
        last_access.valid &&
        adjacentDirection(last_access.addr, blk_addr, direction);

    last_access.addr = blk_addr;
    last_access.lastTouch = accessCounter;

    if (!creates_new_stream) {
        return;
    }

    if (!allowNewStreamThisNotify) {
        statsAmdStream.newStreamsBlocked++;
        return;
    }

    allocateStream(pfi, blk_addr, direction);
    generateStreamPrefetches(blk_addr, direction, addresses);
}

AMDContiguousStreamPrefetcher::AMDContiguousStreamStats::
    AMDContiguousStreamStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(streamsCreated, statistics::units::Count::get(),
               "contiguous unit-stride streams created"),
      ADD_STAT(streamUpdates, statistics::units::Count::get(),
               "existing contiguous streams updated"),
      ADD_STAT(newStreamsBlocked, statistics::units::Count::get(),
               "new contiguous streams blocked by the region coordinator"),
      ADD_STAT(prefetchCandidates, statistics::units::Count::get(),
               "prefetch candidates emitted by contiguous streams")
{}

} // namespace prefetch
} // namespace gem5
