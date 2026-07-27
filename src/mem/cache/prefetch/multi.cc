/*
 * Copyright (c) 2014, 2019 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "mem/cache/prefetch/multi.hh"

#include "mem/cache/prefetch/berti.hh"
#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/despacito_stream.hh"
#include "mem/cache/prefetch/ipcp.hh"
#include "mem/cache/prefetch/opt.hh"
#include "mem/cache/prefetch/xs_stream.hh"
#include "mem/cache/prefetch/xs_stride.hh"
#include "params/MultiPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

Multi::Multi(const MultiPrefetcherParams &p)
  : Queued(p),
    prefetchers(p.prefetchers.begin(), p.prefetchers.end()),
    childFilter(256),
    lastChosenPf(0)
{
}


void
Multi::bindChildFilter(Base *prefetcher)
{
    if (prefetcher == nullptr) {
        return;
    }
    if (auto *bop = dynamic_cast<BOP *>(prefetcher)) {
        bop->filter = &childFilter;
    }
    if (auto *stream = dynamic_cast<DespacitoStreamPrefetcher *>(prefetcher)) {
        stream->filter = &childFilter;
    }
    if (auto *cmc = dynamic_cast<CMCPrefetcher *>(prefetcher)) {
        cmc->filter = &childFilter;
    }
    if (auto *berti = dynamic_cast<BertiPrefetcher *>(prefetcher)) {
        berti->filter = &childFilter;
    }
    if (auto *sstride = dynamic_cast<XSStridePrefetcher *>(prefetcher)) {
        sstride->filter = &childFilter;
    }
    if (auto *opt = dynamic_cast<OptPrefetcher *>(prefetcher)) {
        opt->filter = &childFilter;
    }
    if (auto *xsstream = dynamic_cast<XsStreamPrefetcher *>(prefetcher)) {
        xsstream->filter = &childFilter;
    }
    if (auto *ipcp = dynamic_cast<IPCP *>(prefetcher)) {
        ipcp->rrf = &childFilter;
    }
}

void
Multi::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size)
{
    Queued::setParentInfo(sys, pm, _cache, blk_size);

    for (auto pf : prefetchers) {
        bindChildFilter(pf);
        pf->setParentInfo(sys, pm, _cache, blk_size);
    }
}

Tick
Multi::nextPrefetchReadyTime() const
{
    Tick next_ready = MaxTick;

    for (auto pf : prefetchers)
        next_ready = std::min(next_ready, pf->nextPrefetchReadyTime());

    return next_ready;
}

void
Multi::calculatePrefetch(const PrefetchInfo &pfi,
                         std::vector<AddrPriority> &addresses)
{
    calculatePrefetch(pfi, addresses, false, PrefetchSourceType::PF_NONE,
                      false);
}

void
Multi::calculatePrefetch(const PrefetchInfo &pfi,
                         std::vector<AddrPriority> &addresses, bool late,
                         PrefetchSourceType source, bool miss_repeat)
{
    for (auto *prefetcher : prefetchers) {
        auto *queued = dynamic_cast<Queued *>(prefetcher);
        fatal_if(!queued, "%s: child %s is not a Queued prefetcher",
                 name(), prefetcher->name());
        queued->calculatePrefetch(pfi, addresses, late, source, miss_repeat);
    }
}

bool
Multi::hasPendingPacket()
{
    uint8_t pf_turn = (lastChosenPf + 1) % prefetchers.size();
    return (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick());
}

PacketPtr
Multi::getPacket()
{
    lastChosenPf = (lastChosenPf + 1) % prefetchers.size();
    uint8_t pf_turn = lastChosenPf;

    for (int pf = 0 ;  pf < prefetchers.size(); pf++) {
        if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick()) {
            PacketPtr pkt = prefetchers[pf_turn]->getPacket();
            panic_if(!pkt, "Prefetcher is ready but didn't return a packet.");
            prefetchStats.pfIssued++;
            issuedPrefetches++;
            return pkt;
        }
        pf_turn = (pf_turn + 1) % prefetchers.size();
    }

    return nullptr;
}
void
Multi::addTLB(BaseTLB *_t, bool functional)
{
    Queued::addTLB(_t, functional);
    for (auto pf : prefetchers)
        pf->addTLB(_t, functional);
}

} // namespace prefetch
} // namespace gem5
