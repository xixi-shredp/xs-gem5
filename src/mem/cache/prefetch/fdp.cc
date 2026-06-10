/*
 * Copyright (c) 2022-2023 The University of Edinburgh
 * Copyright (c) 2025 Arm Limited
 * Copyright (c) 2026
 * All rights reserved
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

#include "mem/cache/prefetch/fdp.hh"

#include <algorithm>

#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/FetchDirectedPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

FetchDirectedPrefetcher::FetchDirectedPrefetcher(
    const FetchDirectedPrefetcherParams &p)
    : Base(p),
      cpu(p.cpu),
      markReqAsPrefetch(p.mark_req_as_prefetch),
      squashPrefetches(p.squash_prefetches),
      latency(cyclesToTicks(p.latency)),
      pfqSize(p.pfq_size),
      tqSize(p.tq_size),
      cacheSnoop(p.cache_snoop),
      stats(this, p.pfq_size, p.tq_size)
{}

FetchDirectedPrefetcher::~FetchDirectedPrefetcher()
{
    for (auto listener : fdpListeners) {
        delete listener;
    }
    for (auto &entry : pfq) {
        delete entry.pkt;
        entry.pkt = nullptr;
    }
}

FetchDirectedPrefetcher::FetchRequestListener::FetchRequestListener(
    FetchDirectedPrefetcher &_parent, ProbeManager *pm, const std::string &name)
    : ProbeListenerArgBase(pm, name), parent(_parent)
{}

void
FetchDirectedPrefetcher::FetchRequestListener::notify(const RequestPtr &req)
{
    parent.notifyFetchRequest(req);
}

void
FetchDirectedPrefetcher::notifyFetchRequest(const RequestPtr &req)
{
    if (req == nullptr || req->isPrefetch()) {
        return;
    }
    if (!req->hasVaddr() || !req->hasPaddr() || !req->hasContextId()) {
        return;
    }

    stats.fdipInsertions++;

    const Addr access_vblk = blockAddress(req->getVaddr());
    const Addr next_vblk = access_vblk + blkSize;
    if (!samePage(access_vblk, next_vblk)) {
        return;
    }

    const Addr access_pblk = blockAddress(req->getPaddr());
    const Addr next_pblk = access_pblk + blkSize;
    insertCandidate(next_vblk, next_pblk, req->contextId(), req->isSecure());
}

void
FetchDirectedPrefetcher::insertCandidate(Addr vaddr, Addr paddr,
                                         ContextID context_id, bool secure)
{
    if (std::find(pfq.begin(), pfq.end(), vaddr) != pfq.end()) {
        DPRINTF(HWPrefetch, "FDP %#x already in prefetch queue\n", vaddr);
        stats.pfInPFQ++;
        return;
    }

    stats.pfIdentified++;

    warn_if_once(cacheSnoop && cache == nullptr,
                 "FetchDirectedPrefetcher has no cache accessor. Cache "
                 "snooping is disabled.\n");

    if (!system->isMemAddr(paddr)) {
        DPRINTF(HWPrefetch, "FDP drop non-memory paddr %#x\n", paddr);
        return;
    }
    if (cacheSnoop && cache &&
        (inCache(paddr, secure) || inMissQueue(paddr, secure))) {
        DPRINTF(HWPrefetch, "FDP drop cache/MSHR resident %#x\n", paddr);
        stats.pfInCache++;
        return;
    }
    if (pfq.size() >= pfqSize) {
        DPRINTF(HWPrefetch, "FDP prefetch queue full, dropping %#x\n",
                vaddr);
        stats.pfqDrops++;
        return;
    }

    pfq.emplace_back(*this, vaddr, context_id, secure);
    pfq.back().createPkt(paddr);
    pfq.back().readyTime = curTick() + latency;
    stats.pfPacketsCreated++;
    stats.pfCandidatesAdded++;
    stats.pfqInserts++;
    stats.pfqSizeDistAtNotify.sample(pfq.size());
}

void
FetchDirectedPrefetcher::translationComplete(PrefetchRequest *pfr, bool failed)
{
    auto it = translationq.begin();
    while (it != translationq.end() && &(*it) != pfr) {
        ++it;
    }
    assert(it != translationq.end());

    warn_if_once(cacheSnoop && cache == nullptr,
                 "FetchDirectedPrefetcher has no cache accessor. Cache "
                 "snooping is disabled.\n");

    if (failed) {
        DPRINTF(HWPrefetch, "FDP translation of %#x failed\n", it->addr);
        stats.translationFail++;
    } else {
        DPRINTF(HWPrefetch, "FDP translation of %#x succeeded\n", it->addr);
        stats.translationSuccess++;

        if (it->isCanceled()) {
            DPRINTF(HWPrefetch, "FDP drop canceled request %#x\n", it->addr);
            stats.pfSquashed++;
        } else if (it->req->isUncacheable()) {
            DPRINTF(HWPrefetch, "FDP drop uncacheable request %#x\n",
                    it->addr);
        } else if (!system->isMemAddr(it->req->getPaddr())) {
            DPRINTF(HWPrefetch, "FDP drop non-memory paddr %#x\n",
                    it->req->getPaddr());
        } else if (cacheSnoop && cache &&
                   (inCache(it->req->getPaddr(), it->secure) ||
                    inMissQueue(it->req->getPaddr(), it->secure))) {
            DPRINTF(HWPrefetch, "FDP drop cache/MSHR resident %#x\n",
                    it->req->getPaddr());
            stats.pfInCache++;
        } else if (pfq.size() >= pfqSize) {
            DPRINTF(HWPrefetch, "FDP prefetch queue full, dropping %#x\n",
                    it->addr);
            stats.pfqDrops++;
        } else {
            it->createPkt(it->req->getPaddr());
            it->readyTime = curTick() + latency;
            pfq.push_back(*it);
            stats.pfPacketsCreated++;
            stats.pfCandidatesAdded++;
            stats.pfqInserts++;
        }
    }

    translationq.erase(it);
    stats.tqPops++;
}

PacketPtr
FetchDirectedPrefetcher::getPacket()
{
    if (pfq.empty() || pfq.front().readyTime > curTick()) {
        return nullptr;
    }

    PacketPtr pkt = pfq.front().pkt;
    pfq.front().pkt = nullptr;
    DPRINTF(HWPrefetch, "FDP issue prefetch paddr:%#x vaddr:%#x\n",
            pkt->getAddr(), pfq.front().addr);
    pfq.pop_front();

    stats.pfqPops++;
    prefetchStats.pfIssued++;
    prefetchStats.pfIssued_srcs[PrefetchSourceType::FetchDirected]++;
    return pkt;
}

FetchDirectedPrefetcher::PrefetchRequest::PrefetchRequest(
    FetchDirectedPrefetcher &_owner, Addr _addr, ContextID _context_id,
    bool _secure)
    : owner(_owner),
      addr(_addr),
      contextId(_context_id),
      secure(_secure),
      req(nullptr),
      pkt(nullptr),
      readyTime(MaxTick),
      canceled(false)
{
    req = std::make_shared<Request>(addr, owner.blkSize, Request::INST_FETCH,
                                    owner.requestorId, addr, contextId);
    if (secure) {
        req->setFlags(Request::SECURE);
    }
    if (owner.markReqAsPrefetch) {
        req->setFlags(Request::PREFETCH);
    }
    req->setPFSource(PrefetchSourceType::FetchDirected);
    req->setPFDepth(1);
    req->setXsMetadata(Request::XsMetadata(PrefetchSourceType::FetchDirected,
                                           1));
}

void
FetchDirectedPrefetcher::PrefetchRequest::createPkt(Addr paddr)
{
    req->setPaddr(paddr);
    req->taskId(context_switch_task_id::Prefetcher);
    pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
}

void
FetchDirectedPrefetcher::PrefetchRequest::startTranslation()
{
    if (owner.tlb == nullptr) {
        warn_once("FetchDirectedPrefetcher has no TLB. Dropping requests.\n");
        owner.translationComplete(this, true);
        return;
    }

    auto tc = owner.system->threads[req->contextId()];
    if (owner.functionalTLB) {
        owner.tlb->translateFunctional(req, tc, this, BaseMMU::Execute);
    } else {
        owner.tlb->translateTiming(req, tc, this, BaseMMU::Execute);
    }
}

void
FetchDirectedPrefetcher::PrefetchRequest::finish(const Fault &fault,
                                                 const RequestPtr &req,
                                                 ThreadContext *tc,
                                                 BaseMMU::Mode mode)
{
    owner.translationComplete(this, fault != NoFault);
}

void
FetchDirectedPrefetcher::regProbeListeners()
{
    Base::regProbeListeners();

    if (cpu == nullptr) {
        warn("FetchDirectedPrefetcher: no CPU to listen from registered\n");
        return;
    }

    fdpListeners.push_back(new FetchRequestListener(
        *this, cpu->getProbeManager(), "FetchRequest"));
}

FetchDirectedPrefetcher::Stats::Stats(statistics::Group *parent, int pfq_size,
                                      int tq_size)
    : statistics::Group(parent),
      ADD_STAT(fdipInsertions, statistics::units::Count::get(),
               "Number of fetch request notifications seen by FDP"),
      ADD_STAT(pfIdentified, statistics::units::Count::get(),
               "Number of FDP prefetch candidates identified"),
      ADD_STAT(pfSquashed, statistics::units::Count::get(),
               "Number of FDP prefetches squashed"),
      ADD_STAT(pfInPFQ, statistics::units::Count::get(),
               "Number of FDP candidates already in the prefetch queue"),
      ADD_STAT(pfInTQ, statistics::units::Count::get(),
               "Number of FDP candidates already in the translation queue"),
      ADD_STAT(pfInCache, statistics::units::Count::get(),
               "Number of FDP candidates filtered by cache or MSHR snoop"),
      ADD_STAT(pfPacketsCreated, statistics::units::Count::get(),
               "Number of FDP prefetch packets created"),
      ADD_STAT(pfCandidatesAdded, statistics::units::Count::get(),
               "Number of FDP candidates added to the prefetch queue"),
      ADD_STAT(translationFail, statistics::units::Count::get(),
               "Number of FDP translations that failed"),
      ADD_STAT(translationSuccess, statistics::units::Count::get(),
               "Number of FDP translations that succeeded"),
      ADD_STAT(pfqSizeDistAtNotify, statistics::units::Count::get(),
               "Distribution of FDP prefetch queue occupancy at notify"),
      ADD_STAT(tqSizeDistAtNotify, statistics::units::Count::get(),
               "Distribution of FDP translation queue occupancy at notify"),
      ADD_STAT(pfqInserts, statistics::units::Count::get(),
               "Number of FDP prefetch queue insertions"),
      ADD_STAT(pfqPops, statistics::units::Count::get(),
               "Number of FDP prefetch queue pops"),
      ADD_STAT(pfqDrops, statistics::units::Count::get(),
               "Number of FDP prefetch queue drops"),
      ADD_STAT(tqInserts, statistics::units::Count::get(),
               "Number of FDP translation queue insertions"),
      ADD_STAT(tqPops, statistics::units::Count::get(),
               "Number of FDP translation queue pops"),
      ADD_STAT(tqDrops, statistics::units::Count::get(),
               "Number of FDP translation queue drops")
{
    pfqSizeDistAtNotify.init(0, pfq_size, 4);
    tqSizeDistAtNotify.init(0, tq_size, 4);
}

} // namespace prefetch
} // namespace gem5
