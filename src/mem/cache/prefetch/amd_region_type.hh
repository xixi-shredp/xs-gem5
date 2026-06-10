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

/**
 * @file
 * AMD region type spatial-pattern prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_AMD_REGION_TYPE_HH__
#define __MEM_CACHE_PREFETCH_AMD_REGION_TYPE_HH__

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct AMDRegionTypePrefetcherParams;

namespace prefetch
{

class AMDRegionTypePrefetcher : public Queued
{
  private:
    static constexpr unsigned InvalidPattern =
        std::numeric_limits<unsigned>::max();

    const Addr regionSize;
    const unsigned linesPerRegion;
    const unsigned observationEntries;
    const unsigned regionTypeEntries;
    const unsigned recordedPatternEntries;
    const unsigned observationWindow;
    const unsigned observationTimeout;
    const unsigned duplicatePrefetchWindow;
    const unsigned recordedPatternAssoc;
    const unsigned regionTypeCandidates;
    const bool useRequestorId;
    const unsigned confidenceCounterBits;
    const unsigned initialConfidence;
    const unsigned confidenceThreshold;
    const unsigned agingInterval;
    const unsigned similarityThreshold;
    const unsigned minPatternBits;
    const unsigned degree;
    const Addr prefetchDistance;
    const std::string mergePolicy;
    const bool prefetchCurrent;

    uint64_t accessCounter;
    uint64_t lastAgingAccess;

    struct RegionKey
    {
        RequestorID requestorId = 0;
        bool secure = false;
        Addr regionBase = 0;

        bool
        operator<(const RegionKey &other) const
        {
            if (requestorId != other.requestorId) {
                return requestorId < other.requestorId;
            }
            if (secure != other.secure) {
                return secure < other.secure;
            }
            return regionBase < other.regionBase;
        }

        bool
        operator==(const RegionKey &other) const
        {
            return requestorId == other.requestorId &&
                   secure == other.secure && regionBase == other.regionBase;
        }

        bool
        operator!=(const RegionKey &other) const
        {
            return !(*this == other);
        }
    };

    struct ObservationEntry
    {
        RequestorID requestorId = 0;
        bool secure = false;
        Addr regionBase = 0;
        uint64_t pattern = 0;
        uint64_t createdAt = 0;
        uint64_t lastTouch = 0;
    };

    struct RegionTypeCandidate
    {
        unsigned patternIndex = InvalidPattern;
        uint16_t signature = 0;
        unsigned way = 0;
        unsigned confidence = 0;
        uint64_t lastTouch = 0;
    };

    struct RegionTypeEntry
    {
        RequestorID requestorId = 0;
        bool secure = false;
        Addr regionBase = 0;
        std::vector<RegionTypeCandidate> candidates;
        uint64_t lastTouch = 0;
    };

    struct RecordedPatternEntry
    {
        bool valid = false;
        uint64_t pattern = 0;
        uint16_t signature = 0;
        unsigned way = 0;
        unsigned refCount = 0;
        unsigned confidence = 0;
        uint64_t lastTouch = 0;
        bool replayInFlight = false;
        uint64_t replayStarted = 0;
    };

    struct ReplayTracker
    {
        RegionKey key;
        unsigned patternIndex = InvalidPattern;
        uint16_t signature = 0;
        unsigned way = 0;
        uint64_t issuedMask = 0;
        uint64_t usefulMask = 0;
        uint64_t missMask = 0;
        uint64_t createdAt = 0;
        uint64_t lastTouch = 0;
    };

    std::map<RegionKey, ObservationEntry> observationTable;
    std::deque<RegionKey> observationLru;

    std::map<RegionKey, RegionTypeEntry> regionTypeTable;
    std::deque<RegionKey> regionTypeLru;

    std::vector<RecordedPatternEntry> recordedPatterns;
    std::vector<ReplayTracker> replayTrackers;

    struct AmdStats : public statistics::Group
    {
        AmdStats(statistics::Group *parent);

        statistics::Scalar observationsInstalled;
        statistics::Scalar observationsClearedSparse;
        statistics::Scalar patternsAllocated;
        statistics::Scalar patternsMerged;
        statistics::Scalar mappingsInstalled;
        statistics::Scalar mappingsErased;
        statistics::Scalar replayAttempts;
        statistics::Scalar replayIssuedCandidates;
        statistics::Scalar replayDuplicateSuppressed;
        statistics::Scalar replayFilteredInCache;
        statistics::Scalar replayFilteredDistance;
        statistics::Scalar replayFilteredCurrent;
        statistics::Scalar replayExpiredUnused;
        statistics::Scalar confidencePositive;
        statistics::Scalar confidenceNegative;
        statistics::Scalar candidateAged;
    } statsAmd;

    uint64_t regionMask() const;
    uint16_t makeSignature(uint64_t pattern) const;
    unsigned patternPopCount(uint64_t pattern) const;
    unsigned maxConfidence() const;
    unsigned clampConfidence(unsigned confidence) const;
    unsigned patternWay(unsigned pattern_index) const;

    RegionKey makeRegionKey(const PrefetchInfo &pfi, Addr region_base) const;

    void touchObservation(const RegionKey &key);
    void touchRegionType(const RegionKey &key);
    void removeObservationLru(const RegionKey &key);
    void removeRegionTypeLru(const RegionKey &key);

    void observeRegion(const RegionKey &key, unsigned subdivision);
    void ageObservations();
    void evictObservation();
    void installObservation(const ObservationEntry &entry);

    unsigned findSimilarPattern(uint64_t pattern) const;
    unsigned allocateRecordedPattern(uint64_t pattern);
    void mergeRecordedPattern(unsigned index, uint64_t pattern);
    void refreshMappingsForPattern(unsigned pattern_index);
    void syncReplayTrackersForPattern(unsigned pattern_index);

    void installRegionTypeMapping(const RegionKey &key,
                                  unsigned pattern_index);
    void eraseRegionTypeMapping(const RegionKey &key);
    void evictRegionType();
    void invalidateMappingsForPattern(unsigned pattern_index);
    void decrementRefCount(unsigned pattern_index);

    RegionTypeCandidate *selectCandidate(RegionTypeEntry &entry,
                                         bool require_confident);
    RegionTypeCandidate *findCandidate(RegionTypeEntry &entry,
                                       unsigned pattern_index,
                                       uint16_t signature, unsigned way);
    void adjustConfidence(RegionTypeCandidate &candidate,
                          RecordedPatternEntry &recorded, bool positive);
    void recordMissFeedback(const RegionKey &key, unsigned subdivision);
    void updateMappingConfidence(const RegionKey &key, unsigned subdivision,
                                 const PrefetchInfo &pfi);
    void ageRecordedPatterns();
    void ageRegionTypeCandidates();
    bool hasActiveReplay(const RegionKey &key, unsigned pattern_index,
                         uint16_t signature, unsigned way) const;
    uint64_t replayFeedbackWindow() const;
    void trackReplay(const RegionKey &key, unsigned pattern_index,
                     uint16_t signature, unsigned way, uint64_t issued_mask);
    void expireReplayTrackers();

    void replayRegion(const RegionKey &key, unsigned trigger_subdivision,
                      const PrefetchInfo &pfi,
                      std::vector<AddrPriority> &addresses);

  public:
    AMDRegionTypePrefetcher(const AMDRegionTypePrefetcherParams &p);
    ~AMDRegionTypePrefetcher() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_AMD_REGION_TYPE_HH__
