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

#ifndef __MEM_CACHE_PREFETCH_SANDBOX_MULTI_HH__
#define __MEM_CACHE_PREFETCH_SANDBOX_MULTI_HH__

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "base/circular_queue.hh"
#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct SandboxMultiPrefetchersParams;

namespace prefetch
{

class SandboxMulti : public Queued
{
  protected:
    using ShadowKey = uint64_t;

    struct ChildState
    {
        unsigned lastScore;
        bool active;

        ChildState() : lastScore(0), active(false) {}
    };

    struct SandboxStats : public statistics::Group
    {
        SandboxStats(SandboxMulti *parent);

        statistics::Scalar sandboxHits;
        statistics::Scalar sandboxInsertions;
        statistics::Scalar sandboxEvictions;
        statistics::Scalar evaluationWindows;
        statistics::Scalar activeWindows;
        statistics::Scalar childSelections;
        statistics::Scalar budgetRecomputations;
        statistics::Value currentPrefetchBudget;
    } statsSandbox;

    std::vector<Queued *> prefetchers;
    std::vector<ChildState> childStates;

    const unsigned sandboxEntries;
    const unsigned evaluationWindow;
    const unsigned scoreThresholdPct;
    const double bandwidthRequestsPerAccess;
    const unsigned minPrefetchesPerAccess;
    const unsigned maxPrefetchesPerAccess;
    const unsigned maxPrefetchesPerChild;
    const unsigned maxActivePrefetchers;

    CircularQueue<ShadowKey> sandboxQueue;
    std::unordered_set<ShadowKey> sandboxSet;

    size_t currentEvaluating;
    unsigned accessesInWindow;
    unsigned hitsInWindow;
    unsigned windowDemandReads;
    unsigned windowDemandWrites;
    unsigned windowPrefetchesIssued;
    unsigned currentPrefetchBudget;

    ShadowKey makeShadowKey(Addr addr, bool secure) const;
    void resetSandbox();
    void insertShadowCandidate(Addr addr, bool secure);
    void evaluateDemandHit(Addr addr, bool secure);
    void recomputePrefetchBudget();
    unsigned scoreThreshold() const;
    unsigned perChildIssueBudget(unsigned score) const;
    bool advanceEvaluationWindow();
    int32_t candidatePriority(unsigned score, size_t childRank,
                              size_t candidateRank) const;

  public:
    SandboxMulti(const SandboxMultiPrefetchersParams &p);

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                       unsigned blk_size) override;
    void notifyFill(const PacketPtr &pkt) override;
    void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) override;
    void incrDemandMhsrMisses() override;
    void rxHint(BaseMMU::Translation *dpp) override {}

    PacketPtr getPacket() override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_SANDBOX_MULTI_HH__
