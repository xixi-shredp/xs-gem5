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

#include "mem/cache/prefetch/resemble.hh"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/logging.hh"
#include "mem/cache/prefetch/bop.hh"
#include "params/ReSemblePrefetcher.hh"

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

ReSemble::ReSemble(const ReSemblePrefetcherParams &p)
    : Queued(p),
      hiddenDim(p.hidden_dim),
      hashBits(p.hash_bits),
      alpha(p.alpha),
      gamma(p.gamma),
      epsilonStart(p.epsilon_start),
      epsilonEnd(p.epsilon_end),
      epsilonDecay(p.epsilon_decay),
      rewardWindow(p.reward_window),
      replayCapacity(p.replay_capacity),
      batchSize(p.batch_size),
      policyUpdateInterval(p.policy_update_interval),
      targetUpdateInterval(p.target_update_interval),
      accessCount(0),
      rng(p.seed),
      statsReSemble(this)
{
    fatal_if(p.prefetchers.empty(),
             "%s: ReSemblePrefetcher requires at least one child", name());
    fatal_if(p.prefetchers.size() != p.prediction_types.size(),
             "%s: prefetchers and prediction_types must have the same size",
             name());

    prefetchers.reserve(p.prefetchers.size());
    for (auto *pf : p.prefetchers) {
        auto *queued_pf = dynamic_cast<Queued *>(pf);
        fatal_if(!queued_pf,
                 "%s: ReSemblePrefetcher only supports Queued children",
                 name());
        prefetchers.push_back(queued_pf);
    }

    predictionKinds.reserve(p.prediction_types.size());
    for (const auto &predictionType : p.prediction_types) {
        fatal_if(predictionType != "spatial" && predictionType != "temporal",
                 "%s: unsupported prediction type %s", name(),
                 predictionType);
        predictionKinds.push_back(predictionType == "spatial"
                                      ? PredictionKind::Spatial
                                      : PredictionKind::Temporal);
    }

    initializeNetwork(policyNet);
    syncTargetNetwork();
}

void
ReSemble::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                         unsigned blk_size)
{
    Queued::setParentInfo(sys, pm, _cache, blk_size);

    for (auto *prefetcher : prefetchers) {
        prefetcher->setParentInfo(sys, nullptr, _cache, blk_size);
    }
}

void
ReSemble::calculatePrefetch(const PrefetchInfo &pfi,
                            std::vector<AddrPriority> &addresses)
{
    accessCount++;

    const auto candidates = collectCandidates(pfi);
    const auto state = buildState(pfi, candidates);
    updateTransitions(state, pfi.getAddr());
    maybeTrain();

    statsReSemble.actionsSelected++;
    const auto action = selectAction(state, candidates);
    const auto npAction = prefetchers.size();
    if (action == npAction || !candidates[action].has_value()) {
        statsReSemble.noPrefetchActions++;
        queueTransition(state, action, std::nullopt);
        return;
    }

    statsReSemble.prefetchActions++;
    addresses.push_back(*candidates[action]);
    queueTransition(state, action, candidates[action]);
}

ReSemble::ReSembleStats::ReSembleStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(actionsSelected, statistics::units::Count::get(),
               "number of ReSemble actions selected"),
      ADD_STAT(prefetchActions, statistics::units::Count::get(),
               "number of actions selecting a child prefetcher"),
      ADD_STAT(noPrefetchActions, statistics::units::Count::get(),
               "number of actions selecting NP"),
      ADD_STAT(validTransitions, statistics::units::Count::get(),
               "number of transitions admitted to replay memory"),
      ADD_STAT(rewardHits, statistics::units::Count::get(),
               "number of transitions rewarded by a demand hit"),
      ADD_STAT(rewardExpirations, statistics::units::Count::get(),
               "number of transitions penalized after reward expiry"),
      ADD_STAT(trainingUpdates, statistics::units::Count::get(),
               "number of online controller updates"),
      ADD_STAT(targetUpdates, statistics::units::Count::get(),
               "number of target-network refreshes")
{}

ReSemble::CandidateList
ReSemble::collectCandidates(const PrefetchInfo &pfi)
{
    CandidateList candidates(prefetchers.size());
    for (std::size_t idx = 0; idx < prefetchers.size(); ++idx) {
        std::vector<AddrPriority> childCandidates;
        calculateChildPrefetch(prefetchers[idx], pfi, childCandidates);
        if (childCandidates.empty()) {
            continue;
        }

        const auto bestCandidate = std::max_element(
            childCandidates.begin(), childCandidates.end(),
            [](const AddrPriority &lhs, const AddrPriority &rhs) {
                return lhs.priority < rhs.priority;
            });
        candidates[idx] = *bestCandidate;
    }
    return candidates;
}

std::vector<double>
ReSemble::buildState(const PrefetchInfo &pfi,
                     const CandidateList &candidates) const
{
    std::vector<double> state(prefetchers.size(), 0.0);
    for (std::size_t idx = 0; idx < candidates.size(); ++idx) {
        if (!candidates[idx].has_value()) {
            continue;
        }
        state[idx] =
            encodeCandidate(pfi, *candidates[idx], predictionKinds[idx]);
    }
    return state;
}

double
ReSemble::encodeCandidate(const PrefetchInfo &pfi,
                          const AddrPriority &candidate,
                          PredictionKind kind) const
{
    const Addr candidateBlock = blockAddress(candidate.addr);
    const Addr demandBlock = blockAddress(pfi.getAddr());
    if (kind == PredictionKind::Spatial) {
        const auto delta = static_cast<int64_t>(candidateBlock) -
                           static_cast<int64_t>(demandBlock);
        return static_cast<double>(delta) / static_cast<double>(pageBytes);
    }

    const auto bits = std::min(hashBits, 63u);
    const uint64_t mask = (bits == 63)
                              ? (std::numeric_limits<uint64_t>::max() >> 1)
                              : ((1ULL << bits) - 1);
    const double denominator = (mask == 0) ? 1.0 : static_cast<double>(mask);
    return static_cast<double>(hashAddress(candidateBlock)) / denominator;
}

uint64_t
ReSemble::hashAddress(Addr addr) const
{
    const auto bits = std::min(hashBits, 63u);
    const uint64_t mask = (bits == 63)
                              ? (std::numeric_limits<uint64_t>::max() >> 1)
                              : ((1ULL << bits) - 1);

    uint64_t value = static_cast<uint64_t>(addr);
    uint64_t folded = 0;
    while (value != 0) {
        folded ^= (value & mask);
        value >>= bits;
    }
    return folded & mask;
}

double
ReSemble::currentEpsilon() const
{
    return epsilonEnd +
           (epsilonStart - epsilonEnd) *
               std::pow(epsilonDecay, static_cast<double>(accessCount));
}

std::size_t
ReSemble::selectAction(const std::vector<double> &state,
                       const CandidateList &candidates)
{
    std::vector<std::size_t> availableActions;
    for (std::size_t idx = 0; idx < candidates.size(); ++idx) {
        if (candidates[idx].has_value()) {
            availableActions.push_back(idx);
        }
    }
    availableActions.push_back(prefetchers.size());

    std::uniform_real_distribution<double> unitDistribution(0.0, 1.0);
    if (availableActions.size() > 1 &&
        unitDistribution(rng) < currentEpsilon()) {
        std::uniform_int_distribution<std::size_t> actionDistribution(
            0, availableActions.size() - 1);
        return availableActions[actionDistribution(rng)];
    }

    const auto qValues = forward(targetNet, state);
    auto bestAction = availableActions.front();
    double bestValue = qValues[bestAction];
    for (const auto action : availableActions) {
        if (qValues[action] > bestValue) {
            bestAction = action;
            bestValue = qValues[action];
        }
    }
    return bestAction;
}

void
ReSemble::updateTransitions(const std::vector<double> &state, Addr demandAddr)
{
    for (auto rit = pendingTransitions.rbegin();
         rit != pendingTransitions.rend(); ++rit) {
        if (!rit->hasNextState) {
            rit->nextState = state;
            rit->hasNextState = true;
            break;
        }
    }

    const Addr demandBlock = blockAddress(demandAddr);
    for (auto &transition : pendingTransitions) {
        if (transition.rewardResolved) {
            continue;
        }

        if (transition.hasChosenAddr &&
            blockAddress(transition.chosenAddr) == demandBlock) {
            transition.reward = 1.0;
            transition.rewardResolved = true;
            statsReSemble.rewardHits++;
        } else if ((accessCount - transition.createdAt) >= rewardWindow) {
            transition.reward = -1.0;
            transition.rewardResolved = true;
            statsReSemble.rewardExpirations++;
        }
    }

    for (auto it = pendingTransitions.begin();
         it != pendingTransitions.end();) {
        if (it->rewardResolved && it->hasNextState) {
            if (replayMemory.size() == replayCapacity) {
                replayMemory.pop_front();
            }
            replayMemory.push_back(*it);
            statsReSemble.validTransitions++;
            it = pendingTransitions.erase(it);
        } else {
            ++it;
        }
    }
}

void
ReSemble::queueTransition(const std::vector<double> &state, std::size_t action,
                          const std::optional<AddrPriority> &candidate)
{
    Transition transition{
        state, {}, action, 0, false, 0.0, false, false, accessCount,
    };

    if (candidate.has_value()) {
        transition.chosenAddr = blockAddress(candidate->addr);
        transition.hasChosenAddr = true;
    } else {
        transition.rewardResolved = true;
    }

    pendingTransitions.push_back(std::move(transition));
}

void
ReSemble::maybeTrain()
{
    if (replayMemory.empty() || (accessCount % policyUpdateInterval) != 0) {
        return;
    }

    const auto sampleCount =
        std::min<std::size_t>(batchSize, replayMemory.size());
    std::uniform_int_distribution<std::size_t> sampleDistribution(
        0, replayMemory.size() - 1);
    for (std::size_t sample = 0; sample < sampleCount; ++sample) {
        trainSample(replayMemory[sampleDistribution(rng)]);
    }
    statsReSemble.trainingUpdates++;

    if ((accessCount % targetUpdateInterval) == 0) {
        syncTargetNetwork();
        statsReSemble.targetUpdates++;
    }
}

void
ReSemble::trainSample(const Transition &transition)
{
    std::vector<double> hidden;
    std::vector<double> hiddenPreActivation;
    const auto qValues =
        forward(policyNet, transition.state, &hidden, &hiddenPreActivation);
    const auto nextQValues = forward(targetNet, transition.nextState);

    const double targetValue =
        transition.reward +
        gamma * *std::max_element(nextQValues.begin(), nextQValues.end());
    const double error = qValues[transition.action] - targetValue;

    const auto inputDim = prefetchers.size();
    std::vector<double> oldOutputWeights(hiddenDim, 0.0);
    for (unsigned hiddenIdx = 0; hiddenIdx < hiddenDim; ++hiddenIdx) {
        const auto weightIndex = transition.action * hiddenDim + hiddenIdx;
        oldOutputWeights[hiddenIdx] =
            policyNet.hiddenOutputWeights[weightIndex];
        policyNet.hiddenOutputWeights[weightIndex] -=
            alpha * error * hidden[hiddenIdx];
    }
    policyNet.outputBiases[transition.action] -= alpha * error;

    for (unsigned hiddenIdx = 0; hiddenIdx < hiddenDim; ++hiddenIdx) {
        if (hiddenPreActivation[hiddenIdx] <= 0.0) {
            continue;
        }

        const double hiddenGradient = error * oldOutputWeights[hiddenIdx];
        const auto baseIndex = hiddenIdx * inputDim;
        for (std::size_t inputIdx = 0; inputIdx < inputDim; ++inputIdx) {
            policyNet.inputHiddenWeights[baseIndex + inputIdx] -=
                alpha * hiddenGradient * transition.state[inputIdx];
        }
        policyNet.hiddenBiases[hiddenIdx] -= alpha * hiddenGradient;
    }
}

void
ReSemble::initializeNetwork(Network &network)
{
    const auto inputDim = prefetchers.size();
    const auto outputDim = prefetchers.size() + 1;
    std::uniform_real_distribution<double> initDistribution(-0.05, 0.05);

    network.inputHiddenWeights.resize(inputDim * hiddenDim);
    network.hiddenBiases.resize(hiddenDim, 0.01);
    network.hiddenOutputWeights.resize(hiddenDim * outputDim);
    network.outputBiases.resize(outputDim, 0.01);
    network.outputBiases.back() = 0.0;

    for (auto &weight : network.inputHiddenWeights) {
        weight = initDistribution(rng);
    }
    for (auto &weight : network.hiddenOutputWeights) {
        weight = initDistribution(rng);
    }
}

void
ReSemble::syncTargetNetwork()
{ targetNet = policyNet; }

std::vector<double>
ReSemble::forward(const Network &network, const std::vector<double> &state,
                  std::vector<double> *hidden,
                  std::vector<double> *hiddenPreActivation) const
{
    const auto inputDim = prefetchers.size();
    const auto outputDim = prefetchers.size() + 1;

    std::vector<double> hiddenLocal(hiddenDim, 0.0);
    std::vector<double> hiddenPreLocal(hiddenDim, 0.0);
    for (unsigned hiddenIdx = 0; hiddenIdx < hiddenDim; ++hiddenIdx) {
        double activation = network.hiddenBiases[hiddenIdx];
        const auto baseIndex = hiddenIdx * inputDim;
        for (std::size_t inputIdx = 0; inputIdx < inputDim; ++inputIdx) {
            activation += network.inputHiddenWeights[baseIndex + inputIdx] *
                          state[inputIdx];
        }
        hiddenPreLocal[hiddenIdx] = activation;
        hiddenLocal[hiddenIdx] = std::max(0.0, activation);
    }

    if (hidden != nullptr) {
        *hidden = hiddenLocal;
    }
    if (hiddenPreActivation != nullptr) {
        *hiddenPreActivation = hiddenPreLocal;
    }

    std::vector<double> outputs(outputDim, 0.0);
    for (std::size_t outputIdx = 0; outputIdx < outputDim; ++outputIdx) {
        double output = network.outputBiases[outputIdx];
        const auto baseIndex = outputIdx * hiddenDim;
        for (unsigned hiddenIdx = 0; hiddenIdx < hiddenDim; ++hiddenIdx) {
            output += network.hiddenOutputWeights[baseIndex + hiddenIdx] *
                      hiddenLocal[hiddenIdx];
        }
        outputs[outputIdx] = output;
    }
    return outputs;
}

} // namespace prefetch
} // namespace gem5
