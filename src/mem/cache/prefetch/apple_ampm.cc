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

#include "mem/cache/prefetch/apple_ampm.hh"

#include <algorithm>
#include <cstdlib>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/AppleAMPMPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

AppleAMPM::AccessMapEntry::AccessMapEntry(size_t num_entries,
                                          unsigned initial_quality_factor,
                                          unsigned pointer_initial_value)
    : TaggedEntry(),
      states(num_entries, AM_INIT),
      initialQualityFactor(initial_quality_factor),
      pointerInitialValue(pointer_initial_value),
      qualityFactor(initial_quality_factor),
      pointerField(pointer_initial_value),
      storeOnly(false)
{
}

void
AppleAMPM::AccessMapEntry::resetMetadata()
{
    qualityFactor = initialQualityFactor;
    pointerField = pointerInitialValue;
    storeOnly = false;
}

void
AppleAMPM::AccessMapEntry::invalidate()
{
    TaggedEntry::invalidate();
    for (auto &entry : states) {
        entry = AM_INIT;
    }
    resetMetadata();
}

AppleAMPM::AppleAMPM(const AppleAMPMPrefetcherParams &p)
    : Queued(p),
      limitStride(p.limit_stride),
      degree(p.degree),
      hotZoneSize(p.hot_zone_size),
      initialQualityFactor(p.initial_quality_factor),
      maxQualityFactor(p.max_quality_factor),
      prefetchTokenCost(p.prefetch_token_cost),
      storeOnlyPrefetchTokenCost(p.store_only_prefetch_token_cost),
      successfulPrefetchTokens(p.successful_prefetch_tokens),
      cacheHitPenaltyTokens(p.cache_hit_penalty_tokens),
      pointerPrefetchTokens(p.pointer_prefetch_tokens),
      qualityFactorBypassAccesses(p.quality_factor_bypass_accesses),
      usePointerValueHeuristic(p.use_pointer_value_heuristic),
      pointerFieldMax(p.pointer_field_max),
      pointerInitialValue(p.pointer_initial_value),
      pointerIncrement(p.pointer_increment),
      pointerDecrement(p.pointer_decrement),
      pointerThreshold(p.pointer_threshold),
      pointerTrackingEntries(p.pointer_tracking_entries),
      pointerTrackingWindow(p.pointer_tracking_window),
      pointerMinAddr(p.pointer_min_addr),
      pointerValueDistance(p.pointer_value_distance),
      byteOrder(p.sys->getGuestByteOrder()),
      accessMapTable(
          p.access_map_table_assoc, p.access_map_table_entries,
          p.access_map_table_indexing_policy,
          p.access_map_table_replacement_policy,
          AccessMapEntry(hotZoneSize / blkSize, initialQualityFactor,
                         pointerInitialValue)),
      pointerSequence(0)
{
    fatal_if(!isPowerOf2(hotZoneSize),
             "the AppleAMPM hot zone size must be a power of 2");
    fatal_if(hotZoneSize < blkSize,
             "the AppleAMPM hot zone size must be at least one cache line");
    fatal_if(initialQualityFactor > maxQualityFactor,
             "initial_quality_factor must be <= max_quality_factor");
    fatal_if(pointerInitialValue > pointerFieldMax,
             "pointer_initial_value must be <= pointer_field_max");
}

AppleAMPM::AccessMapEntry *
AppleAMPM::findAccessMapEntry(Addr am_addr, bool is_secure) const
{
    return accessMapTable.findEntry(am_addr, is_secure);
}

AppleAMPM::AccessMapEntry *
AppleAMPM::getAccessMapEntry(Addr am_addr, bool is_secure)
{
    AccessMapEntry *am_entry = accessMapTable.findEntry(am_addr, is_secure);
    if (am_entry != nullptr) {
        accessMapTable.accessEntry(am_entry);
    } else {
        am_entry = accessMapTable.findVictim(am_addr);
        assert(am_entry != nullptr);
        accessMapTable.insertEntry(am_addr, is_secure, am_entry);
    }
    return am_entry;
}

void
AppleAMPM::addQualityTokens(AccessMapEntry &entry, unsigned tokens)
{
    entry.qualityFactor =
        std::min(maxQualityFactor, entry.qualityFactor + tokens);
}

void
AppleAMPM::subtractQualityTokens(AccessMapEntry &entry, unsigned tokens)
{
    entry.qualityFactor =
        tokens > entry.qualityFactor ? 0 : entry.qualityFactor - tokens;
}

void
AppleAMPM::setEntryState(AccessMapEntry &entry, Addr block,
                         AccessMapState state, bool demand_hit)
{
    AccessMapState old = entry.states[block];

    if (old == AM_PREFETCH && state == AM_ACCESS) {
        entry.states[block] = demand_hit ? AM_SUCCESS : AM_ACCESS;
        if (demand_hit) {
            addQualityTokens(entry, successfulPrefetchTokens);
        }
        return;
    }

    if (old == AM_SUCCESS && state == AM_ACCESS) {
        return;
    }

    entry.states[block] = state;
}

bool
AppleAMPM::isPatternAccess(AccessMapState state) const
{
    return state == AM_ACCESS || state == AM_SUCCESS;
}

bool
AppleAMPM::mapIndexToAddress(Addr am_addr, bool is_secure, Addr index,
                             uint64_t lines_per_zone, AccessMapEntry *prev,
                             AccessMapEntry *curr, AccessMapEntry *next,
                             Addr &pf_addr, AccessMapEntry *&entry,
                             Addr &block) const
{
    Addr zone = am_addr;
    entry = nullptr;

    if (index < lines_per_zone) {
        if (am_addr == 0 || prev == nullptr) {
            return false;
        }
        zone = am_addr - 1;
        entry = prev;
        block = index;
    } else if (index < 2 * lines_per_zone) {
        entry = curr;
        block = index - lines_per_zone;
    } else if (index < 3 * lines_per_zone) {
        if (next == nullptr) {
            return false;
        }
        zone = am_addr + 1;
        entry = next;
        block = index - 2 * lines_per_zone;
    } else {
        return false;
    }

    pf_addr = zone * hotZoneSize + block * blkSize;
    return entry != nullptr && entry->isValid() &&
           entry->isSecure() == is_secure;
}

unsigned
AppleAMPM::prefetchCost(const AccessMapEntry &entry) const
{
    return entry.storeOnly ? storeOnlyPrefetchTokenCost : prefetchTokenCost;
}

bool
AppleAMPM::pointerActive(const AccessMapEntry &entry) const
{
    return entry.pointerField >= pointerThreshold;
}

bool
AppleAMPM::qualityFactorBypassed(const AccessMapEntry &entry) const
{
    if (qualityFactorBypassAccesses == 0) {
        return false;
    }

    unsigned accesses = 0;
    for (auto state : entry.states) {
        if (isPatternAccess(state)) {
            accesses++;
        }
    }
    return accesses >= qualityFactorBypassAccesses;
}

bool
AppleAMPM::consumeTokensForPrefetch(AccessMapEntry &entry)
{
    if (qualityFactorBypassed(entry)) {
        return true;
    }

    if (pointerActive(entry)) {
        addQualityTokens(entry, pointerPrefetchTokens);
    }

    const unsigned cost = prefetchCost(entry);
    if (entry.qualityFactor < cost) {
        return false;
    }

    subtractQualityTokens(entry, cost);
    return true;
}

void
AppleAMPM::incrementPointerField(AccessMapEntry &entry)
{
    entry.pointerField =
        std::min(pointerFieldMax, entry.pointerField + pointerIncrement);
}

void
AppleAMPM::decrementPointerField(AccessMapEntry &entry)
{
    entry.pointerField = pointerDecrement > entry.pointerField
                             ? 0
                             : entry.pointerField - pointerDecrement;
}

void
AppleAMPM::consumeMatchingPointerValue(Addr addr, bool is_secure)
{
    const Addr value_block = blockAddress(addr);

    for (auto it = pendingPointerValues.begin();
         it != pendingPointerValues.end();) {
        const bool expired =
            pointerSequence - it->sequence > pointerTrackingWindow;
        if (expired) {
            AccessMapEntry *source =
                findAccessMapEntry(it->sourceZone, it->secure);
            if (source != nullptr) {
                decrementPointerField(*source);
            }
            it = pendingPointerValues.erase(it);
            continue;
        }

        if (it->secure == is_secure && it->valueBlock == value_block) {
            AccessMapEntry *source =
                findAccessMapEntry(it->sourceZone, is_secure);
            if (source != nullptr) {
                incrementPointerField(*source);
            }
            it = pendingPointerValues.erase(it);
        } else {
            ++it;
        }
    }
}

bool
AppleAMPM::looksLikePointerRead(const PrefetchInfo &pfi, Addr &value) const
{
    value = 0;

    if (!usePointerValueHeuristic || pfi.isWrite() || pfi.isCacheMiss() ||
        pfi.getDataPtr() == nullptr || pfi.getSize() > sizeof(uint64_t)) {
        return false;
    }

    switch (pfi.getSize()) {
        case sizeof(uint8_t):
            value = pfi.get<uint8_t>(byteOrder);
            break;
        case sizeof(uint16_t):
            value = pfi.get<uint16_t>(byteOrder);
            break;
        case sizeof(uint32_t):
            value = pfi.get<uint32_t>(byteOrder);
            break;
        case sizeof(uint64_t):
            value = pfi.get<uint64_t>(byteOrder);
            break;
        default:
            return false;
    }

    if (value < pointerMinAddr) {
        return false;
    }

    if (pointerValueDistance != 0) {
        const Addr addr = pfi.getAddr();
        const uint64_t distance = value > addr ? value - addr : addr - value;
        if (distance > pointerValueDistance) {
            return false;
        }
    }

    return blockAddress(value) != blockAddress(pfi.getAddr());
}

bool
AppleAMPM::recordPotentialPointerValue(const PrefetchInfo &pfi, Addr am_addr)
{
    Addr value = 0;
    if (!looksLikePointerRead(pfi, value) || pointerTrackingEntries == 0) {
        return false;
    }

    PendingPointerValue pending{blockAddress(value), am_addr, pfi.isSecure(),
                                pointerSequence};

    pendingPointerValues.push_back(pending);
    while (pendingPointerValues.size() > pointerTrackingEntries) {
        AccessMapEntry *source =
            findAccessMapEntry(pendingPointerValues.front().sourceZone,
                               pendingPointerValues.front().secure);
        if (source != nullptr) {
            decrementPointerField(*source);
        }
        pendingPointerValues.erase(pendingPointerValues.begin());
    }

    return true;
}

void
AppleAMPM::updateAccessMetadata(const PrefetchInfo &pfi, AccessMapEntry &entry,
                                Addr am_addr)
{
    pointerSequence++;
    const bool empty_map =
        std::all_of(entry.states.begin(), entry.states.end(),
                    [](AccessMapState state) { return state == AM_INIT; });

    if (pfi.isWrite()) {
        if (empty_map) {
            entry.storeOnly = true;
        }
        return;
    }

    entry.storeOnly = false;
    consumeMatchingPointerValue(pfi.getAddr(), pfi.isSecure());
    if (!recordPotentialPointerValue(pfi, am_addr)) {
        decrementPointerField(entry);
    }
}

void
AppleAMPM::calculatePrefetch(const PrefetchInfo &pfi,
                             std::vector<AddrPriority> &addresses)
{
    assert(addresses.empty());

    const bool is_secure = pfi.isSecure();
    const Addr am_addr = pfi.getAddr() / hotZoneSize;
    const Addr current_block = (pfi.getAddr() % hotZoneSize) / blkSize;
    const uint64_t lines_per_zone = hotZoneSize / blkSize;

    AccessMapEntry *am_entry_curr = getAccessMapEntry(am_addr, is_secure);
    AccessMapEntry *am_entry_prev =
        (am_addr > 0) ? findAccessMapEntry(am_addr - 1, is_secure) : nullptr;
    AccessMapEntry *am_entry_next =
        (am_addr < (MaxAddr / hotZoneSize))
            ? findAccessMapEntry(am_addr + 1, is_secure)
            : nullptr;

    assert(am_entry_curr != nullptr);
    assert(am_entry_prev == nullptr || am_entry_curr != am_entry_prev);
    assert(am_entry_next == nullptr || am_entry_curr != am_entry_next);
    assert(am_entry_prev == nullptr || am_entry_next == nullptr ||
           am_entry_prev != am_entry_next);

    updateAccessMetadata(pfi, *am_entry_curr, am_addr);
    setEntryState(*am_entry_curr, current_block, AM_ACCESS,
                  !pfi.isCacheMiss());

    std::vector<AccessMapState> states(3 * lines_per_zone);
    for (unsigned idx = 0; idx < lines_per_zone; idx++) {
        states[idx] =
            am_entry_prev != nullptr ? am_entry_prev->states[idx] : AM_INVALID;
        states[idx + lines_per_zone] = am_entry_curr->states[idx];
        states[idx + 2 * lines_per_zone] =
            am_entry_next != nullptr ? am_entry_next->states[idx] : AM_INVALID;
    }

    const int states_current_block = current_block + lines_per_zone;
    const int max_stride =
        limitStride == 0 ? lines_per_zone / 2 : limitStride + 1;

    struct PatternOffset
    {
        int strideMultiplier;
        int lineBias;
    };

    struct PatternDesc
    {
        const char *name;
        const PatternOffset *required;
        unsigned requiredCount;
        const PatternOffset *prefetches;
        unsigned prefetchCount;
        bool usesStride;
        bool requireInterveningInit;
    };

    static constexpr PatternOffset forward4Req[] = {
        {0, 0}, {-1, 0}, {-2, 0}, {-3, 0}};
    static constexpr PatternOffset backward4Req[] = {
        {0, 0}, {1, 0}, {2, 0}, {3, 0}};
    static constexpr PatternOffset forward3Req[] = {{0, 0}, {-1, 0}, {-2, 0}};
    static constexpr PatternOffset backward3Req[] = {{0, 0}, {1, 0}, {2, 0}};
    static constexpr PatternOffset forwardWildcardReq[] = {
        {0, 0}, {-1, 0}, {-3, 0}};
    static constexpr PatternOffset backwardWildcardReq[] = {
        {0, 0}, {1, 0}, {3, 0}};
    static constexpr PatternOffset forwardAdjacentReq[] = {
        {0, 0}, {-1, 0}, {-2, -1}};
    static constexpr PatternOffset backwardAdjacentReq[] = {
        {0, 0}, {1, 0}, {2, 1}};
    static constexpr PatternOffset forwardIrregularReq[] = {
        {0, 0}, {0, -1}, {0, -3}, {0, -4}};
    static constexpr PatternOffset backwardIrregularReq[] = {
        {0, 0}, {0, 1}, {0, 3}, {0, 4}};
    static constexpr PatternOffset forwardPf3[] = {{1, 0}, {2, 0}, {3, 0}};
    static constexpr PatternOffset backwardPf3[] = {{-1, 0}, {-2, 0}, {-3, 0}};
    static constexpr PatternOffset forwardPf2[] = {{1, 0}, {2, 0}};
    static constexpr PatternOffset backwardPf2[] = {{-1, 0}, {-2, 0}};
    static constexpr PatternOffset forwardIrregularPf[] = {{0, 2}, {0, 5}};
    static constexpr PatternOffset backwardIrregularPf[] = {{0, -2}, {0, -5}};

    static constexpr PatternDesc patterns[] = {
        {"forward-long", forward4Req, 4, forwardPf3, 3, true, true},
        {"backward-long", backward4Req, 4, backwardPf3, 3, true, true},
        {"forward-irregular", forwardIrregularReq, 4, forwardIrregularPf, 2,
         false, false},
        {"backward-irregular", backwardIrregularReq, 4, backwardIrregularPf, 2,
         false, false},
        {"forward-dense", forward3Req, 3, forwardPf2, 2, true, true},
        {"backward-dense", backward3Req, 3, backwardPf2, 2, true, true},
        {"forward-wildcard", forwardWildcardReq, 3, forwardPf2, 2, true,
         false},
        {"backward-wildcard", backwardWildcardReq, 3, backwardPf2, 2, true,
         false},
        {"forward-adjacent", forwardAdjacentReq, 3, forwardPf2, 2, true,
         false},
        {"backward-adjacent", backwardAdjacentReq, 3, backwardPf2, 2, true,
         false},
    };

    for (const auto &pattern : patterns) {
        const int pattern_max_stride = pattern.usesStride ? max_stride : 2;
        for (int stride = 1; stride < pattern_max_stride; stride++) {
            auto indexFor = [states_current_block,
                             stride](const PatternOffset &offset) -> int {
                return states_current_block + offset.lineBias +
                       offset.strideMultiplier * stride;
            };

            bool matched = true;
            for (unsigned idx = 0; idx < pattern.requiredCount; idx++) {
                const int access_index = indexFor(pattern.required[idx]);
                if (access_index < 0 ||
                    access_index >= static_cast<int>(states.size()) ||
                    !isPatternAccess(states[access_index])) {
                    matched = false;
                    break;
                }
            }
            if (matched && pattern.requireInterveningInit && stride > 1) {
                for (unsigned idx = 1; idx < pattern.requiredCount; idx++) {
                    const int first = indexFor(pattern.required[idx - 1]);
                    const int second = indexFor(pattern.required[idx]);
                    const int start = std::min(first, second) + 1;
                    const int end = std::max(first, second);
                    for (int gap = start; gap < end; gap++) {
                        if (gap < 0 ||
                            gap >= static_cast<int>(states.size()) ||
                            states[gap] != AM_INIT) {
                            matched = false;
                            break;
                        }
                    }
                    if (!matched) {
                        break;
                    }
                }
            }
            if (!matched) {
                continue;
            }

            for (unsigned idx = 0; idx < pattern.prefetchCount; idx++) {
                const int target_index = indexFor(pattern.prefetches[idx]);
                if (target_index < 0 ||
                    target_index >= static_cast<int>(states.size()) ||
                    states[target_index] != AM_INIT) {
                    continue;
                }

                Addr pf_addr = 0;
                AccessMapEntry *pf_entry = nullptr;
                Addr pf_block = 0;

                if (!mapIndexToAddress(am_addr, is_secure, target_index,
                                       lines_per_zone, am_entry_prev,
                                       am_entry_curr, am_entry_next, pf_addr,
                                       pf_entry, pf_block)) {
                    continue;
                }

                if (!useVirtualAddresses && inCache(pf_addr, is_secure)) {
                    subtractQualityTokens(*pf_entry, cacheHitPenaltyTokens);
                    continue;
                }

                if (!consumeTokensForPrefetch(*pf_entry)) {
                    continue;
                }

                setEntryState(*pf_entry, pf_block, AM_PREFETCH);
                states[target_index] = AM_PREFETCH;
                addresses.push_back(AddrPriority(pf_addr, 0, PrefetchSourceType::AppleAMPM));
                DPRINTF(HWPrefetch,
                        "AppleAMPM generated %s prefetch addr=%#x qf=%u "
                        "ptr=%u store_only=%d\n",
                        pattern.name, pf_addr, pf_entry->qualityFactor,
                        pf_entry->pointerField, pf_entry->storeOnly);

                if (addresses.size() == degree) {
                    return;
                }
            }
        }
    }
}

} // namespace prefetch
} // namespace gem5
