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

#ifndef __MEM_CACHE_PREFETCH_RESEMBLE_HH__
#define __MEM_CACHE_PREFETCH_RESEMBLE_HH__

#include <cstdint>
#include <deque>
#include <list>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct ReSemblePrefetcherParams;

namespace prefetch
{

class ReSemble : public Queued
{
  protected:
    enum class PredictionKind
    {
        Spatial,
        Temporal,
    };

    struct Transition
    {
        std::vector<double> state;
        std::vector<double> nextState;
        std::size_t action;
        Addr chosenAddr;
        bool hasChosenAddr;
        double reward;
        bool rewardResolved;
        bool hasNextState;
        uint64_t createdAt;
    };

    struct Network
    {
        std::vector<double> inputHiddenWeights;
        std::vector<double> hiddenBiases;
        std::vector<double> hiddenOutputWeights;
        std::vector<double> outputBiases;
    };

    using CandidateList = std::vector<std::optional<AddrPriority>>;

    std::vector<Queued *> prefetchers;
    std::vector<PredictionKind> predictionKinds;

    const unsigned hiddenDim;
    const unsigned hashBits;
    const double alpha;
    const double gamma;
    const double epsilonStart;
    const double epsilonEnd;
    const double epsilonDecay;
    const unsigned rewardWindow;
    const unsigned replayCapacity;
    const unsigned batchSize;
    const unsigned policyUpdateInterval;
    const unsigned targetUpdateInterval;

    uint64_t accessCount;
    std::list<Transition> pendingTransitions;
    std::deque<Transition> replayMemory;
    Network policyNet;
    Network targetNet;
    std::mt19937_64 rng;

    struct ReSembleStats : public statistics::Group
    {
        ReSembleStats(statistics::Group *parent);
        statistics::Scalar actionsSelected;
        statistics::Scalar prefetchActions;
        statistics::Scalar noPrefetchActions;
        statistics::Scalar validTransitions;
        statistics::Scalar rewardHits;
        statistics::Scalar rewardExpirations;
        statistics::Scalar trainingUpdates;
        statistics::Scalar targetUpdates;
    } statsReSemble;

    CandidateList collectCandidates(const PrefetchInfo &pfi);
    std::vector<double> buildState(const PrefetchInfo &pfi,
                                   const CandidateList &candidates) const;
    double encodeCandidate(const PrefetchInfo &pfi,
                           const AddrPriority &candidate,
                           PredictionKind kind) const;
    uint64_t hashAddress(Addr addr) const;
    double currentEpsilon() const;
    std::size_t selectAction(const std::vector<double> &state,
                             const CandidateList &candidates);
    void updateTransitions(const std::vector<double> &state, Addr demandAddr);
    void queueTransition(const std::vector<double> &state, std::size_t action,
                         const std::optional<AddrPriority> &candidate);
    void maybeTrain();
    void trainSample(const Transition &transition);
    void initializeNetwork(Network &network);
    void syncTargetNetwork();
    std::vector<double>
    forward(const Network &network, const std::vector<double> &state,
            std::vector<double> *hidden = nullptr,
            std::vector<double> *hiddenPreActivation = nullptr) const;

  public:
    ReSemble(const ReSemblePrefetcherParams &p);
    ~ReSemble() = default;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                       unsigned blk_size) override;
    void rxHint(BaseMMU::Translation *dpp) override {}
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_RESEMBLE_HH__
