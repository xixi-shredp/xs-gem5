/*
 * Micro-Armed Bandit (DUCB) prefetcher manager implementation.
 */

#include "mem/cache/prefetch/bandit.hh"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/logging.hh"
#include "cpu/base.hh"
#include "params/BanditPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

Bandit::Bandit(const BanditPrefetcherParams &p)
    : Multi(p),
      numArms(p.arm_masks.size()),
      armMask(p.arm_masks.begin(), p.arm_masks.end()),
      rTable(numArms, 0.0),
      nTable(numArms, 0.0),
      nTotal(0.0),
      gamma(p.gamma),
      c(p.c),
      banditStep(p.bandit_step),
      banditStepRR(p.bandit_step_rr),
      currentArm(0),
      rrRemaining(numArms),
      stepAccesses(0),
      stepStartTick(0),
      stepStartInsts(0),
      cpu(p.cpu)
{
    fatal_if(numArms == 0, "Bandit: arm_masks must be non-empty");
    fatal_if(gamma <= 0.0 || gamma > 1.0, "Bandit: gamma must be in (0, 1]");
}

void
Bandit::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *_cache,
                      unsigned blk_size)
{
    Multi::setParentInfo(sys, pm, _cache, blk_size);
    stepStartTick = curTick();
    stepStartInsts = readTotalInsts();
}

uint64_t
Bandit::readTotalInsts() const
{
    if (cpu != nullptr) {
        return cpu->totalInsts();
    }
    return BaseCPU::numSimulatedInsts();
}

Tick
Bandit::nextPrefetchReadyTime() const
{
    Tick next_ready = MaxTick;
    uint64_t mask = armMask[currentArm];
    for (size_t i = 0; i < prefetchers.size(); ++i) {
        if (mask & (1ULL << i)) {
            next_ready =
                std::min(next_ready, prefetchers[i]->nextPrefetchReadyTime());
        }
    }
    return next_ready;
}


bool
Bandit::hasPendingPacket()
{
    uint64_t mask = armMask[currentArm];
    for (size_t i = 0; i < prefetchers.size(); ++i) {
        if ((mask & (1ULL << i)) &&
            prefetchers[i]->nextPrefetchReadyTime() <= curTick()) {
            return true;
        }
    }

    return false;
}

PacketPtr
Bandit::getPacket()
{
    uint64_t mask = armMask[currentArm];
    const size_t n = prefetchers.size();
    lastChosenPf = (lastChosenPf + 1) % n;
    size_t turn = lastChosenPf;
    for (size_t i = 0; i < n; ++i) {
        if ((mask & (1ULL << turn)) &&
            prefetchers[turn]->nextPrefetchReadyTime() <= curTick()) {
            PacketPtr pkt = prefetchers[turn]->getPacket();
            panic_if(!pkt,
                     "Bandit: sub-prefetcher ready but returned no packet.");
            prefetchStats.pfIssued++;
            issuedPrefetches++;
            return pkt;
        }
        turn = (turn + 1) % n;
    }
    return nullptr;
}

void
Bandit::calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses)
{
    calculatePrefetch(pfi, addresses, false, PrefetchSourceType::PF_NONE,
                      false);
}

void
Bandit::calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses, bool late,
                          PrefetchSourceType source, bool miss_repeat)
{
    observeAccess();
    const uint64_t mask = armMask[currentArm];
    for (size_t i = 0; i < prefetchers.size(); ++i) {
        if (((mask >> i) & 1ULL) == 0) {
            continue;
        }
        auto *queued = dynamic_cast<Queued *>(prefetchers[i]);
        fatal_if(!queued, "%s: child %s is not a Queued prefetcher",
                 name(), prefetchers[i]->name());
        queued->calculatePrefetch(pfi, addresses, late, source, miss_repeat);
    }
}

void
Bandit::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    observeAccess();
}

void
Bandit::observeAccess()
{
    stepAccesses++;
    uint64_t threshold = (rrRemaining > 0) ? banditStepRR : banditStep;
    if (stepAccesses < threshold) {
        return;
    }

    Tick now = curTick();
    uint64_t insts = readTotalInsts();
    Tick dTicks = now - stepStartTick;
    uint64_t dInsts = insts - stepStartInsts;
    Cycles dCycles(divCeil(dTicks, clockPeriod()));
    double reward = 0.0;
    if (dCycles > 0) {
        reward = static_cast<double>(dInsts) / static_cast<double>(dCycles);
    }

    finishStep(reward);

    stepAccesses = 0;
    stepStartTick = now;
    stepStartInsts = insts;
    currentArm = selectNextArm();
}

void
Bandit::finishStep(double reward)
{
    // DUCB updSels: discount all n_i by gamma, then increment chosen arm.
    for (auto &n : nTable) {
        n *= gamma;
    }
    nTable[currentArm] += 1.0;
    nTotal = gamma * nTotal + 1.0;

    // updRew: running average r_arm over n_arm selections.
    double n_arm = nTable[currentArm];
    rTable[currentArm] = (rTable[currentArm] * (n_arm - 1.0) + reward) / n_arm;

    if (rrRemaining > 0) {
        rrRemaining--;
    }
}

uint32_t
Bandit::selectNextArm()
{
    // Initial Round Robin phase: try every arm once.
    if (rrRemaining > 0) {
        uint32_t tried = numArms - rrRemaining;
        return tried % numArms;
    }

    // DUCB: arm with the highest potential.
    double logN = std::log(std::max(nTotal, 1.0));
    double best_val = -std::numeric_limits<double>::infinity();
    uint32_t best_arm = 0;
    for (uint32_t a = 0; a < numArms; ++a) {
        double n_a = std::max(nTable[a], 1e-9);
        double potential = rTable[a] + c * std::sqrt(logN / n_a);
        if (potential > best_val) {
            best_val = potential;
            best_arm = a;
        }
    }
    return best_arm;
}

} // namespace prefetch
} // namespace gem5
