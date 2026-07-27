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

#include "mem/cache/prefetch/sandbox_multi.hh"

#include <algorithm>
#include <cmath>

#include "base/logging.hh"
#include "mem/cache/prefetch/bop.hh"
#include "params/SandboxMultiPrefetchers.hh"

namespace gem5
{

namespace prefetch
{

namespace
{

void
calculateChildPrefetch(Queued *pf, const Base::PrefetchInfo &pfi,
                       std::vector<Queued::AddrPriority> &candidates)
{
    if (auto *bop = dynamic_cast<BOP *>(pf)) {
        bop->calculatePrefetch(pfi, candidates, false);
        return;
    }

    pf->calculatePrefetch(pfi, candidates);
}

} // anonymous namespace

SandboxMulti::SandboxStats::SandboxStats(SandboxMulti *parent)
    : statistics::Group(parent),
      ADD_STAT(sandboxHits, statistics::units::Count::get(),
               "number of demand accesses found in the shadow sandbox"),
      ADD_STAT(
          sandboxInsertions, statistics::units::Count::get(),
          "number of shadow prefetch candidates inserted into the sandbox"),
      ADD_STAT(sandboxEvictions, statistics::units::Count::get(),
               "number of shadow sandbox entries evicted due to capacity"),
      ADD_STAT(evaluationWindows, statistics::units::Count::get(),
               "number of completed child evaluation windows"),
      ADD_STAT(
          activeWindows, statistics::units::Count::get(),
          "number of windows whose child passed the activation threshold"),
      ADD_STAT(childSelections, statistics::units::Count::get(),
               "number of active child selections for real issue"),
      ADD_STAT(budgetRecomputations, statistics::units::Count::get(),
               "number of times the dynamic prefetch budget was recomputed"),
      ADD_STAT(currentPrefetchBudget, statistics::units::Count::get(),
               "current bandwidth-derived prefetch budget per access")
{
    currentPrefetchBudget.functor(
        [parent]() { return parent->currentPrefetchBudget; });
}

SandboxMulti::SandboxMulti(const SandboxMultiPrefetchersParams &p)
    : Queued(p),
      statsSandbox(this),
      prefetchers(),
      childStates(p.prefetchers.size()),
      sandboxEntries(p.sandbox_entries),
      evaluationWindow(p.evaluation_window),
      scoreThresholdPct(p.score_threshold_pct),
      bandwidthRequestsPerAccess(p.bandwidth_requests_per_access),
      minPrefetchesPerAccess(p.min_prefetches_per_access),
      maxPrefetchesPerAccess(p.max_prefetches_per_access),
      maxPrefetchesPerChild(p.max_prefetches_per_child),
      maxActivePrefetchers(p.max_active_prefetchers),
      sandboxQueue(p.sandbox_entries),
      sandboxSet(),
      currentEvaluating(0),
      accessesInWindow(0),
      hitsInWindow(0),
      windowDemandReads(0),
      windowDemandWrites(0),
      windowPrefetchesIssued(0),
      currentPrefetchBudget(p.max_prefetches_per_access)
{
    fatal_if(p.prefetchers.empty(),
             "%s: SandboxMultiPrefetchers requires at least one child",
             name());

    prefetchers.reserve(p.prefetchers.size());
    for (auto *pf : p.prefetchers) {
        auto *queued_pf = dynamic_cast<Queued *>(pf);
        fatal_if(!queued_pf,
                 "%s: SandboxMultiPrefetchers only supports "
                 "QueuedPrefetcher children",
                 name());
        prefetchers.push_back(queued_pf);
    }
}

void
SandboxMulti::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                            unsigned blk_size)
{
    Queued::setParentInfo(sys, pm, _cache, blk_size);
    for (auto *pf : prefetchers) {
        pf->setParentInfo(sys, nullptr, _cache, blk_size);
    }
}

void
SandboxMulti::notifyFill(const PacketPtr &pkt)
{
    for (auto *pf : prefetchers) {
        pf->notifyFill(pkt);
    }
}

void
SandboxMulti::prefetchUnused(Addr paddr, PrefetchSourceType pfSource)
{
    Base::prefetchUnused(paddr, pfSource);
    for (auto *pf : prefetchers) {
        pf->prefetchUnused(paddr, pfSource);
    }
}

void
SandboxMulti::incrDemandMhsrMisses()
{
    Base::incrDemandMhsrMisses();
    for (auto *pf : prefetchers) {
        pf->incrDemandMhsrMisses();
    }
}

SandboxMulti::ShadowKey
SandboxMulti::makeShadowKey(Addr addr, bool secure) const
{
    return (static_cast<ShadowKey>(blockAddress(addr)) << 1) |
           static_cast<ShadowKey>(secure ? 1 : 0);
}

void
SandboxMulti::resetSandbox()
{
    sandboxQueue.flush();
    sandboxSet.clear();
    accessesInWindow = 0;
    hitsInWindow = 0;
    windowDemandReads = 0;
    windowDemandWrites = 0;
    windowPrefetchesIssued = 0;
}

void
SandboxMulti::insertShadowCandidate(Addr addr, bool secure)
{
    const auto key = makeShadowKey(addr, secure);
    if (sandboxSet.find(key) != sandboxSet.end()) {
        return;
    }

    if (sandboxQueue.full()) {
        sandboxSet.erase(sandboxQueue.front());
        sandboxQueue.pop_front();
        statsSandbox.sandboxEvictions++;
    }

    sandboxQueue.push_back(key);
    sandboxSet.insert(key);
    statsSandbox.sandboxInsertions++;
}

void
SandboxMulti::recomputePrefetchBudget()
{
    if (accessesInWindow == 0) {
        currentPrefetchBudget = minPrefetchesPerAccess;
        return;
    }

    const auto totalBudget = static_cast<unsigned>(std::ceil(
        static_cast<double>(accessesInWindow) * bandwidthRequestsPerAccess));
    const auto usedBudget =
        windowDemandReads + windowDemandWrites + windowPrefetchesIssued;
    const auto unusedBudget =
        (usedBudget < totalBudget) ? (totalBudget - usedBudget) : 0;
    const auto nextBudget = static_cast<unsigned>(
        std::ceil(static_cast<double>(unusedBudget) / accessesInWindow));

    currentPrefetchBudget = std::min(
        maxPrefetchesPerAccess, std::max(minPrefetchesPerAccess, nextBudget));
    statsSandbox.budgetRecomputations++;
}

void
SandboxMulti::evaluateDemandHit(Addr addr, bool secure)
{
    if (sandboxSet.find(makeShadowKey(addr, secure)) != sandboxSet.end()) {
        hitsInWindow++;
        statsSandbox.sandboxHits++;
    }
}

unsigned
SandboxMulti::scoreThreshold() const
{ return std::max(1u, (evaluationWindow * scoreThresholdPct) / 100); }

unsigned
SandboxMulti::perChildIssueBudget(unsigned score) const
{
    const unsigned threshold = scoreThreshold();
    if (score < threshold) {
        return 0;
    }
    return std::min(maxPrefetchesPerChild, score / threshold);
}

bool
SandboxMulti::advanceEvaluationWindow()
{
    accessesInWindow++;
    if (accessesInWindow < evaluationWindow) {
        return false;
    }

    auto &state = childStates[currentEvaluating];
    state.lastScore = hitsInWindow;
    state.active = hitsInWindow >= scoreThreshold();

    statsSandbox.evaluationWindows++;
    if (state.active) {
        statsSandbox.activeWindows++;
    }

    recomputePrefetchBudget();
    currentEvaluating = (currentEvaluating + 1) % prefetchers.size();
    resetSandbox();
    return true;
}

int32_t
SandboxMulti::candidatePriority(unsigned score, size_t childRank,
                                size_t candidateRank) const
{
    const auto rank_bias = static_cast<int32_t>(
        childRank * maxPrefetchesPerChild + candidateRank);
    return static_cast<int32_t>(score * 1024) - rank_bias;
}

PacketPtr
SandboxMulti::getPacket()
{
    PacketPtr pkt = Queued::getPacket();
    if (pkt != nullptr) {
        windowPrefetchesIssued++;
    }
    return pkt;
}

void
SandboxMulti::calculatePrefetch(const PrefetchInfo &pfi,
                                std::vector<AddrPriority> &addresses)
{
    std::vector<std::vector<AddrPriority>> candidates(prefetchers.size());
    for (size_t idx = 0; idx < prefetchers.size(); ++idx) {
        calculateChildPrefetch(prefetchers[idx], pfi, candidates[idx]);
    }

    if (pfi.isWrite()) {
        windowDemandWrites++;
    } else {
        windowDemandReads++;
    }

    evaluateDemandHit(pfi.getAddr(), pfi.isSecure());
    for (const auto &candidate : candidates[currentEvaluating]) {
        insertShadowCandidate(candidate.addr, pfi.isSecure());
    }
    const unsigned budgetForThisAccess = currentPrefetchBudget;
    advanceEvaluationWindow();

    std::vector<size_t> active_children;
    for (size_t idx = 0; idx < childStates.size(); ++idx) {
        if (childStates[idx].active) {
            active_children.push_back(idx);
        }
    }

    std::stable_sort(
        active_children.begin(), active_children.end(),
        [this](size_t lhs, size_t rhs) {
            if (childStates[lhs].lastScore != childStates[rhs].lastScore) {
                return childStates[lhs].lastScore > childStates[rhs].lastScore;
            }
            return lhs < rhs;
        });

    if (active_children.size() > maxActivePrefetchers) {
        active_children.resize(maxActivePrefetchers);
    }

    unsigned remaining = budgetForThisAccess;
    for (size_t child_rank = 0;
         child_rank < active_children.size() && remaining > 0; ++child_rank) {
        const auto idx = active_children[child_rank];
        const auto budget = perChildIssueBudget(childStates[idx].lastScore);
        if (budget == 0) {
            continue;
        }

        unsigned issued = 0;
        for (size_t candidate_rank = 0;
             candidate_rank < candidates[idx].size() && issued < budget &&
             remaining > 0;
             ++candidate_rank) {
            const auto &candidate = candidates[idx][candidate_rank];
            addresses.emplace_back(
                candidate.addr,
                candidatePriority(childStates[idx].lastScore, child_rank,
                                  candidate_rank),
                candidate.pfSource);
            issued++;
            remaining--;
        }

        if (issued > 0) {
            statsSandbox.childSelections++;
        }
    }
}

} // namespace prefetch
} // namespace gem5
