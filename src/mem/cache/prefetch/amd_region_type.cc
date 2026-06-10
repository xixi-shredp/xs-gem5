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

#include "mem/cache/prefetch/amd_region_type.hh"

#include <algorithm>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/AMDRegionTypePrefetcher.hh"

namespace gem5
{

namespace prefetch
{

AMDRegionTypePrefetcher::AmdStats::AmdStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(observationsInstalled, statistics::units::Count::get(),
               "number of non-sparse region observations installed"),
      ADD_STAT(observationsClearedSparse, statistics::units::Count::get(),
               "number of sparse observations that cleared stale mappings"),
      ADD_STAT(patternsAllocated, statistics::units::Count::get(),
               "number of recorded pattern entries allocated"),
      ADD_STAT(
          patternsMerged, statistics::units::Count::get(),
          "number of observations merged into existing recorded patterns"),
      ADD_STAT(mappingsInstalled, statistics::units::Count::get(),
               "number of region-type candidate mappings installed"),
      ADD_STAT(mappingsErased, statistics::units::Count::get(),
               "number of region-type candidate mappings erased"),
      ADD_STAT(replayAttempts, statistics::units::Count::get(),
               "number of confident region replay attempts"),
      ADD_STAT(replayIssuedCandidates, statistics::units::Count::get(),
               "number of replay candidates emitted after local filtering"),
      ADD_STAT(replayDuplicateSuppressed, statistics::units::Count::get(),
               "number of duplicate replay attempts suppressed"),
      ADD_STAT(replayFilteredInCache, statistics::units::Count::get(),
               "number of replay candidates already present in cache or MSHR"),
      ADD_STAT(replayFilteredDistance, statistics::units::Count::get(),
               "number of replay candidates outside prefetch distance"),
      ADD_STAT(replayFilteredCurrent, statistics::units::Count::get(),
               "number of triggering subdivisions filtered from replay"),
      ADD_STAT(replayExpiredUnused, statistics::units::Count::get(),
               "number of tracked replay lines that expired unused"),
      ADD_STAT(confidencePositive, statistics::units::Count::get(),
               "number of confidence increments from useful tracked replays"),
      ADD_STAT(confidenceNegative, statistics::units::Count::get(),
               "number of confidence decrements from unused or missed tracked "
               "replays"),
      ADD_STAT(candidateAged, statistics::units::Count::get(),
               "number of stale region-type candidates aged")
{}

AMDRegionTypePrefetcher::AMDRegionTypePrefetcher(
    const AMDRegionTypePrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      linesPerRegion(p.region_size / p.block_size),
      observationEntries(p.observation_entries),
      regionTypeEntries(p.region_type_entries),
      recordedPatternEntries(p.recorded_pattern_entries),
      observationWindow(p.observation_window),
      observationTimeout(p.observation_timeout),
      duplicatePrefetchWindow(p.duplicate_prefetch_window),
      recordedPatternAssoc(p.recorded_pattern_assoc),
      regionTypeCandidates(p.region_type_candidates),
      useRequestorId(p.use_requestor_id),
      confidenceCounterBits(p.confidence_counter_bits),
      initialConfidence(p.initial_confidence),
      confidenceThreshold(p.confidence_threshold),
      agingInterval(p.aging_interval),
      similarityThreshold(p.similarity_threshold),
      minPatternBits(p.min_pattern_bits),
      degree(p.degree),
      prefetchDistance(p.prefetch_distance),
      mergePolicy(p.merge_policy),
      prefetchCurrent(p.prefetch_current),
      accessCounter(0),
      lastAgingAccess(0),
      recordedPatterns(recordedPatternEntries),
      statsAmd(this)
{
    fatal_if(!isPowerOf2(regionSize),
             "AMDRegionTypePrefetcher region_size must be a power of two");
    fatal_if(
        regionSize < blkSize,
        "AMDRegionTypePrefetcher region_size must be at least block_size");
    fatal_if((regionSize % blkSize) != 0,
             "AMDRegionTypePrefetcher region_size must contain whole blocks");
    fatal_if(linesPerRegion == 0 || linesPerRegion > 64,
             "AMDRegionTypePrefetcher supports 1..64 subdivisions per region");
    fatal_if(observationEntries == 0,
             "AMDRegionTypePrefetcher observation_entries must be non-zero");
    fatal_if(regionTypeEntries == 0,
             "AMDRegionTypePrefetcher region_type_entries must be non-zero");
    fatal_if(
        recordedPatternEntries == 0,
        "AMDRegionTypePrefetcher recorded_pattern_entries must be non-zero");
    fatal_if(
        recordedPatternAssoc == 0,
        "AMDRegionTypePrefetcher recorded_pattern_assoc must be non-zero");
    fatal_if(
        regionTypeCandidates == 0,
        "AMDRegionTypePrefetcher region_type_candidates must be non-zero");
    fatal_if(confidenceCounterBits == 0 || confidenceCounterBits > 31,
             "AMDRegionTypePrefetcher confidence_counter_bits must be 1..31");
    fatal_if(initialConfidence > maxConfidence() ||
                 confidenceThreshold > maxConfidence(),
             "AMDRegionTypePrefetcher confidence values exceed counter max");
    fatal_if(
        mergePolicy != "or" && mergePolicy != "and" &&
            mergePolicy != "replace",
        "AMDRegionTypePrefetcher merge_policy must be or, and, or replace");
}

uint64_t
AMDRegionTypePrefetcher::regionMask() const
{
    return linesPerRegion == 64 ? ~0ULL : ((1ULL << linesPerRegion) - 1);
}

uint16_t
AMDRegionTypePrefetcher::makeSignature(uint64_t pattern) const
{
    pattern &= regionMask();
    pattern ^= pattern >> 32;
    pattern ^= pattern >> 16;
    return static_cast<uint16_t>(pattern);
}

unsigned
AMDRegionTypePrefetcher::patternPopCount(uint64_t pattern) const
{
    return popCount(pattern & regionMask());
}

unsigned
AMDRegionTypePrefetcher::maxConfidence() const
{
    return (1U << confidenceCounterBits) - 1;
}

unsigned
AMDRegionTypePrefetcher::clampConfidence(unsigned confidence) const
{
    return std::min(confidence, maxConfidence());
}

unsigned
AMDRegionTypePrefetcher::patternWay(unsigned pattern_index) const
{
    return pattern_index % recordedPatternAssoc;
}

AMDRegionTypePrefetcher::RegionKey
AMDRegionTypePrefetcher::makeRegionKey(const PrefetchInfo &pfi,
                                       Addr region_base) const
{
    return RegionKey{useRequestorId ? pfi.getRequestorId()
                                    : static_cast<RequestorID>(0),
                     pfi.isSecure(), region_base};
}

void
AMDRegionTypePrefetcher::touchObservation(const RegionKey &key)
{
    auto it = std::find(observationLru.begin(), observationLru.end(), key);
    if (it != observationLru.end()) {
        observationLru.erase(it);
    }
    observationLru.push_front(key);
}

void
AMDRegionTypePrefetcher::touchRegionType(const RegionKey &key)
{
    auto it = std::find(regionTypeLru.begin(), regionTypeLru.end(), key);
    if (it != regionTypeLru.end()) {
        regionTypeLru.erase(it);
    }
    regionTypeLru.push_front(key);
}

void
AMDRegionTypePrefetcher::removeObservationLru(const RegionKey &key)
{
    observationLru.erase(
        std::remove(observationLru.begin(), observationLru.end(), key),
        observationLru.end());
}

void
AMDRegionTypePrefetcher::removeRegionTypeLru(const RegionKey &key)
{
    regionTypeLru.erase(
        std::remove(regionTypeLru.begin(), regionTypeLru.end(), key),
        regionTypeLru.end());
}

void
AMDRegionTypePrefetcher::observeRegion(const RegionKey &key,
                                       unsigned subdivision)
{
    const uint64_t bit = 1ULL << subdivision;
    auto it = observationTable.find(key);
    if (it != observationTable.end()) {
        it->second.pattern |= bit;
        it->second.lastTouch = accessCounter;
        touchObservation(key);
        if (observationWindow != 0 &&
            accessCounter - it->second.createdAt >= observationWindow) {
            ObservationEntry completed = it->second;
            observationTable.erase(it);
            removeObservationLru(key);
            installObservation(completed);
        }
        return;
    }

    while (observationTable.size() >= observationEntries) {
        evictObservation();
    }

    ObservationEntry entry;
    entry.requestorId = key.requestorId;
    entry.secure = key.secure;
    entry.regionBase = key.regionBase;
    entry.pattern = bit;
    entry.createdAt = accessCounter;
    entry.lastTouch = accessCounter;
    observationTable[key] = entry;
    touchObservation(key);
}

void
AMDRegionTypePrefetcher::ageObservations()
{
    if (observationTimeout == 0) {
        return;
    }

    for (auto it = observationTable.begin(); it != observationTable.end();) {
        if (accessCounter - it->second.lastTouch >= observationTimeout) {
            const RegionKey key = it->first;
            ObservationEntry completed = it->second;
            it = observationTable.erase(it);
            removeObservationLru(key);
            installObservation(completed);
        } else {
            ++it;
        }
    }
}

void
AMDRegionTypePrefetcher::evictObservation()
{
    while (!observationLru.empty()) {
        const RegionKey victim = observationLru.back();
        observationLru.pop_back();
        auto it = observationTable.find(victim);
        if (it == observationTable.end()) {
            continue;
        }
        installObservation(it->second);
        observationTable.erase(it);
        return;
    }
}

void
AMDRegionTypePrefetcher::installObservation(const ObservationEntry &entry)
{
    const RegionKey key{entry.requestorId, entry.secure, entry.regionBase};
    const uint64_t pattern = entry.pattern & regionMask();
    if (patternPopCount(pattern) < minPatternBits) {
        statsAmd.observationsClearedSparse++;
        eraseRegionTypeMapping(key);
        return;
    }
    statsAmd.observationsInstalled++;

    unsigned pattern_index = findSimilarPattern(pattern);
    if (pattern_index == InvalidPattern) {
        pattern_index = allocateRecordedPattern(pattern);
    } else {
        mergeRecordedPattern(pattern_index, pattern);
    }

    installRegionTypeMapping(key, pattern_index);
}

unsigned
AMDRegionTypePrefetcher::findSimilarPattern(uint64_t pattern) const
{
    unsigned best_index = InvalidPattern;
    unsigned best_diff = std::numeric_limits<unsigned>::max();

    for (unsigned i = 0; i < recordedPatterns.size(); ++i) {
        const auto &entry = recordedPatterns[i];
        if (!entry.valid) {
            continue;
        }

        const unsigned diff = patternPopCount(pattern ^ entry.pattern);
        const bool exact = diff == 0;
        const bool near =
            similarityThreshold > 0 && diff > 0 && diff < similarityThreshold;
        if ((exact || near) && diff < best_diff) {
            best_index = i;
            best_diff = diff;
        }
    }

    return best_index;
}

unsigned
AMDRegionTypePrefetcher::allocateRecordedPattern(uint64_t pattern)
{
    unsigned victim = InvalidPattern;
    for (unsigned i = 0; i < recordedPatterns.size(); ++i) {
        if (!recordedPatterns[i].valid) {
            victim = i;
            break;
        }
    }

    if (victim == InvalidPattern) {
        victim = 0;
        for (unsigned i = 1; i < recordedPatterns.size(); ++i) {
            const auto &candidate = recordedPatterns[i];
            const auto &current = recordedPatterns[victim];
            if (candidate.refCount < current.refCount ||
                (candidate.refCount == current.refCount &&
                 candidate.lastTouch < current.lastTouch)) {
                victim = i;
            }
        }
        invalidateMappingsForPattern(victim);
    }

    auto &entry = recordedPatterns[victim];
    entry.valid = true;
    entry.pattern = pattern & regionMask();
    entry.signature = makeSignature(pattern);
    entry.way = patternWay(victim);
    entry.refCount = 0;
    entry.confidence = clampConfidence(initialConfidence);
    entry.lastTouch = accessCounter;
    entry.replayInFlight = false;
    entry.replayStarted = 0;
    statsAmd.patternsAllocated++;
    return victim;
}

void
AMDRegionTypePrefetcher::mergeRecordedPattern(unsigned index, uint64_t pattern)
{
    auto &entry = recordedPatterns[index];
    if (mergePolicy == "and") {
        entry.pattern &= pattern;
    } else if (mergePolicy == "replace") {
        invalidateMappingsForPattern(index);
        entry.pattern = pattern;
    } else {
        entry.pattern |= pattern;
    }
    entry.pattern &= regionMask();
    entry.signature = makeSignature(entry.pattern);
    entry.way = patternWay(index);
    entry.confidence =
        std::max(entry.confidence, clampConfidence(initialConfidence));
    entry.lastTouch = accessCounter;
    entry.replayInFlight = false;
    refreshMappingsForPattern(index);
    statsAmd.patternsMerged++;
}

void
AMDRegionTypePrefetcher::refreshMappingsForPattern(unsigned pattern_index)
{
    if (pattern_index >= recordedPatterns.size() ||
        !recordedPatterns[pattern_index].valid) {
        return;
    }

    const auto &recorded = recordedPatterns[pattern_index];
    for (auto it = regionTypeTable.begin(); it != regionTypeTable.end();) {
        auto &candidates = it->second.candidates;
        for (auto &candidate : candidates) {
            if (candidate.patternIndex == pattern_index) {
                candidate.signature = recorded.signature;
                candidate.way = recorded.way;
            }
        }
        ++it;
    }

    syncReplayTrackersForPattern(pattern_index);
}

void
AMDRegionTypePrefetcher::syncReplayTrackersForPattern(unsigned pattern_index)
{
    if (pattern_index >= recordedPatterns.size() ||
        !recordedPatterns[pattern_index].valid) {
        return;
    }

    const auto &recorded = recordedPatterns[pattern_index];
    for (auto &tracker : replayTrackers) {
        if (tracker.patternIndex != pattern_index) {
            continue;
        }
        tracker.signature = recorded.signature;
        tracker.way = recorded.way;
        tracker.lastTouch = accessCounter;
    }
}

void
AMDRegionTypePrefetcher::installRegionTypeMapping(const RegionKey &key,
                                                  unsigned pattern_index)
{
    auto existing = regionTypeTable.find(key);
    if (existing != regionTypeTable.end()) {
        auto &entry = existing->second;
        auto &candidates = entry.candidates;
        auto candidate = std::find_if(
            candidates.begin(), candidates.end(),
            [pattern_index](const RegionTypeCandidate &candidate) {
                return candidate.patternIndex == pattern_index;
            });
        bool new_candidate = false;
        if (candidate == candidates.end()) {
            if (candidates.size() >= regionTypeCandidates) {
                auto victim = std::min_element(
                    candidates.begin(), candidates.end(),
                    [](const RegionTypeCandidate &a,
                       const RegionTypeCandidate &b) {
                        return a.confidence < b.confidence ||
                               (a.confidence == b.confidence &&
                                a.lastTouch < b.lastTouch);
                    });
                decrementRefCount(victim->patternIndex);
                *victim = RegionTypeCandidate();
                candidate = victim;
            } else {
                candidates.push_back(RegionTypeCandidate());
                candidate = candidates.end() - 1;
            }
            new_candidate = true;
            recordedPatterns[pattern_index].refCount++;
            statsAmd.mappingsInstalled++;
        }
        candidate->patternIndex = pattern_index;
        candidate->signature = recordedPatterns[pattern_index].signature;
        candidate->way = recordedPatterns[pattern_index].way;
        if (new_candidate) {
            candidate->confidence = recordedPatterns[pattern_index].confidence;
        }
        candidate->lastTouch = accessCounter;
        entry.lastTouch = accessCounter;
        touchRegionType(key);
        return;
    }

    while (regionTypeTable.size() >= regionTypeEntries) {
        evictRegionType();
    }

    RegionTypeEntry entry;
    entry.requestorId = key.requestorId;
    entry.secure = key.secure;
    entry.regionBase = key.regionBase;
    entry.lastTouch = accessCounter;

    RegionTypeCandidate candidate;
    candidate.patternIndex = pattern_index;
    candidate.signature = recordedPatterns[pattern_index].signature;
    candidate.way = recordedPatterns[pattern_index].way;
    candidate.confidence = recordedPatterns[pattern_index].confidence;
    candidate.lastTouch = accessCounter;
    entry.candidates.push_back(candidate);

    regionTypeTable[key] = entry;
    recordedPatterns[pattern_index].refCount++;
    statsAmd.mappingsInstalled++;
    touchRegionType(key);
}

void
AMDRegionTypePrefetcher::eraseRegionTypeMapping(const RegionKey &key)
{
    auto it = regionTypeTable.find(key);
    if (it == regionTypeTable.end()) {
        return;
    }

    for (const auto &candidate : it->second.candidates) {
        decrementRefCount(candidate.patternIndex);
        statsAmd.mappingsErased++;
    }
    regionTypeTable.erase(it);
    removeRegionTypeLru(key);
}

void
AMDRegionTypePrefetcher::evictRegionType()
{
    while (!regionTypeLru.empty()) {
        const RegionKey victim = regionTypeLru.back();
        regionTypeLru.pop_back();
        auto it = regionTypeTable.find(victim);
        if (it == regionTypeTable.end()) {
            continue;
        }
        for (const auto &candidate : it->second.candidates) {
            decrementRefCount(candidate.patternIndex);
            statsAmd.mappingsErased++;
        }
        regionTypeTable.erase(it);
        return;
    }
}

void
AMDRegionTypePrefetcher::invalidateMappingsForPattern(unsigned pattern_index)
{
    for (auto it = regionTypeTable.begin(); it != regionTypeTable.end();) {
        auto &candidates = it->second.candidates;
        const auto old_size = candidates.size();
        candidates.erase(
            std::remove_if(
                candidates.begin(), candidates.end(),
                [pattern_index](const RegionTypeCandidate &candidate) {
                    return candidate.patternIndex == pattern_index;
                }),
            candidates.end());
        statsAmd.mappingsErased += old_size - candidates.size();
        if (candidates.empty()) {
            const RegionKey key = it->first;
            it = regionTypeTable.erase(it);
            removeRegionTypeLru(key);
        } else {
            ++it;
        }
    }

    if (pattern_index < recordedPatterns.size()) {
        recordedPatterns[pattern_index].refCount = 0;
    }
}

void
AMDRegionTypePrefetcher::decrementRefCount(unsigned pattern_index)
{
    if (pattern_index >= recordedPatterns.size()) {
        return;
    }
    auto &entry = recordedPatterns[pattern_index];
    if (entry.refCount > 0) {
        entry.refCount--;
    }
}

AMDRegionTypePrefetcher::RegionTypeCandidate *
AMDRegionTypePrefetcher::selectCandidate(RegionTypeEntry &entry,
                                         bool require_confident)
{
    RegionTypeCandidate *best = nullptr;
    unsigned best_score = 0;

    for (auto &candidate : entry.candidates) {
        if (candidate.patternIndex >= recordedPatterns.size()) {
            continue;
        }

        auto &recorded = recordedPatterns[candidate.patternIndex];
        if (!recorded.valid || candidate.signature != recorded.signature ||
            candidate.way != recorded.way) {
            continue;
        }

        if (require_confident && (candidate.confidence < confidenceThreshold ||
                                  recorded.confidence < confidenceThreshold)) {
            continue;
        }

        const unsigned score = candidate.confidence + recorded.confidence;
        if (best == nullptr || score > best_score ||
            (score == best_score && candidate.lastTouch > best->lastTouch)) {
            best = &candidate;
            best_score = score;
        }
    }

    return best;
}

AMDRegionTypePrefetcher::RegionTypeCandidate *
AMDRegionTypePrefetcher::findCandidate(RegionTypeEntry &entry,
                                       unsigned pattern_index,
                                       uint16_t signature, unsigned way)
{
    for (auto &candidate : entry.candidates) {
        if (candidate.patternIndex != pattern_index ||
            candidate.signature != signature || candidate.way != way ||
            pattern_index >= recordedPatterns.size()) {
            continue;
        }
        if (!recordedPatterns[pattern_index].valid) {
            continue;
        }
        return &candidate;
    }
    return nullptr;
}

void
AMDRegionTypePrefetcher::adjustConfidence(RegionTypeCandidate &candidate,
                                          RecordedPatternEntry &recorded,
                                          bool positive)
{
    if (positive) {
        candidate.confidence =
            std::min(candidate.confidence + 1, maxConfidence());
        recorded.confidence =
            std::min(recorded.confidence + 1, maxConfidence());
    } else {
        if (candidate.confidence > 0) {
            candidate.confidence--;
        }
        if (recorded.confidence > 0) {
            recorded.confidence--;
        }
    }
    candidate.lastTouch = accessCounter;
    recorded.lastTouch = accessCounter;
}

void
AMDRegionTypePrefetcher::recordMissFeedback(const RegionKey &key,
                                            unsigned subdivision)
{
    const uint64_t bit = 1ULL << subdivision;
    auto mapping = regionTypeTable.find(key);
    if (mapping == regionTypeTable.end()) {
        return;
    }

    for (auto &tracker : replayTrackers) {
        if (tracker.key != key || (tracker.missMask & bit) != 0 ||
            (tracker.usefulMask & bit) != 0 ||
            (tracker.issuedMask & bit) == 0 ||
            tracker.patternIndex >= recordedPatterns.size()) {
            continue;
        }

        auto *candidate = findCandidate(mapping->second, tracker.patternIndex,
                                        tracker.signature, tracker.way);
        auto &recorded = recordedPatterns[tracker.patternIndex];
        if (candidate == nullptr || !recorded.valid) {
            continue;
        }

        adjustConfidence(*candidate, recorded, false);
        tracker.missMask |= bit;
        tracker.lastTouch = accessCounter;
        statsAmd.confidenceNegative++;
    }
}

void
AMDRegionTypePrefetcher::updateMappingConfidence(const RegionKey &key,
                                                 unsigned subdivision,
                                                 const PrefetchInfo &pfi)
{
    const Addr blk_addr = blockAddress(pfi.getAddr());
    const bool demand_miss = pfi.isCacheMiss();
    const bool was_prefetched =
        cache != nullptr &&
        cache->hasBeenPrefetched(blk_addr, pfi.isSecure());
    if (demand_miss && !was_prefetched) {
        recordMissFeedback(key, subdivision);
    }
    if (!was_prefetched) {
        return;
    }

    const uint64_t bit = 1ULL << subdivision;
    for (auto &tracker : replayTrackers) {
        if (tracker.key != key || (tracker.issuedMask & bit) == 0 ||
            (tracker.usefulMask & bit) != 0) {
            continue;
        }

        auto mapping = regionTypeTable.find(key);
        if (mapping == regionTypeTable.end() ||
            tracker.patternIndex >= recordedPatterns.size()) {
            return;
        }

        auto *candidate = findCandidate(mapping->second, tracker.patternIndex,
                                        tracker.signature, tracker.way);
        if (candidate == nullptr) {
            return;
        }

        auto &recorded = recordedPatterns[tracker.patternIndex];
        adjustConfidence(*candidate, recorded, true);
        tracker.usefulMask |= bit;
        tracker.lastTouch = accessCounter;
        statsAmd.confidencePositive++;
        return;
    }
}

void
AMDRegionTypePrefetcher::ageRecordedPatterns()
{
    if (agingInterval == 0 ||
        accessCounter - lastAgingAccess < agingInterval) {
        return;
    }
    lastAgingAccess = accessCounter;

    ageRegionTypeCandidates();

    for (unsigned i = 0; i < recordedPatterns.size(); ++i) {
        auto &recorded = recordedPatterns[i];
        if (!recorded.valid) {
            continue;
        }

        const uint64_t feedback_window = replayFeedbackWindow();
        if (recorded.replayInFlight && feedback_window != 0 &&
            accessCounter - recorded.replayStarted >= feedback_window) {
            recorded.replayInFlight = false;
        }

        if (accessCounter - recorded.lastTouch < agingInterval) {
            continue;
        }

        if (recorded.confidence > 0) {
            recorded.confidence--;
            continue;
        }

        invalidateMappingsForPattern(i);
        recorded.valid = false;
        recorded.pattern = 0;
        recorded.signature = 0;
        recorded.refCount = 0;
        recorded.replayInFlight = false;
    }
}

void
AMDRegionTypePrefetcher::ageRegionTypeCandidates()
{
    for (auto it = regionTypeTable.begin(); it != regionTypeTable.end();) {
        auto &candidates = it->second.candidates;
        for (auto candidate = candidates.begin();
             candidate != candidates.end();) {
            if (accessCounter - candidate->lastTouch < agingInterval) {
                ++candidate;
                continue;
            }

            if (candidate->confidence > 0) {
                candidate->confidence--;
                candidate->lastTouch = accessCounter;
                statsAmd.candidateAged++;
                ++candidate;
                continue;
            }

            decrementRefCount(candidate->patternIndex);
            candidate = candidates.erase(candidate);
            statsAmd.mappingsErased++;
        }

        if (candidates.empty()) {
            const RegionKey key = it->first;
            it = regionTypeTable.erase(it);
            removeRegionTypeLru(key);
        } else {
            ++it;
        }
    }
}

bool
AMDRegionTypePrefetcher::hasActiveReplay(const RegionKey &key,
                                         unsigned pattern_index,
                                         uint16_t signature,
                                         unsigned way) const
{
    if (duplicatePrefetchWindow == 0) {
        return false;
    }

    for (const auto &tracker : replayTrackers) {
        if (tracker.key == key && tracker.patternIndex == pattern_index &&
            tracker.signature == signature && tracker.way == way &&
            accessCounter - tracker.createdAt < duplicatePrefetchWindow) {
            return true;
        }
    }
    return false;
}

uint64_t
AMDRegionTypePrefetcher::replayFeedbackWindow() const
{
    return std::max<uint64_t>(duplicatePrefetchWindow, observationTimeout);
}

void
AMDRegionTypePrefetcher::trackReplay(const RegionKey &key,
                                     unsigned pattern_index,
                                     uint16_t signature, unsigned way,
                                     uint64_t issued_mask)
{
    if (replayFeedbackWindow() == 0 || issued_mask == 0) {
        return;
    }

    ReplayTracker tracker;
    tracker.key = key;
    tracker.patternIndex = pattern_index;
    tracker.signature = signature;
    tracker.way = way;
    tracker.issuedMask = issued_mask;
    tracker.createdAt = accessCounter;
    tracker.lastTouch = accessCounter;
    replayTrackers.push_back(tracker);
}

void
AMDRegionTypePrefetcher::expireReplayTrackers()
{
    const uint64_t feedback_window = replayFeedbackWindow();
    if (feedback_window == 0) {
        return;
    }

    for (auto tracker = replayTrackers.begin();
         tracker != replayTrackers.end();) {
        if (accessCounter - tracker->createdAt < feedback_window) {
            ++tracker;
            continue;
        }

        const uint64_t unused = tracker->issuedMask & ~tracker->usefulMask;
        const uint64_t unpenalized_unused = unused & ~tracker->missMask;
        if (unused != 0) {
            auto mapping = regionTypeTable.find(tracker->key);
            if (mapping != regionTypeTable.end() &&
                tracker->patternIndex < recordedPatterns.size()) {
                auto *candidate =
                    findCandidate(mapping->second, tracker->patternIndex,
                                  tracker->signature, tracker->way);
                auto &recorded = recordedPatterns[tracker->patternIndex];
                if (unpenalized_unused != 0 && candidate != nullptr &&
                    recorded.valid) {
                    adjustConfidence(*candidate, recorded, false);
                    statsAmd.confidenceNegative++;
                }
            }
            statsAmd.replayExpiredUnused += patternPopCount(unused);
        }

        if (tracker->patternIndex < recordedPatterns.size()) {
            recordedPatterns[tracker->patternIndex].replayInFlight = false;
        }
        tracker = replayTrackers.erase(tracker);
    }
}

void
AMDRegionTypePrefetcher::replayRegion(const RegionKey &key,
                                      unsigned trigger_subdivision,
                                      const PrefetchInfo &pfi,
                                      std::vector<AddrPriority> &addresses)
{
    auto mapping = regionTypeTable.find(key);
    if (mapping == regionTypeTable.end()) {
        return;
    }

    mapping->second.lastTouch = accessCounter;
    touchRegionType(key);

    auto *candidate = selectCandidate(mapping->second, true);
    if (candidate == nullptr) {
        return;
    }

    const unsigned pattern_index = candidate->patternIndex;
    if (pattern_index >= recordedPatterns.size()) {
        return;
    }

    auto &recorded = recordedPatterns[pattern_index];
    if (!recorded.valid || candidate->signature != recorded.signature ||
        candidate->way != recorded.way) {
        return;
    }

    if (recorded.confidence < confidenceThreshold ||
        candidate->confidence < confidenceThreshold) {
        return;
    }

    statsAmd.replayAttempts++;
    if (hasActiveReplay(key, pattern_index, recorded.signature,
                        recorded.way)) {
        statsAmd.replayDuplicateSuppressed++;
        return;
    }

    const uint64_t pattern = recorded.pattern & regionMask();
    unsigned issued = 0;
    uint64_t issued_mask = 0;
    const Addr trigger_addr = blockAddress(pfi.getAddr());

    auto emit = [&](unsigned subdivision) {
        if (issued >= degree || subdivision >= linesPerRegion) {
            return;
        }
        if (!prefetchCurrent && subdivision == trigger_subdivision) {
            statsAmd.replayFilteredCurrent++;
            return;
        }
        if ((pattern & (1ULL << subdivision)) == 0) {
            return;
        }

        const Addr pf_addr = blockAddress(
            key.regionBase + subdivision * static_cast<Addr>(blkSize));
        const Addr distance = pf_addr > trigger_addr ? pf_addr - trigger_addr
                                                     : trigger_addr - pf_addr;
        if (prefetchDistance != 0 && distance > prefetchDistance) {
            statsAmd.replayFilteredDistance++;
            return;
        }
        if (inCache(pf_addr, pfi.isSecure()) ||
            inMissQueue(pf_addr, pfi.isSecure())) {
            statsAmd.replayFilteredInCache++;
            return;
        }

        addresses.push_back(AddrPriority(
            pf_addr, 0, PrefetchSourceType::AMDRegionType));
        issued_mask |= 1ULL << subdivision;
        issued++;
    };

    for (unsigned delta = 0; delta < linesPerRegion && issued < degree;
         ++delta) {
        if (trigger_subdivision + delta < linesPerRegion) {
            emit(trigger_subdivision + delta);
        }
        if (delta != 0 && trigger_subdivision >= delta) {
            emit(trigger_subdivision - delta);
        }
    }

    if (issued > 0) {
        trackReplay(key, pattern_index, recorded.signature, recorded.way,
                    issued_mask);
        recorded.replayInFlight = true;
        recorded.replayStarted = accessCounter;
        statsAmd.replayIssuedCandidates += issued;
    }
    recorded.lastTouch = accessCounter;
    candidate->lastTouch = accessCounter;
}

void
AMDRegionTypePrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    accessCounter++;

    const Addr blk_addr = blockAddress(pfi.getAddr());
    const Addr region_base = roundDown(blk_addr, regionSize);
    const unsigned subdivision = (blk_addr - region_base) / blkSize;
    const RegionKey key = makeRegionKey(pfi, region_base);

    updateMappingConfidence(key, subdivision, pfi);
    expireReplayTrackers();
    ageObservations();
    ageRecordedPatterns();
    replayRegion(key, subdivision, pfi, addresses);
    observeRegion(key, subdivision);
}

} // namespace prefetch
} // namespace gem5
