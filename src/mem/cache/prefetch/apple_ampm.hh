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
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_CACHE_PREFETCH_APPLE_AMPM_HH__
#define __MEM_CACHE_PREFETCH_APPLE_AMPM_HH__

#include <cstdint>
#include <vector>

#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "sim/byteswap.hh"

namespace gem5
{

struct AppleAMPMPrefetcherParams;

namespace prefetch
{

class AppleAMPM : public Queued
{
    const unsigned limitStride;
    const unsigned degree;
    const uint64_t hotZoneSize;

    const unsigned initialQualityFactor;
    const unsigned maxQualityFactor;
    const unsigned prefetchTokenCost;
    const unsigned storeOnlyPrefetchTokenCost;
    const unsigned successfulPrefetchTokens;
    const unsigned cacheHitPenaltyTokens;
    const unsigned pointerPrefetchTokens;
    const unsigned qualityFactorBypassAccesses;

    const bool usePointerValueHeuristic;
    const unsigned pointerFieldMax;
    const unsigned pointerInitialValue;
    const unsigned pointerIncrement;
    const unsigned pointerDecrement;
    const unsigned pointerThreshold;
    const unsigned pointerTrackingEntries;
    const unsigned pointerTrackingWindow;
    const Addr pointerMinAddr;
    const uint64_t pointerValueDistance;

    const ByteOrder byteOrder;

    enum AccessMapState
    {
        AM_INIT,
        AM_PREFETCH,
        AM_ACCESS,
        AM_SUCCESS,
        AM_INVALID
    };

    struct AccessMapEntry : public TaggedEntry
    {
        std::vector<AccessMapState> states;
        const unsigned initialQualityFactor;
        const unsigned pointerInitialValue;

        unsigned qualityFactor;
        unsigned pointerField;
        bool storeOnly;

        AccessMapEntry(size_t num_entries, unsigned initial_quality_factor,
                       unsigned pointer_initial_value);

        void invalidate() override;
        void resetMetadata();
    };

    struct PendingPointerValue
    {
        Addr valueBlock;
        Addr sourceZone;
        bool secure;
        uint64_t sequence;
    };

    AssociativeSet<AccessMapEntry> accessMapTable;
    std::vector<PendingPointerValue> pendingPointerValues;
    uint64_t pointerSequence;

    AccessMapEntry *findAccessMapEntry(Addr am_addr, bool is_secure) const;
    AccessMapEntry *getAccessMapEntry(Addr am_addr, bool is_secure);

    void setEntryState(AccessMapEntry &entry, Addr block, AccessMapState state,
                       bool demand_hit = false);
    bool isPatternAccess(AccessMapState state) const;
    bool mapIndexToAddress(Addr am_addr, bool is_secure, Addr index,
                           uint64_t lines_per_zone, AccessMapEntry *prev,
                           AccessMapEntry *curr, AccessMapEntry *next,
                           Addr &pf_addr, AccessMapEntry *&entry,
                           Addr &block) const;

    unsigned prefetchCost(const AccessMapEntry &entry) const;
    bool pointerActive(const AccessMapEntry &entry) const;
    bool qualityFactorBypassed(const AccessMapEntry &entry) const;
    bool consumeTokensForPrefetch(AccessMapEntry &entry);
    void addQualityTokens(AccessMapEntry &entry, unsigned tokens);
    void subtractQualityTokens(AccessMapEntry &entry, unsigned tokens);

    void updateAccessMetadata(const PrefetchInfo &pfi, AccessMapEntry &entry,
                              Addr am_addr);
    void consumeMatchingPointerValue(Addr addr, bool is_secure);
    bool looksLikePointerRead(const PrefetchInfo &pfi, Addr &value) const;
    bool recordPotentialPointerValue(const PrefetchInfo &pfi, Addr am_addr);
    void incrementPointerField(AccessMapEntry &entry);
    void decrementPointerField(AccessMapEntry &entry);

  public:
    AppleAMPM(const AppleAMPMPrefetcherParams &p);
    ~AppleAMPM() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_APPLE_AMPM_HH__
